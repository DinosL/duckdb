#include "duckdb/optimizer/projection_pullup.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"

namespace duckdb {

void ProjectionPullup::PopParents(const LogicalOperator &op) {
	// pop back elements until the last operator in the stack is THIS operator
	while (!parents.empty() && parents.back().get().get() != &op) {
		parents.pop_back();
	}
	// then pop THIS operator back, and stop
	if (!parents.empty()) {
		parents.pop_back();
	}
}

LogicalOperator *ProjectionPullup::FindParent(LogicalOperator &target, LogicalOperator &current) {
	if (&current == &target) {
		return nullptr;
	}
	for (auto &child : current.children) {
		if (child.get() == &target) {
			return &current;
		}
		if (child) {
			auto result = FindParent(target, *child);
			if (result) {
				return result;
			}
		}
	}
	return nullptr;
}

void ProjectionPullup::InsertProjectionBelowOp(unique_ptr<LogicalOperator> &op, unique_ptr<LogicalOperator> &child,
                                               bool stop_at_op) {
	if (child->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		child->ResolveOperatorTypes();
		auto proj_index = optimizer.binder.GenerateTableIndex();
		auto child_bindings = child->GetColumnBindings();
		const auto child_types = child->types;
		const auto column_count = child_bindings.size();

		vector<unique_ptr<Expression>> expressions;
		expressions.reserve(column_count);
		for (idx_t i = 0; i < column_count; i++) {
			expressions.push_back(make_uniq<BoundColumnRefExpression>(child_types[i], child_bindings[i]));
		}

		ColumnBindingReplacer replacer;
		for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
			const auto &old_binding = child_bindings[col_idx];
			replacer.replacement_bindings.emplace_back(old_binding, ColumnBinding(proj_index, col_idx));
		}

		auto new_projection = make_uniq<LogicalProjection>(proj_index, std::move(expressions));
		if (child->has_estimated_cardinality) {
			new_projection->SetEstimatedCardinality(child->estimated_cardinality);
		}

		new_projection->children.emplace_back(std::move(child));
		child = std::move(new_projection);

		if (stop_at_op) {
			replacer.stop_operator = op.get();
		} else {
			replacer.stop_operator = child.get();
		}
		replacer.VisitOperator(root);
	}
	ProjectionPullup next(optimizer, root);
	next.Optimize(child->children[0]);
}

void ProjectionPullup::Optimize(unique_ptr<LogicalOperator> &op) {
	// for (idx_t i = 0; i < parents.size(); i++) {
	// 	auto &op = parents[i];
	// 	Printer::PrintF("%s, ", op.get().GetName());
	// }
	// Printer::PrintF("\n");
	switch (op->type) {
	// These operators depend on column order.
	// If their immediate child is a projection, keep it and recurse into the projection’s child.
	// If no projection is present, insert one, then recurse into the newly added projection’s child.
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_UNION: {
		for (auto &child : op->children) {
			InsertProjectionBelowOp(op, child, true);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_DISTINCT:
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
	case LogicalOperatorType::LOGICAL_CTE_REF:
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
	case LogicalOperatorType::LOGICAL_PIVOT: {
		for (auto &child : op->children) {
			InsertProjectionBelowOp(op, child, false);
		}
		return;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &comp_join = op->Cast<LogicalComparisonJoin>();
		if (comp_join.join_type == JoinType::MARK || comp_join.join_type == JoinType::SINGLE) {
			break; // bail
		}
		auto &left = op->children[0];
		auto &right = op->children[1];

		// We can pull through this operator, add it to the stack
		parents.push_back(op);
		if (comp_join.join_type == JoinType::SEMI) {
			// LHS: can pull through
			Optimize(left);

			// RHS: Cannot pull through. Add a projection "barrier"
			InsertProjectionBelowOp(op, right, false);
			// PopParents(*op);
		} else {
			// All other joins: recurse normally on both sides
			// for (auto &child : op->children) {
			// 	Optimize(child);
			// }
			Optimize(left);
			// Printer::PrintF("In join");
			// for (idx_t i = 0; i < parents.size(); i++) {
			// 	auto &op = parents[i];
			// 	Printer::PrintF("%s, ", op.get()->GetName());
			// }
			Optimize(right);
			// PopParents(*op);
		}

		PopParents(*op);
		return;
	}
	case LogicalOperatorType::LOGICAL_FILTER: {
		// We can pull through this operator, add it to the stack
		parents.push_back(op);

		// Recurse
		Optimize(op->children[0]);

		PopParents(*op);
		return;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		// Printer::PrintF("Can pull it up to %d", pull_up_to_here);
		// for (idx_t i = 0; i < parents.size(); i++) {
		// 	auto &op = parents[i];
		// 	Printer::PrintF("%s, ", op.get()->GetName());
		// }
		// Printer::PrintF("\n");
		auto &proj = op->Cast<LogicalProjection>();
		auto proj_bindings = proj.GetColumnBindings();
		// for (auto &parent_op : parents) {
		// 	if (parent_op.get().get()->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		// 		// Stop pulling this projection through another projection
		// 		Optimize(op->children[0]); // recurse normally
		// 		return;
		// 	}
		// }
		// Printer::PrintF("Handling projection %d", proj.table_index);
		// Printer::PrintF("In projection");
		// for (idx_t i = 0; i < parents.size(); i++) {
		// 	auto &op = parents[i];
		// 	Printer::PrintF("%s, ", op.get()->GetName());
		// }

		// Check if all expressions are simple column refs
		// Cannot pull this projection up safely if any expression is not a column ref
		bool all_column_refs = true;
		column_binding_map_t<unique_ptr<Expression>> projection_map;
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			projection_map[proj_bindings[i]] = proj.expressions[i]->Copy();
			if (proj.expressions[i]->type != ExpressionType::BOUND_COLUMN_REF) {
				all_column_refs = false;
			}
		}
		bool can_pull_through;

		// if expressions in the projections are colrefs, we can always pull it up
		// if it's not a colref, we can pull it up only if it does not appear in the operator enumerate expressions
		idx_t pull_up_to_here = parents.size();
		for (idx_t i = parents.size(); i > 0; i--) {
			idx_t parent_idx = i - 1;
			LogicalOperator &parent_op = *parents[parent_idx].get();
			can_pull_through = true;

			LogicalOperatorVisitor::EnumerateExpressions(parent_op, [&](unique_ptr<Expression> *expr) {
				ExpressionIterator::EnumerateExpression(*expr, [&](unique_ptr<Expression> &child_expr) {
					if (child_expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
						return;
					}

					auto &colref = child_expr->Cast<BoundColumnRefExpression>();
					auto entry = projection_map.find(colref.binding);

					if (entry == projection_map.end()) {
						return; // not referencing this projection
					}

					// This parent references a projection output
					// If that output is NOT a simple column ref -> we cannot pull through
					if (entry->second->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
						can_pull_through = false;
					}
				});
			});

			if (!can_pull_through) {
				// Can only pull up to here
				pull_up_to_here = parent_idx + 1;
				break;
			}
		}

		// after the loop we figured out how far we can pull it up
		// If we can pull up, replace bindings along parents and remove this projection
		if (pull_up_to_here > 0) {
			if (all_column_refs) {
				auto child_bindings = op->children[0]->GetColumnBindings();
				// Do not remove projections above UNNEST. The projection above the unnest extracts just the required
				// fields. Removing it forces all other operators to carry the full struct, eventually causing the
				// memory blowup.
				if (op->children[0]->type == LogicalOperatorType::LOGICAL_UNNEST) {
					parents.push_back(op);
					Optimize(op->children[0]);
					PopParents(*op);
					return;
				}
				ColumnBindingReplacer replacer;
				for (idx_t i = 0; i < proj.expressions.size(); i++) {
					auto &colref = proj.expressions[i]->Cast<BoundColumnRefExpression>();
					replacer.replacement_bindings.emplace_back(proj_bindings[i], colref.binding);
				}

				replacer.stop_operator = proj.children[0];
				replacer.VisitOperator(root);

				// Re-run optimization after removing this projection.
				// Binding rewrites can make parent projections redundant, and without
				// another pass they would not be eliminated.
				Optimize(op->children[0]);
				op = std::move(op->children[0]);

				return;
			}

			for (idx_t i = 0; i < proj.expressions.size(); i++) {
				if (proj.expressions[i]->type == ExpressionType::VALUE_CONSTANT ||
				    proj.expressions[i]->type == ExpressionType::OPERATOR_COALESCE) {
					return;
				}
			}

			if (!can_pull_through) {
				return;
			}

			// auto &insert_node = *parents[pull_up_to_here - 1].get();
			auto &insert_node = *parents[parents.size() - pull_up_to_here].get();

			auto *parent_of_insert = FindParent(insert_node, root);
			if (parent_of_insert->type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY)
				return;

			if (parent_of_insert && parent_of_insert->type == LogicalOperatorType::LOGICAL_PROJECTION) {
				auto &parent_proj = parent_of_insert->Cast<LogicalProjection>();

				// Inline expressions into parent projection
				for (auto &expr : parent_proj.expressions) {
					ExpressionIterator::EnumerateExpression(expr, [&](unique_ptr<Expression> &child_expr) {
						if (child_expr->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
							return;
						}

						auto &colref = child_expr->Cast<BoundColumnRefExpression>();
						auto entry = projection_map.find(colref.binding);

						if (entry != projection_map.end()) {
							child_expr = entry->second->Copy();
						}
					});
				}

				ColumnBindingReplacer replacer;

				for (idx_t i = 0; i < proj.expressions.size(); i++) {
					if (proj.expressions[i]->type == ExpressionType::BOUND_COLUMN_REF) {
						auto &colref = proj.expressions[i]->Cast<BoundColumnRefExpression>();
						replacer.replacement_bindings.emplace_back(proj_bindings[i], colref.binding);
						// Printer::PrintF("Replacing [%d,%d] with [%d,%d]", proj_bindings[i].table_index,
						// proj_bindings[i].column_index, colref.binding.table_index, colref.binding.column_index);
					}
				}

				// replacer.stop_operator = op.get();
				replacer.stop_operator = op->children[0].get();

				// Rewrite only along pull-through chain
				for (idx_t i = 0; i < pull_up_to_here; i++) {
					replacer.VisitOperator(*parents[i].get());
				}

				// Remove current projection
				op = std::move(op->children[0]);

				// Continue optimizing from child
				Optimize(op);
				return;
			}

			ColumnBindingReplacer replacer;

			for (idx_t i = 0; i < proj.expressions.size(); i++) {
				if (proj.expressions[i]->type == ExpressionType::BOUND_COLUMN_REF) {
					auto &colref = proj.expressions[i]->Cast<BoundColumnRefExpression>();
					replacer.replacement_bindings.emplace_back(proj_bindings[i], colref.binding);
					// Printer::PrintF("Replacing [%d,%d] with [%d,%d]", proj_bindings[i].table_index, proj_bindings[i].column_index, colref.binding.table_index, colref.binding.column_index);
				}
			}

			replacer.stop_operator = op->children[0].get();

			// Rewrite only along pull-through chain
			for (idx_t i = 0; i < pull_up_to_here; i++) {
				replacer.VisitOperator(*parents[i].get());
			}

			// Detach projection
			auto projection_to_move = std::move(op);
			op = std::move(projection_to_move->children[0]);

			if (pull_up_to_here == parents.size()) {
				auto &top_parent = parents[parents.size() - pull_up_to_here].get();

				projection_to_move->children[0] = std::move(top_parent);
				top_parent = std::move(projection_to_move);
				return;
			}
			Printer::PrintF("ERROR==========================================================================================================");
			// Insert between parents[pull_up_to_here-1] and parents[pull_up_to_here]
			else {
				auto &parent_above = parents[pull_up_to_here - 1].get();
				auto &child_below = parents[pull_up_to_here].get();

				for (auto &child : parent_above->children) {
					if (child.get() == child_below.get()) {
						projection_to_move->children[0] = std::move(child);
						child = std::move(projection_to_move);
						return;
					}
				}
			}

			return;
		}

		// Recurse on child
		Optimize(op->children[0]);

		// Clean up parents stack
		if (!parents.empty() && parents.back().get() == op) {
			parents.pop_back();
		}
		return;
	}
	default: {
		break;
	}
	}

	// Create new optimizer for child (start fresh without any state)
	for (auto &child : op->children) {
		ProjectionPullup next(optimizer, root);
		next.Optimize(child);
	}
}
} // namespace duckdb
