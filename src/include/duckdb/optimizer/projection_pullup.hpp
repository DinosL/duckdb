//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/projection_pullup.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_binding_replacer.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Optimizer;
class LogicalOperator;

//! The ProjectionPullup optimizer pulls up projections from joins
class ProjectionPullup {
public:
	explicit ProjectionPullup(Optimizer &optimizer);

public:
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);
	bool CanOptimize(LogicalOperator &op);
	unique_ptr<LogicalOperator> Pullup(unique_ptr<LogicalOperator> op);

private:
	//! The optimizer
	Optimizer &optimizer;
	ColumnBindingReplacer replacer;
};

} // namespace duckdb