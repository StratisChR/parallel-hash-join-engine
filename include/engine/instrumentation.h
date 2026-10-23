// Measurement tools, all switched off by default. Each switch below compiles its
// counters into the engine; with the switch off, the macros expand to nothing.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <variant>
#include <vector>

#include <plan.h>
#include "engine/column.h"

namespace Contest {

// #define PHASE_TIMERS   // uncomment to print a per-phase time breakdown at the end of a run

#ifdef PHASE_TIMERS
struct PhaseTimers {
    std::atomic<uint64_t> scan{0}, build{0}, probe{0}, merge{0},
                          root_build{0}, root_probe{0}, root_finalize{0},
                          scan_int{0}, scan_str{0}, rows_int{0}, rows_str{0},
                          root_rows{0}, root_str_values{0};
    // Intermediate joins grouped by build-side rows: <1k, 1k-100k, >=100k.
    std::atomic<uint64_t> joins_by_size[3]{}, us_by_size[3]{};
};
inline PhaseTimers g_timers;

struct ScopedTimer {
    std::atomic<uint64_t>& acc;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~ScopedTimer() {
        acc += std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start).count();
    }
};
#define PHASE_TIMER(name) ScopedTimer phase_timer_##name{g_timers.name}
#else
#define PHASE_TIMER(name)
#endif

// ---------------------------------------------------------------------------------
// Materialization statistics (measurement only, off by default).
// For every intermediate join it counts the values written into the result, split into
//   - key values:     columns that some join further up uses as its join key,
//   - carried values: columns no join uses, copied along only to reach the root output.
// It also estimates what a row-id design would write instead: the key columns plus one
// row id per base table that still has carried columns.
// #define MATERIALIZATION_STATS
#ifdef MATERIALIZATION_STATS
struct MatStats {
    uint64_t joins = 0, rows = 0, values = 0, key_values = 0, rowid_values = 0;
    ~MatStats() {
        if (joins == 0) return;
        auto M = [](uint64_t v) { return v / 1e6; };
        uint64_t carried = values - key_values;
        uint64_t alt     = key_values + rowid_values;
        std::fprintf(stderr,
            "intermediate joins: %llu, result rows: %.1f M\n"
            "values written now:      %8.1f M\n"
            "  join keys:             %8.1f M (%.0f%%)\n"
            "  carried to root only:  %8.1f M (%.0f%%)\n"
            "with row ids instead:    %8.1f M (keys %.1f M + row ids %.1f M) = %.0f%% of now\n",
            (unsigned long long)joins, M(rows), M(values),
            M(key_values), 100.0 * key_values / values,
            M(carried),    100.0 * carried / values,
            M(alt), M(key_values), M(rowid_values), 100.0 * alt / values);
    }
};
inline MatStats g_mat;
inline std::vector<std::vector<bool>>   g_mat_key_cols;  // [node][output col]: used as a key above
inline std::vector<std::vector<size_t>> g_mat_origin;    // [node][output col]: scan node it comes from

// Walks the plan once per query, before execution.
inline void mat_analyze(const Plan& plan) {
    const size_t N = plan.nodes.size();
    g_mat_key_cols.assign(N, {});
    g_mat_origin.assign(N, {});

    // Bottom-up: which scan does each output column come from?
    std::function<void(size_t)> origin = [&](size_t n) {
        auto& node = plan.nodes[n];
        auto& o = g_mat_origin[n];
        o.resize(node.output_attrs.size());
        if (auto* j = std::get_if<JoinNode>(&node.data)) {
            origin(j->left);
            origin(j->right);
            size_t nl = plan.nodes[j->left].output_attrs.size();
            for (size_t c = 0; c < o.size(); ++c) {
                size_t s = std::get<0>(node.output_attrs[c]);
                o[c] = s < nl ? g_mat_origin[j->left][s] : g_mat_origin[j->right][s - nl];
            }
        } else {
            for (auto& x : o) x = n;
        }
    };

    // Top-down: which output columns of each node does some join above use as its key?
    std::function<void(size_t)> keys = [&](size_t n) {
        auto& node = plan.nodes[n];
        auto* j = std::get_if<JoinNode>(&node.data);
        if (!j) return;
        size_t nl = plan.nodes[j->left].output_attrs.size();
        auto& kl = g_mat_key_cols[j->left];
        auto& kr = g_mat_key_cols[j->right];
        kl.assign(nl, false);
        kr.assign(plan.nodes[j->right].output_attrs.size(), false);
        kl[j->left_attr]  = true;
        kr[j->right_attr] = true;
        for (size_t c = 0; c < node.output_attrs.size(); ++c) {
            if (!g_mat_key_cols[n][c]) continue;
            size_t s = std::get<0>(node.output_attrs[c]);
            if (s < nl) kl[s] = true; else kr[s - nl] = true;
        }
        keys(j->left);
        keys(j->right);
    };

    origin(plan.root);
    g_mat_key_cols[plan.root].assign(plan.nodes[plan.root].output_attrs.size(), false);
    keys(plan.root);
}

// Called after each intermediate join with its result.
inline void mat_record(size_t n, const std::vector<column_t>& result) {
    const size_t cols = result.size();
    const size_t rows = cols ? result[0].size() : 0;
    size_t key_cols = 0;
    std::vector<size_t> carried_scans;
    for (size_t c = 0; c < cols; ++c) {
        if (g_mat_key_cols[n][c]) ++key_cols;
        else carried_scans.push_back(g_mat_origin[n][c]);
    }
    std::sort(carried_scans.begin(), carried_scans.end());
    size_t scans = std::unique(carried_scans.begin(), carried_scans.end()) - carried_scans.begin();

    g_mat.joins        += 1;
    g_mat.rows         += rows;
    g_mat.values       += uint64_t(rows) * cols;
    g_mat.key_values   += uint64_t(rows) * key_cols;
    g_mat.rowid_values += uint64_t(rows) * scans;
}
#define MAT_ANALYZE(plan)          mat_analyze(plan)
#define MAT_RECORD(node, result)   mat_record(node, result)
#else
#define MAT_ANALYZE(plan)
#define MAT_RECORD(node, result)
#endif
// ---------------------------------------------------------------------------------

} // namespace Contest
