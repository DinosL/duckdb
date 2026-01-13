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

	switch (op.type) {
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		return false;
	default:
		break;
	}

	// return true;

	if (op.children.size()  == 2) {
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
	}

	return true;
}

unique_ptr<LogicalOperator> ProjectionPullup::Optimize(unique_ptr<LogicalOperator> op) {
	auto root = op.get();
	ColumnBindingReplacer replacer;
	op = RewritePlan(std::move(op), root, replacer, true);

	if (!replacer.replacement_bindings.empty() ) {
		replacer.VisitOperator(*op);
	}

	return op;
}

unique_ptr<LogicalOperator> ProjectionPullup::RewritePlan(unique_ptr<LogicalOperator> op, LogicalOperator *root, ColumnBindingReplacer &replacer, bool is_root) {
	// first Recurse into children and pull up projections as far as they can go
	for (auto &child : op->children) {
		child = RewritePlan(std::move(child), root, replacer, false);
	}

	if (CanOptimize(*op)) {
		// ColumnBindingReplacer replacer;
		op =  Pullup(std::move(op), replacer, root);
		// if (!replacer.replacement_bindings.empty() ) {
		// 	replacer.VisitOperator(*op);
		// }
		return op;
	}

	// return std::move(op->children[0]);
	return op;
 }

unique_ptr<LogicalOperator> ProjectionPullup::Pullup(unique_ptr<LogicalOperator> op, ColumnBindingReplacer &new_replacer, LogicalOperator *root) {


	switch (op->type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			//TODO: It only has one child. Fix
			for (auto &child : op->children) {
				if (child->type == LogicalOperatorType::LOGICAL_PROJECTION) {
					auto &parent_proj = op->Cast<LogicalProjection>();
					auto &child_proj = child->Cast<LogicalProjection>();

					// TODO: this rule applies to all the operators. Maybe move to CanOptimize
					for (auto &expr : child_proj.expressions) {
						if (expr->type == ExpressionType::BOUND_FUNCTION) {
							return  op;
						}
					}

					vector<unique_ptr<Expression>> new_parent_expressions;

					for (auto &expr : parent_proj.expressions) {
						// if (expr->type == ExpressionType::OPERATOR_CAST) {
						// 	return op;
						// }
						// Case 1: parent expression is a column reference
						if (expr->type == ExpressionType::BOUND_COLUMN_REF) {
							auto &parent_colref = expr->Cast<BoundColumnRefExpression>();

							idx_t child_idx = parent_colref.binding.column_index;

							// Replace parent expression with child expression
							expr = child_proj.expressions[child_idx]->Copy();
							continue;
						}

						// Case 2: parent expression is a bound function
						ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
							if (child->type != ExpressionType::BOUND_COLUMN_REF) {
								return;
							}

							auto &colref = child->Cast<BoundColumnRefExpression>();
							idx_t child_idx = colref.binding.column_index;

							// Replace the column ref with the child expression
							child = child_proj.expressions[child_idx]->Copy();
						});
					}

					// Remove the child projection
					parent_proj.children[0] = std::move(child_proj.children[0]);
				}
			}
			break;
		}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op->Cast<LogicalJoin>();

		bool is_left_proj = (join.children[0]->type == LogicalOperatorType::LOGICAL_PROJECTION);
		bool is_right_proj = (join.children[1]->type == LogicalOperatorType::LOGICAL_PROJECTION);

		// vector<unique_ptr<Expression>> merged_expressions;
		vector<ColumnBinding> new_bindings;
		vector<ColumnBinding> old_bindings;

		if (is_left_proj) {
			auto &left_proj = join.children[0]->Cast<LogicalProjection>();
			for (auto &expr : left_proj.expressions) {
				// TODO: Check join expression and only skip if expression in join condition is bound_func
				if (expr->type == ExpressionType::BOUND_FUNCTION || expr->type == ExpressionType::CASE_EXPR)
					return op;
			}

			// After removing the projection we need to make sure that the join operator points to whatever the projection was pointing at
			for (auto &child : left_proj.children) {
				auto child_bindings = child->GetColumnBindings();
				for (auto &binding : child_bindings) {
					// Printer::PrintF("Left Bind %d,%d", binding.table_index, binding.column_index);
					new_bindings.push_back(binding);
				}
			}

			auto left_bindings = left_proj.GetColumnBindings();
			// for (auto &binding : left_bindings) {
			// 	Printer::PrintF("Left Bind %d,%d", binding.table_index, binding.column_index);
			// }
			old_bindings.insert(old_bindings.end(), left_bindings.begin(), left_bindings.end());

			// Extract the child and replace join's inputs with the underlying operators
			auto left_child = std::move(left_proj.children[0]);
			join.children[0] = std::move(left_child);
		}
		else {
			// No projection to pullup, just pass the bindings to the new projection
			auto &left_child = *join.children[0];
			auto left_bindings = left_child.GetColumnBindings();

			for (auto &b : left_bindings) {
				Printer::PrintF("Left bind %d, %d", b.table_index, b.column_index);
				new_bindings.push_back(b);
				old_bindings.push_back(b);
			}
		}

		if (is_right_proj) {
			auto &right_proj = join.children[1]->Cast<LogicalProjection>();
			for (auto &expr : right_proj.expressions) {
				if (expr->type == ExpressionType::BOUND_FUNCTION || expr->type == ExpressionType::CASE_EXPR) {
					return op;
				}
			}

			// After removing the projection we need to make sure that the join operator points to whatever the projection was pointing at
			//TODO: one child here as well. Simplify
			for (auto &child : right_proj.children) {
				auto child_bindings = child->GetColumnBindings();
				auto sz = child_bindings.size();
				for (auto &binding : child_bindings) {
					Printer::PrintF("Right Bind %d,%d", binding.table_index, binding.column_index);
					new_bindings.push_back(binding);
				}
			}

			auto right_bindings = right_proj.GetColumnBindings();
			old_bindings.insert(old_bindings.end(), right_bindings.begin(), right_bindings.end());

			// Extract the child and replace join's inputs with the underlying operators
			auto right_child = std::move(right_proj.children[0]);
			join.children[1] = std::move(right_child);
		}
		else {
			// No projection to pullup, just pass the bindings to the new projection
			auto &right_child = *join.children[1];
			auto right_bindings = right_child.GetColumnBindings();

			for (auto &b : right_bindings) {
				new_bindings.push_back(b);
				old_bindings.push_back(b);
			}
		}

		D_ASSERT(old_bindings.size() == new_bindings.size());
		for (idx_t i = 0; i < old_bindings.size(); i++) {
			new_replacer.replacement_bindings.emplace_back(old_bindings[i], new_bindings[i]);
			// Printer::PrintF("Replacing %d, %d with %d, %d", old_bindings[i].table_index, old_bindings[i].column_index, new_bindings[i].table_index, new_bindings[i].column_index);
		}

		auto &right_inner_join = join.children[1]->Cast<LogicalJoin>();
		new_replacer.stop_operator = &right_inner_join;

		return op;

		}
	default: break;
	}

	return  op;
}

} // namespace duckdb