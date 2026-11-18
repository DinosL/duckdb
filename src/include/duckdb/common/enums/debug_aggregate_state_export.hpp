//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/enums/debug_aggregate_state_export.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"

namespace duckdb {

//! Which hash aggregate phases are forced to export their states (debug setting)
enum class DebugAggregateStateExport : uint8_t {
	//! Export only under memory pressure (default)
	NONE = 0,
	//! Every thread exports its states when it combines
	COMBINE = 1,
	//! Every sink call exports its states as well, and the exported partitions are widened once
	SINK = 2
};

} // namespace duckdb
