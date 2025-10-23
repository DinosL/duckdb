#include "duckdb/optimizer/window_rewriter.hpp"




namespace duckdb {

WindowRewriter::WindowRewriter(Optimizer &optimizer) : optimizer(optimizer) {

}

bool WindowRewriter::CanOptimize(LogicalOperator &op) {
	// if (op.type != LogicalOperatorType::LOGICAL_WINDOW) {
	// 	return false;
	// }
	// auto &window = op.Cast<LogicalWindow>();
	//
	// // We can only optimize if there is exactly one expression: row_number() (at least for now)
	// if (window.expressions.size() != 1) {
	// 	return false;
	// }
	//
	// auto &expression = window.expressions[0];
	// if (expression->type != ExpressionType::WINDOW_ROW_NUMBER) {
	// 	return false;
	// }

	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION && op.children[0]->type == LogicalOperatorType::LOGICAL_WINDOW) {
		auto *child = op.children[0].get();
		auto &window = child->Cast<LogicalWindow>();
		auto &expression = window.expressions[0];
		if (expression->type != ExpressionType::WINDOW_ROW_NUMBER) {
			return false;
		}
		return true;
	}

	return false;

}


unique_ptr<LogicalOperator> WindowRewriter::Optimize(unique_ptr<LogicalOperator> op) {

	ColumnBindingReplacer replacer;
	op = OptimizeInternal(std::move(op), replacer);

	if (!replacer.replacement_bindings.empty()) {
		replacer.VisitOperator(*op);
	}

	return op;
}

unique_ptr<LogicalOperator> WindowRewriter::OptimizeInternal(
    unique_ptr<LogicalOperator> op, ColumnBindingReplacer &replacer) {

    if (CanOptimize(*op)) {
        auto &proj = op->Cast<LogicalProjection>();
        auto &window = proj.children[0]->Cast<LogicalWindow>();

    	// Only support ROW_NUMBER() OVER () with no PARTITION or ORDER BY
    	if (window.expressions.size() != 1) {
    		return op;
    	}

    	auto &child = window.children[0];
        const auto child_bindings = child->GetColumnBindings();
        const auto child_types = child->types;

        // Save old bindings
        const auto old_window_bindings = proj.GetColumnBindings();

        vector<unique_ptr<Expression>> expressions;
    	auto row_number_index = child_types.size();
        expressions.reserve(row_number_index + 1);

        for (idx_t i = 0; i < row_number_index; ++i) {
            expressions.push_back(make_uniq<BoundColumnRefExpression>(child_types[i], child_bindings[i]));
        }

        // Replace ROW_NUMBER() with row_number virtual column reference
        const auto new_proj_index = optimizer.binder.GenerateTableIndex();
    	//FIXME don't assume the row_number is always in the last column
        expressions.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, row_number_index));

        // Create new projection that replaces both the WINDOW and PROJECTION
        auto new_projection = make_uniq<LogicalProjection>(new_proj_index, std::move(expressions));
        new_projection->children.push_back(std::move(child));

        // Map old bindings to new projection bindings
        const auto new_projection_bindings = new_projection->GetColumnBindings();
        for (idx_t i = 0; i < old_window_bindings.size() && i < new_projection_bindings.size(); i++) {
            replacer.replacement_bindings.emplace_back(old_window_bindings[i], new_projection_bindings[i]);
        }

        replacer.stop_operator = new_projection.get();

        return std::move(new_projection);
    }

    // Recurse into children
    for (auto &child : op->children) {
        child = OptimizeInternal(std::move(child), replacer);
    }
    return op;
}


}