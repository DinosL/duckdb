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
	op = RewritePlan(std::move(op), root, true);

	// if (!replacer.replacement_bindings.empty() ) {
	// 	replacer.VisitOperator(*op);
	// }

	return op;
}

unique_ptr<LogicalOperator> ProjectionPullup::RewritePlan(unique_ptr<LogicalOperator> op, LogicalOperator *root, bool is_root) {
	// first Recurse into children and pull up projections as far as they can go
	for (auto &child : op->children) {
		child = RewritePlan(std::move(child), root, false);
	}

	if (CanOptimize(*op)) {
		ColumnBindingReplacer replacer;
		op =  Pullup(std::move(op), replacer, root);
		if (!replacer.replacement_bindings.empty() ) {
			replacer.VisitOperator(*op);
		}
		return op;
	}

	// return std::move(op->children[0]);
	return op;
 }

unique_ptr<LogicalOperator> ProjectionPullup::Pullup(unique_ptr<LogicalOperator> op, ColumnBindingReplacer &new_replacer, LogicalOperator *root) {


	switch (op->type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
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

					for (idx_t i = 0; i < parent_proj.expressions.size(); i++) {
						auto &parent_expr = parent_proj.expressions[i];


						Expression *child_expr = nullptr;
						if (parent_expr->type == ExpressionType::BOUND_FUNCTION) {
							auto &parent_bound_func = parent_expr->Cast<BoundFunctionExpression>();
							column_binding_set_t expr_bindings;
							ExpressionIterator::EnumerateChildren(parent_bound_func, [&](unique_ptr<Expression> &child) {
								if (child->type == ExpressionType::BOUND_COLUMN_REF) {
									auto &bound_ref = child->Cast<BoundColumnRefExpression>();
									expr_bindings.insert(bound_ref.binding);
								}
							});
							for (auto &item : parent_bound_func.children) {
								if (item->type == ExpressionType::BOUND_COLUMN_REF) {
									auto &parent_bound_col_ref = item->Cast<BoundColumnRefExpression>();
									for (auto &expr : child_proj.expressions) {
										auto &child_bound_col_ref = expr->Cast<BoundColumnRefExpression>();
										if (child_bound_col_ref.binding.column_index == parent_bound_col_ref.binding.column_index) {
											child_expr = expr.get();
											//TODO: I should probably break the outer loop as well
											break;
										}
									}
								}
							}
						}
						else if (parent_expr->type == ExpressionType::BOUND_COLUMN_REF) {
							auto &parent_bound_col_ref = parent_expr->Cast<BoundColumnRefExpression>();
							for (auto &expr : child_proj.expressions) {
								auto &child_bound_col_ref = expr->Cast<BoundColumnRefExpression>();
								if (child_bound_col_ref.binding.column_index == parent_bound_col_ref.binding.column_index) {
									child_expr = expr.get();
									break;
								}
							}
						}


						D_ASSERT(child_expr != nullptr);



						unique_ptr<Expression> merged_expressions;

						// Case 1: Parent is BoundColumnRef
						if (parent_expr->type == ExpressionType::BOUND_COLUMN_REF) {
							//
							merged_expressions = child_expr->Copy();
						}
						else if (parent_expr->type == ExpressionType::BOUND_FUNCTION && child_expr->type == ExpressionType::BOUND_COLUMN_REF) {
							// Case 2: Parent is BoundFunction, Child is BoundColumnRef

							merged_expressions = parent_expr->Copy();
							auto &child_col = child_expr->Cast<BoundColumnRefExpression>();

							ColumnBindingReplacer replacer;
							replacer.replacement_bindings.emplace_back(parent_expr->Cast<BoundFunctionExpression>().children[0]->Cast<BoundColumnRefExpression>().binding,child_col.binding);
							// Printer::PrintF("Case 2 replacer %d, %d with %d, %d", parent_expr->Cast<BoundFunctionExpression>().children[0]->Cast<BoundColumnRefExpression>().binding.table_index,parent_expr->Cast<BoundFunctionExpression>().children[0]->Cast<BoundColumnRefExpression>().binding.column_index, child_col.binding.table_index, child_col.binding.column_index);
							// replacer.VisitExpression(*merged_expressions);
						}
						// Case 3: Parent is BoundFunction, Child is BoundFunction - just skip for now
						else if (parent_expr->type == ExpressionType::BOUND_FUNCTION &&
								 child_expr->type == ExpressionType::BOUND_FUNCTION) {
							return op;
								 }
						else {
							return op;
						}

						new_parent_expressions.push_back(std::move(merged_expressions));

					}

					// Replace parent expressions
					parent_proj.expressions = std::move(new_parent_expressions);

					// and fix column bindings
					ColumnBindingReplacer replacer;
					auto old_bindings = child_proj.GetColumnBindings();
					auto new_bindings = child_proj.children[0]->GetColumnBindings();
					for (idx_t i = 0; i < old_bindings.size(); i++) {
						replacer.replacement_bindings.emplace_back(old_bindings[i], new_bindings[i]);
						// Printer::PrintF("Replacer %d, %d with %d, %d", old_bindings[i].table_index, old_bindings[i].column_index, new_bindings[i].table_index, new_bindings[i].column_index);
					}
					replacer.VisitOperator(parent_proj);

					// If this child projection was a stop operator, update the new stop operator
					if (new_replacer.stop_operator == child_proj) {
						new_replacer.stop_operator = parent_proj;
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

		vector<unique_ptr<Expression>> merged_expressions;
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
			for (auto &binding : left_bindings) {
				Printer::PrintF("Left Bind %d,%d", binding.table_index, binding.column_index);
			}
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
			for (auto &expr : right_proj.expressions) {
				if (expr->type == ExpressionType::BOUND_FUNCTION || expr->type == ExpressionType::CASE_EXPR) {
					return op;
				}
			}

			// After removing the projection we need to make sure that the join operator points to whatever the projection was pointing at
			for (auto &child : right_proj.children) {
				auto child_bindings = child->GetColumnBindings();
				for (auto &binding : child_bindings) {
					// Printer::PrintF("Right Bind %d,%d", binding.table_index, binding.column_index);
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
			for (auto &binding : old_bindings) {
				Printer::PrintF("Old bindings: %d, %d", binding.table_index, binding.column_index);

			}

			for (auto &binding : new_bindings) {
				Printer::PrintF("NEW bindings: %d, %d", binding.table_index, binding.column_index);

			}


		// Create a new projection to replace the two projections above the joins
		idx_t new_proj_index = optimizer.binder.GenerateTableIndex();
		auto new_projection = make_uniq<LogicalProjection>(new_proj_index, std::move(merged_expressions));
		// new_projection->children.push_back(std::move(op));
		auto new_proj_bindings = new_projection->GetColumnBindings();
		// We need to replace the bindings until the level of the joins
		ColumnBindingReplacer replacer;
		D_ASSERT(old_bindings.size() == new_bindings.size());
		for (idx_t i = 0; i < old_bindings.size(); i++) {
			replacer.replacement_bindings.emplace_back(old_bindings[i], new_bindings[i]);
			Printer::PrintF("Replacing %d, %d with %d, %d", old_bindings[i].table_index, old_bindings[i].column_index, new_bindings[i].table_index, new_bindings[i].column_index);
		}

		// And replace the bindings of the projection above the one we just added
		D_ASSERT(old_bindings.size() == new_proj_bindings.size());
		for (idx_t i = 0; i < old_bindings.size(); i++) {
			new_replacer.replacement_bindings.emplace_back(old_bindings[i], new_proj_bindings[i]);
			Printer::PrintF("New Replacer %d, %d with %d, %d", old_bindings[i].table_index, old_bindings[i].column_index, new_proj_bindings[i].table_index, new_proj_bindings[i].column_index);
		}
		new_replacer.stop_operator = new_projection.get();
		// new_replacer.VisitOperator(*new_projection);
		new_projection->children.push_back(std::move(op));

		auto &right_inner_join = join.children[1]->Cast<LogicalJoin>();
		replacer.stop_operator = &right_inner_join;
		replacer.VisitOperator(*new_projection);


		return new_projection;

		}
	default: break;
	}

	return  op;
}

} // namespace duckdb