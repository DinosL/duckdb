#pragma once

#include "column_binding_replacer.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"

namespace duckdb {

class Optimizer;
class LogicalOperator;

class ProjectionPullup {
public:
	explicit ProjectionPullup(LogicalOperator &root) : root(root) {
	}

	void Optimize(unique_ptr<LogicalOperator> &op);
	void PopParents(const LogicalOperator &op);

private:
	LogicalOperator &root;
	std::vector<LogicalOperator *> parents;
};

} // namespace duckdb
