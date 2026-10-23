// Entry point of the engine: execute() runs one query plan.
//   scan.cpp          scans of the input tables
//   join.cpp          hash joins (intermediate and root)
//   lazy_strings.cpp  string references resolved at output
#include <cstdio>
#include <variant>

#include <plan.h>
#include "engine/execute_internal.h"
#include "engine/instrumentation.h"
#include "engine/lazy_strings.h"

namespace Contest {

ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>) {
                auto result = execute_hash_join(plan, value, node.output_attrs);
                MAT_RECORD(node_idx, result);
                return result;
            } else {
                return execute_scan(plan, value, node.output_attrs);
            }
        },
        node.data);
}

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    reset_string_columns();
    MAT_ANALYZE(plan);
    auto& root_node = plan.nodes[plan.root];
    auto& j = std::get<JoinNode>(root_node.data);
    return execute_hash_join_root(plan, j, root_node.output_attrs);
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {
#ifdef PHASE_TIMERS
    auto ms = [](auto& a) { return a.load() / 1000.0; };
    fprintf(stderr,
        "scan %.0f | build %.0f | probe %.0f | merge %.0f | root build %.0f | "
        "root probe+output %.0f | root finalize %.0f  (ms)\n",
        ms(g_timers.scan), ms(g_timers.build), ms(g_timers.probe), ms(g_timers.merge),
        ms(g_timers.root_build), ms(g_timers.root_probe), ms(g_timers.root_finalize));
    const char* label[3] = {"<1k", "1k-100k", ">=100k"};
    for (int k = 0; k < 3; ++k)
        fprintf(stderr, "joins with build side %-7s: %4lu joins, %6.0f ms\n", label[k],
                (unsigned long)g_timers.joins_by_size[k].load(), ms(g_timers.us_by_size[k]));
    fprintf(stderr, "scan: INT32 with nulls %.0f ms (%.1f M rows) | VARCHAR (lazy) %.0f ms (%.1f M rows)\n",
        ms(g_timers.scan_int), g_timers.rows_int.load() / 1e6,
        ms(g_timers.scan_str), g_timers.rows_str.load() / 1e6);
    fprintf(stderr, "root output: %.1f M rows, %.1f M string values\n",
            g_timers.root_rows.load() / 1e6, g_timers.root_str_values.load() / 1e6);
#endif
}

} // namespace Contest
