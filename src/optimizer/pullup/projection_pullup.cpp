#include "duckdb/optimizer/projection_pullup.hpp"

#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"

namespace duckdb {

ProjectionPullup::ProjectionPullup(Optimizer &optimizer_p) : optimizer(optimizer_p) {
}

bool ProjectionPullup::CanOptimize(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_JOIN:
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
	case LogicalOperatorType::LOGICAL_POSITIONAL_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN: {
		auto *left = op.children[0].get();
		auto *right = op.children[1].get();

		if (left->type != LogicalOperatorType::LOGICAL_PROJECTION)
			return false;

		if (right->type != LogicalOperatorType::LOGICAL_PROJECTION)
			return false;

		return true;
	}
	default:
		return false;
	}

	return true;
}

unique_ptr<LogicalOperator> ProjectionPullup::Optimize(unique_ptr<LogicalOperator> op) {
	LogicalOperator *root = op.get();
	op = Pullup(std::move(op));

	if (!replacer.replacement_bindings.empty()) {
		replacer.VisitOperator(*root);
	}

	return op;
}

unique_ptr<LogicalOperator> ProjectionPullup::Pullup(unique_ptr<LogicalOperator> op) {
	if (CanOptimize(*op)) {
		auto &join = op->Cast<LogicalJoin>();

		auto &left_proj = join.children[0]->Cast<LogicalProjection>();
		auto &right_proj = join.children[1]->Cast<LogicalProjection>();

		// Extract children
		unique_ptr<LogicalOperator> left_child = std::move(left_proj.children[0]);
		unique_ptr<LogicalOperator> right_child = std::move(right_proj.children[0]);

		// Replace join's inputs with the underlying operators
		join.children[0] = std::move(left_child);
		join.children[1] = std::move(right_child);

		// Build projection list from the projections above the joins
		vector<unique_ptr<Expression>> merged_expressions;

		for (auto &expr : left_proj.expressions) {
			Printer::PrintF("Left expressions - ", expr->ToString());
			merged_expressions.push_back(expr->Copy());
		}
		for (auto &expr : right_proj.expressions) {
			Printer::PrintF("Right expressions - ", expr->ToString());
			merged_expressions.push_back(expr->Copy());
		}

		// TODO: Verify empty expression list means pass-through projection
		// Create a new projection to replace the two projections above the joins
		auto new_projection = make_uniq<LogicalProjection>(DConstants::INVALID_INDEX, std::move(merged_expressions));
		new_projection->children.push_back(std::move(op));

		// return (std::move(op));
		return new_projection;
		// op = std::move(new_projection);
	}

	// Recurse into children
	for (auto &child : op->children) {
		child = Pullup(std::move(child));
	}
	return op;
}

} // namespace duckdb