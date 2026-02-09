#include "duckdb/optimizer/projection_pullup.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"

namespace duckdb {

unique_ptr<LogicalOperator> ProjectionPullup::Optimize(unique_ptr<LogicalOperator> op) {
	// for (idx_t i = 0; i < parents.size(); i++) {
	// 	auto &op = parents[i];
	// 	Printer::PrintF("%s, ", op.get()->GetName());
	// }
	// Printer::PrintF("\n");
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &comp_join = op->Cast<LogicalComparisonJoin>();
		if (comp_join.join_type == JoinType::MARK) {
			break; // bail
		}

		// We can pull through this operator, add it to the stack
		parents.push_back(op);

		// Recurse
		for (auto &child : op->children) {
			child = Optimize(std::move(child));
		}

		// pop back elements until the last operator in the stack is THIS operator
		while (!parents.empty() && parents.back().get() != op) {
			parents.pop_back();
		}
		// then pop THIS operator back, and stop
		if (!parents.empty()) {
			parents.pop_back();
		}
		return op;
	}
	// FIXME: OPT I think we can pull through unnest when the expression in the projection is not referencing the
	// unnested column LogicalOperatorType::LOGICAL_UNNEST:
	case LogicalOperatorType::LOGICAL_FILTER: {
		// We can pull through this operator, add it to the stack
		parents.push_back(op);

		// Recurse
		op->children[0] = Optimize(std::move(op->children[0]));

		// pop back elements until the last operator in the stack is THIS operator
		while (!parents.empty() && parents.back().get() != op) {
			parents.pop_back();
		}
		// then pop THIS operator back, and stop
		if (!parents.empty()) {
			parents.pop_back();
		}
		return op;
	}
	// FIXME: Cannot pull through these operators. Not sure if this is the best way to handle it.
	// I push them to the parents, so later when I find a projection I can
	case LogicalOperatorType::LOGICAL_WINDOW:
	// FIXME: not sure what to do about delim_join. Problem is that the optimizer is pulling a column reference through
	// a DELIM_JOIN
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		bool old = blocking_operator;
		blocking_operator = true;

		op->children[0] = Optimize(std::move(op->children[0]));

		blocking_operator = old;
		return op;
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		if (blocking_operator) {
			// Projection is below a blocking operator must not be removed
			parents.push_back(op);
			op->children[0] = Optimize(std::move(op->children[0]));
			parents.pop_back();
			return op;
		}
		auto &proj = op->Cast<LogicalProjection>();
		auto proj_bindings = proj.GetColumnBindings();

		// create data structure on projection expressions and output column bindings
		// might need column_binding_map_t anyway?
		column_binding_map_t<unique_ptr<Expression>> projection_map;
		for (idx_t i = 0; i < proj.expressions.size(); i++) {
			projection_map[proj_bindings[i]] = proj.expressions[i]->Copy();
		}

		// Check if all expressions are simple column refs
		// Cannot pull this projection up safely if any expression is not a column ref
		bool all_column_refs = true;
		for (auto &expr : proj.expressions) {
			if (expr->type != ExpressionType::BOUND_COLUMN_REF) {
				all_column_refs = false;
				break;
			}
		}

		// loop backwards through parents
		// call LogicalOperatorVisitor::EnumerateExpressions on each parent to figure out if you can push through it
		// if expressions in the projections are colrefs, we can always pull it up
		// if it's not a colref, we can pull it up only if it does not appear in the operator enumerate expressions
		idx_t pull_up_to_here = parents.size();
		for (idx_t i = parents.size(); i > 0; i--) {
			idx_t parent_idx = i - 1;
			auto &parent_op = parents[parent_idx].get();
			bool can_pull_through = true;

			LogicalOperatorVisitor::EnumerateExpressions(*parent_op, [&](unique_ptr<Expression> *expr) {
				ExpressionIterator::EnumerateExpression(*expr, [&](unique_ptr<Expression> &child_expr) {
					if (child_expr->type == ExpressionType::BOUND_COLUMN_REF) {
						auto &colref = child_expr->Cast<BoundColumnRefExpression>();
						auto entry = projection_map.find(colref.binding);
						if (entry != projection_map.end()) {
							// Projection is referenced by parent
							if (entry->second->type != ExpressionType::BOUND_COLUMN_REF) {
								// Not a simple column ref, cannot pull through
								can_pull_through = false;
							}
						}
					}
				});
			});

			if (!can_pull_through) {
				// Can only pull up to here
				pull_up_to_here = parent_idx + 1;
				break;
			}
		}

		// after the loop we figured out how far we can pull it up (under which parent should it be placed?)
		// now do column binding replacement starting from root, stop_operator = proj.children[0]

		// If we can pull up, replace bindings along parents and remove this projection
		if (pull_up_to_here > 0 && all_column_refs) {
			ColumnBindingReplacer replacer;
			for (idx_t i = 0; i < proj.expressions.size(); i++) {
				auto &colref = proj.expressions[i]->Cast<BoundColumnRefExpression>();
				replacer.replacement_bindings.emplace_back(proj_bindings[i], colref.binding);
				// Printer::PrintF("Replacing [%d,%d] with [%d,%d]", proj_bindings[i].table_index,
				// proj_bindings[i].column_index, colref.binding.table_index, colref.binding.column_index);
			}

			// Replace bindings from root up to pull_up_to_here
			for (idx_t i = 0; i < pull_up_to_here; i++) {
				auto &parent = parents[i].get();

				// Replace in regular expressions
				LogicalOperatorVisitor::EnumerateExpressions(
				    *parent, [&](unique_ptr<Expression> *expr) { replacer.VisitExpression(expr); });

				// replace in join conditions if this is a join
				if (parent->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
					auto &join = parent->Cast<LogicalComparisonJoin>();
					for (auto &condition : join.conditions) {
						if (condition.IsComparison()) {
							replacer.VisitExpression(&condition.LeftReference());
							replacer.VisitExpression(&condition.RightReference());
						} else {
							replacer.VisitExpression(&condition.JoinExpressionReference());
						}
					}
				}
			}

			// Remove this projection
			auto child = std::move(op->children[0]);

			// recurse on children
			child = Optimize(std::move(child));

			// Return the child instead of this projection
			return child;
		}

		// If we cannot pull up, push this projection to parents stack
		parents.push_back(op);

		// Recurse on child
		op->children[0] = Optimize(std::move(op->children[0]));

		// Clean up parents stack
		if (!parents.empty() && &parents.back().get() == &op) {
			parents.pop_back();
		}

		return op;
	}
	default: {
		break;
	}
	}

	// Create new optimizer for child (start fresh without any state)
	for (auto &child : op->children) {
		ProjectionPullup next;
		child = next.Optimize(std::move(child));
	}

	// Also write a FIXME to do a second pass after this is done to combine projections
	return op;
}
} // namespace duckdb
