// The parts of query execution, shared between src/execute.cpp, src/scan.cpp and
// src/join.cpp. Not part of the public interface (that is execute() in plan.h).
#pragma once

#include <tuple>
#include <vector>

#include <plan.h>
#include "engine/column.h"

namespace Contest {

// An intermediate result: one column_t per output column of a plan node.
using ExecuteResult = std::vector<column_t>;

// Runs plan node `node_idx` (a scan or an intermediate join) and returns its result.
ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// scan.cpp
ExecuteResult execute_scan(const Plan& plan, const ScanNode& scan,
                           const std::vector<std::tuple<size_t, DataType>>& output_attrs);

// join.cpp
ExecuteResult execute_hash_join(const Plan& plan, const JoinNode& join,
                                const std::vector<std::tuple<size_t, DataType>>& output_attrs);
ColumnarTable execute_hash_join_root(const Plan& plan, const JoinNode& join,
                                     const std::vector<std::tuple<size_t, DataType>>& output_attrs);

} // namespace Contest
