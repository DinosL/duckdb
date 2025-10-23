//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/window_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class WindowRewriter  : public BaseColumnPruner {
public:

	explicit WindowRewriter(Optimizer &optimizer);
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);
	unique_ptr<LogicalOperator> OptimizeInternal(unique_ptr<LogicalOperator> op, ColumnBindingReplacer &replacer);
	static bool CanOptimize(LogicalOperator &op);


private:
	Optimizer &optimizer;
};

}