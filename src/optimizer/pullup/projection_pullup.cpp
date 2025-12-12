#include "duckdb/optimizer/projection_pullup.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

ProjectionPullup::ProjectionPullup(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

bool ProjectionPullup::CanOptimize(LogicalOperator &op) {
	//TODO what about filters?
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN: {
		break;
	}
	default:
		// Windows, Aggregates...
		return false;
	}

	bool is_left_proj = (op.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
	bool is_right_proj = (op.children[1]->type == LogicalOperatorType::LOGICAL_PROJECTION);

	// If neither side is a projection, nothing to pull up.
	if (!is_left_proj && !is_right_proj) {
		return false;
	}

	//Ensure that projected expressions are not needed by the join conditions
	if (is_left_proj) {
		auto &left_proj = op.children[0]->Cast<LogicalProjection>();
		for (auto &expr : left_proj.expressions) {
			//TODO: Check if that's enough
			if (expr->type == ExpressionType::BOUND_FUNCTION)
				return false;
		}
	}

	if (is_right_proj) {
		auto &right_proj = op.children[1]->Cast<LogicalProjection>();
		for (auto &expr : right_proj.expressions) {
			if (expr->type == ExpressionType::BOUND_FUNCTION)
				return false;
		}
	}


	return true;
}

unique_ptr<LogicalOperator> ProjectionPullup::Optimize(unique_ptr<LogicalOperator> op) {
	LogicalOperator *root = op.get();
	ColumnBindingReplacer replacer;
	op = RewritePlan(std::move(op), replacer, root);

	if (!replacer.replacement_bindings.empty()) {
		replacer.VisitOperator(*op);
	}

	return op;
}

unique_ptr<LogicalOperator> ProjectionPullup::RewritePlan(unique_ptr<LogicalOperator> op, ColumnBindingReplacer &replacer, LogicalOperator *root) {
	if (CanOptimize(*op)) {
		return Pullup(std::move(op), replacer, root);
	}

	// Recurse into children
	for (auto &child : op->children) {
		child = RewritePlan(std::move(child), replacer, root);
	}
	// return std::move(op->children[0]);
	return op;
}

unique_ptr<LogicalOperator> ProjectionPullup::Pullup(unique_ptr<LogicalOperator> op, ColumnBindingReplacer &new_replacer, LogicalOperator *root) {
	auto &join = op->Cast<LogicalJoin>();

	bool is_left_proj = (join.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
	bool is_right_proj = (join.children[1]->type == LogicalOperatorType::LOGICAL_PROJECTION);

	vector<unique_ptr<Expression>> merged_expressions;
	vector<ColumnBinding> new_bindings;
	vector<ColumnBinding> old_bindings;

	if (is_left_proj) {
		auto &left_proj = join.children[0]->Cast<LogicalProjection>();

		// After removing the projection we need to make sure that the join operator points to whatever the projection was pointing at
		for (auto &child : left_proj.children) {
			auto child_bindings = child->GetColumnBindings();
			for (auto &binding : child_bindings) {
				Printer::PrintF("Left Bind %d,%d", binding.table_index, binding.column_index);
				new_bindings.push_back(binding);
			}
		}

		auto left_bindings = left_proj.GetColumnBindings();
		old_bindings.insert(old_bindings.end(), left_bindings.begin(), left_bindings.end());

		// Build projection expression list from the projections above the join
		for (auto &expr : left_proj.expressions) {
			merged_expressions.push_back(expr->Copy());
		}

		// Extract the child and replace join's inputs with the underlying operators
		auto left_child = std::move(left_proj.children[0]);
		join.children[0] = std::move(left_child);
	}
	else {
		// No projection to pullup, just pass the bindings to the new projection
		auto &left_child = *join.children[0];
		auto left_bindings = left_child.GetColumnBindings();

		for (auto &b : left_bindings) {
			LogicalType type = left_child.types[b.column_index];
			merged_expressions.push_back(make_uniq<BoundColumnRefExpression>(type, b));
			new_bindings.push_back(b);
			old_bindings.push_back(b);
		}
	}

	if (is_right_proj) {
		auto &right_proj = join.children[1]->Cast<LogicalProjection>();

		// After removing the projection we need to make sure that the join operator points to whatever the projection was pointing at
		for (auto &child : right_proj.children) {
			auto child_bindings = child->GetColumnBindings();
			for (auto &binding : child_bindings) {
				Printer::PrintF("Right Bind %d,%d", binding.table_index, binding.column_index);
				new_bindings.push_back(binding);
			}
		}

		auto right_bindings = right_proj.GetColumnBindings();
		old_bindings.insert(old_bindings.end(), right_bindings.begin(), right_bindings.end());

		// Build projection expression list from the projections above the join
		for (auto &expr : right_proj.expressions) {
			merged_expressions.push_back(expr->Copy());
		}

		// Extract the child and replace join's inputs with the underlying operators
		auto right_child = std::move(right_proj.children[0]);
		join.children[1] = std::move(right_child);
	}
	else {
		// No projection to pullup, just pass the bindings to the new projection
		auto &right_child = *join.children[1];
		auto right_bindings = right_child.GetColumnBindings();

		for (auto &b : right_bindings) {
			LogicalType type = right_child.types[b.column_index];
			merged_expressions.push_back(make_uniq<BoundColumnRefExpression>(type, b));
			new_bindings.push_back(b);
			old_bindings.push_back(b);
		}
	}

	// Create a new projection to replace the two projections above the joins
	idx_t new_proj_index = optimizer.binder.GenerateTableIndex();
	auto new_projection = make_uniq<LogicalProjection>(new_proj_index, std::move(merged_expressions));
	new_projection->children.push_back(std::move(op));
	auto new_proj_bindings = new_projection->GetColumnBindings();

	// We need to replace the bindings until the level of the joins
	ColumnBindingReplacer replacer;
	D_ASSERT(old_bindings.size() == new_bindings.size());
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		replacer.replacement_bindings.emplace_back(old_bindings[i], new_bindings[i]);
		// Printer::PrintF("Replacing %d, %d with %d, %d", old_bindings[i].table_index, old_bindings[i].column_index, new_bindings[i].table_index, new_bindings[i].column_index);
	}

	// And replace the bindings of the projection above the one we just added
	D_ASSERT(old_bindings.size() == new_proj_bindings.size());
	for (idx_t i = 0; i < old_bindings.size(); i++) {
		new_replacer.replacement_bindings.emplace_back(old_bindings[i], new_proj_bindings[i]);
		// Printer::PrintF("New Replacer %d, %d with %d, %d", old_bindings[i].table_index, old_bindings[i].column_index, new_proj_bindings[i].table_index, new_proj_bindings[i].column_index);
	}
	new_replacer.stop_operator = new_projection.get();

	auto &right_inner_join = join.children[1]->Cast<LogicalJoin>();
	replacer.stop_operator = &right_inner_join;
	replacer.VisitOperator(*new_projection);


	return new_projection;
}

} // namespace duckdb