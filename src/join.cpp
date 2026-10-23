// Hash joins: build on the smaller input, probe in parallel batches, and either merge the
// per-thread results into an intermediate result or write the final result directly.
#include <algorithm>
#include <atomic>
#include <cassert>
#include <memory>
#include <stdexcept>
#include <string>

#include <plan.h>
#include <table.h>
#include "engine/column.h"
#include "engine/execute_internal.h"
#include "engine/instrumentation.h"
#include "engine/lazy_strings.h"
#include "engine/thread_pool.h"
#include "hashtable/unchained_map.h"

namespace Contest {

// Wrapper for writing to columnar tables during Root Join materialization
struct ColumnWriter {
    std::unique_ptr<Column> col;
    std::unique_ptr<ColumnInserter<int32_t>> int_inserter;
    std::unique_ptr<ColumnInserter<std::string>> str_inserter;
    DataType type;
    std::string scratch;   // reused buffer for long strings (one per writer, so one per thread)

    void (ColumnWriter::*add_val_impl)(const value_t&, const Plan&) = nullptr;
    void (ColumnWriter::*finalize_impl)() = nullptr;

    ColumnWriter() = default;
    ColumnWriter(ColumnWriter&&) = default;
    ColumnWriter& operator=(ColumnWriter&&) = default;

    void init(DataType t) {
        type = t;
        col = std::make_unique<Column>(t);
        if (t == DataType::INT32) {
            int_inserter = std::make_unique<ColumnInserter<int32_t>>(*col);
            add_val_impl = &ColumnWriter::add_val_int;
            finalize_impl = &ColumnWriter::finalize_int;
        } else {
            str_inserter = std::make_unique<ColumnInserter<std::string>>(*col);
            add_val_impl = &ColumnWriter::add_val_str;
            finalize_impl = &ColumnWriter::finalize_str;
        }
    }

    void add_val(const value_t& v, const Plan& plan) {
        (this->*add_val_impl)(v, plan);
    }

    void finalize() {
        if (col) {
            (this->*finalize_impl)();
        }
    }

private:
    void add_val_int(const value_t& v, const Plan& plan) {
        if (v.is_null()) int_inserter->insert_null();
        else int_inserter->insert(v.get_int());
    }

    void add_val_str(const value_t& v, const Plan& plan) {
        std::string_view sv;
        if (v.is_null() || !resolve_string(v, scratch, sv)) str_inserter->insert_null();
        else str_inserter->insert(sv);
    }

    void finalize_int() { int_inserter->finalize(); }
    void finalize_str() { str_inserter->finalize(); }
};

// Combines the per-thread results of a join into one result. Row order inside an
// intermediate result does not matter (a join's output is the same for any input
// order), so instead of copying every row, the full pages of every thread are moved
// over (just pointers), and only each thread's partially filled last page is copied.
// All columns of one thread have the same length, so every output column gets the
// same row arrangement.
static void merge_thread_results(std::vector<column_t>& results,
                                 std::vector<ExecuteResult>& thread_results,
                                 size_t num_output_cols) {
    const size_t num_threads = thread_results.size();
    for (size_t c = 0; c < num_output_cols; ++c) {
        column_t& dst = results[c];
        size_t total = 0, tail_total = 0;
        for (auto& tr : thread_results) {
            if (c >= tr.size()) continue;
            total      += tr[c].size();
            tail_total += tr[c].size() % PAGE_CAPACITY;
        }
        dst.total_items = total;
        dst.pages.reserve((total + PAGE_CAPACITY - 1) / PAGE_CAPACITY);

        // 1. Move every full page (no copying).
        for (size_t t = 0; t < num_threads; ++t) {
            if (c >= thread_results[t].size()) continue;
            column_t& src = thread_results[t][c];
            assert(!src.is_view);
            size_t full = src.size() / PAGE_CAPACITY;
            for (size_t p = 0; p < full; ++p) dst.pages.push_back(std::move(src.pages[p]));
        }

        // 2. Copy the partial last pages, packed one after another.
        size_t tail_pages = (tail_total + PAGE_CAPACITY - 1) / PAGE_CAPACITY;
        size_t first_tail_page = dst.pages.size();
        for (size_t p = 0; p < tail_pages; ++p) dst.pages.push_back(acquire_page());
        size_t pos = 0;   // position within the tail area
        for (size_t t = 0; t < num_threads; ++t) {
            if (c >= thread_results[t].size()) continue;
            column_t& src = thread_results[t][c];
            size_t tail = src.size() % PAGE_CAPACITY;
            if (tail == 0) continue;
            const value_t* from = src.pages[src.size() / PAGE_CAPACITY]->data;
            size_t done = 0;
            while (done < tail) {
                size_t d_page = first_tail_page + pos / PAGE_CAPACITY, d_off = pos % PAGE_CAPACITY;
                size_t run = std::min(tail - done, PAGE_CAPACITY - d_off);
                std::copy_n(from + done, run, &dst.pages[d_page]->data[d_off]);
                done += run; pos += run;
            }
        }
    }
}

// Logic shared between standard Join and Root Join
struct JoinLogic{

    template <typename T, typename ResultContainer, typename MaterializeFn>
    static std::vector<ResultContainer> run_probe(
        UnchainedHashMap<T, size_t>& hash_table,
        ExecuteResult& probe_table,
        size_t probe_col_idx,
        bool build_left,
        size_t num_output_cols,
        MaterializeFn&& materialize_fn
    )
    {
        size_t num_threads = pool().size();
        std::vector<ResultContainer> thread_results(num_threads);
        for (auto& r : thread_results) r.resize(num_output_cols);

        auto& probe_col = probe_table[probe_col_idx];
        size_t total_rows = probe_col.total_items;

        std::atomic<size_t> next_batch_idx{0};
        // Rows each thread takes at a time: big enough that taking work is cheap, small
        // enough that every thread gets work (65536 left most threads idle on probe sides
        // below ~786k rows). About 8 pieces per thread, between 2048 and 65536 rows.
        const size_t BATCH_SIZE = std::clamp<size_t>(total_rows / (num_threads * 8), 2048, 65536);
        constexpr size_t PB = UnchainedHashMap<T, size_t>::PROBE_BATCH;

        pool().run([&](size_t t) {
            auto& local_res = thread_results[t];

            // Reports one match: probe row `probe_row` matched build row `build_row`.
            auto emit = [&](size_t probe_row, size_t build_row) {
                materialize_fn(local_res, build_left ? build_row : probe_row,
                                          build_left ? probe_row : build_row);
            };

            int32_t  keys[PB];      // used by the materialized path only
            uint32_t rows[PB];

            while (true) {
                size_t start = next_batch_idx.fetch_add(BATCH_SIZE, std::memory_order_relaxed);
                if (start >= total_rows) break;
                size_t end = std::min(start + BATCH_SIZE, total_rows);

                if (probe_col.is_view) {
                    // Zero-copy input: keys are already a contiguous int32 array, no nulls.
                    auto it = std::upper_bound(probe_col.view_page_offsets.begin(),
                                               probe_col.view_page_offsets.end(), start);
                    size_t page_idx = (it == probe_col.view_page_offsets.begin())
                                      ? 0 : std::distance(probe_col.view_page_offsets.begin(), it) - 1;
                    while (page_idx < probe_col.view_pages.size()) {
                        size_t page_start = probe_col.view_page_offsets[page_idx];
                        if (page_start >= end) break;
                        std::byte* raw_page = probe_col.view_pages[page_idx];
                        uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw_page);
                        size_t work_start = std::max(start, page_start);
                        size_t work_end   = std::min(end, page_start + num_rows);
                        const int32_t* data = reinterpret_cast<const int32_t*>(raw_page + 4)
                                              + (work_start - page_start);
                        for (size_t b = work_start; b < work_end; b += PB) {
                            size_t n = std::min(PB, work_end - b);
                            hash_table.lookup_batch(data + (b - work_start), n,
                                [&](size_t i, size_t build_row) { emit(b + i, build_row); });
                        }
                        page_idx++;
                    }
                } else {
                    // Materialized input: collect non-null keys (and their row numbers) into a batch.
                    size_t n = 0;
                    auto flush = [&] {
                        hash_table.lookup_batch(keys, n,
                            [&](size_t i, size_t build_row) { emit(rows[i], build_row); });
                        n = 0;
                    };
                    for (size_t cur = start; cur < end;) {
                        size_t page_idx = cur / PAGE_CAPACITY, offset = cur % PAGE_CAPACITY;
                        if (page_idx >= probe_col.pages.size()) break;
                        const value_t* cells = probe_col.pages[page_idx]->data + offset;
                        size_t count = std::min(end - cur, PAGE_CAPACITY - offset);
                        for (size_t i = 0; i < count; ++i) {
                            if (cells[i].is_null()) continue;
                            keys[n] = cells[i].get_int();
                            rows[n] = static_cast<uint32_t>(cur + i);
                            if (++n == PB) flush();
                        }
                        cur += count;
                    }
                    if (n) flush();
                }
            }
        });
        return thread_results;
    }
};

struct JoinAlgorithm {
    bool                                              build_left;
    ExecuteResult&                                    left;
    ExecuteResult&                                    right;
    ExecuteResult&                                    results;
    size_t                                            left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>&  output_attrs;
    const Plan* plan_ptr;

    template <class T>
    auto run() {
#ifdef PHASE_TIMERS
        auto join_start = std::chrono::steady_clock::now();
#endif
        UnchainedHashMap<T, size_t> hash_table;

        // Build Phase
        ExecuteResult& build_table = build_left ? left : right;
        size_t build_idx = build_left ? left_col : right_col;
        { PHASE_TIMER(build); hash_table.build(build_table, build_idx); }

        // Probe Phase
        ExecuteResult& probe_table = build_left ? right : left;
        size_t probe_idx = build_left ? right_col : left_col;
        size_t left_table_cols = left.size();

        // Resolve once per join where each output column comes from.
        struct OutSource { const column_t* col; bool from_left; };
        std::vector<OutSource> sources;
        sources.reserve(output_attrs.size());
        for (auto& [src_col, _] : output_attrs)
            sources.push_back(src_col < left_table_cols ? OutSource{&left[src_col], true}
                                                        : OutSource{&right[src_col - left_table_cols], false});
        const size_t num_cols = sources.size();

        std::vector<ExecuteResult> thread_results;
        {
            PHASE_TIMER(probe);
            thread_results = JoinLogic::run_probe<T, ExecuteResult>(
                hash_table, probe_table, probe_idx, build_left, output_attrs.size(),
                [&](ExecuteResult& local, size_t l_idx, size_t r_idx) {
                    if (num_cols == 0) return;
                    // All output columns of a thread have the same length; only column 0
                    // keeps the count during the probe (fixed up for the others below).
                    size_t n   = local[0].total_items;
                    size_t off = n % PAGE_CAPACITY;
                    if (off == 0)
                        for (size_t c = 0; c < num_cols; ++c) local[c].pages.push_back(acquire_page());
                    for (size_t c = 0; c < num_cols; ++c) {
                        const OutSource& s = sources[c];
                        local[c].pages.back()->data[off] = (*s.col)[s.from_left ? l_idx : r_idx];
                    }
                    local[0].total_items = n + 1;
                }
            );
        }

        // Give every output column the row count that column 0 tracked.
        for (auto& local : thread_results)
            for (size_t c = 1; c < num_cols; ++c) local[c].total_items = local[0].total_items;

        // Merge Phase
        {
            PHASE_TIMER(merge);
            results.resize(output_attrs.size());
            merge_thread_results(results, thread_results, output_attrs.size());
        }

#ifdef PHASE_TIMERS
        size_t b = build_table[build_idx].size();
        int k = b < 1000 ? 0 : (b < 100000 ? 1 : 2);
        g_timers.joins_by_size[k]++;
        g_timers.us_by_size[k] += std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - join_start).count();
#endif
    }
};

ExecuteResult execute_hash_join(const Plan& plan,
                                const JoinNode& join,
                                const std::vector<std::tuple<size_t, DataType>>& output_attrs) {

    auto left_idx    = join.left;
    auto right_idx   = join.right;
    auto& left_node  = plan.nodes[left_idx];
    auto& right_node = plan.nodes[right_idx];
    auto& left_types = left_node.output_attrs;
    auto& right_types = right_node.output_attrs;

    auto left  = execute_impl(plan, left_idx);
    auto right = execute_impl(plan, right_idx);

    // Build on the smaller input. The plan's build side comes from PostgreSQL's size
    // *estimates*; here we know the actual sizes. A join's result is the same either way.
    bool build_left = left[join.left_attr].size() <= right[join.right_attr].size();

    ExecuteResult results;
    results.reserve(output_attrs.size());
    for(size_t i = 0; i < output_attrs.size(); ++i) {
        results.emplace_back();
    }

    JoinAlgorithm join_algorithm{
        .build_left    = build_left,
        .left          = left,
        .right         = right,
        .results       = results,
        .left_col      = join.left_attr,
        .right_col     = join.right_attr,
        .output_attrs  = output_attrs,
        .plan_ptr      = &plan
    };

    if (build_left) {
        switch (std::get<1>(left_types[join.left_attr])) {
            case DataType::INT32:   join_algorithm.run<int32_t>(); break;
            default: throw std::runtime_error("unsupported join key type");
        }
    } else {
        switch (std::get<1>(right_types[join.right_attr])) {
            case DataType::INT32:   join_algorithm.run<int32_t>(); break;
            default: throw std::runtime_error("unsupported join key type");
        }
    }

    return results;
}

ColumnarTable execute_hash_join_root(const Plan& plan,
                                     const JoinNode& join,
                                     const std::vector<std::tuple<size_t, DataType>>& output_attrs) {

    auto left  = execute_impl(plan, join.left);
    auto right = execute_impl(plan, join.right);

    bool build_left = left[join.left_attr].size() <= right[join.right_attr].size();

    auto& left_types  = plan.nodes[join.left].output_attrs;
    auto& right_types = plan.nodes[join.right].output_attrs;

    using RootResultType = std::vector<ColumnWriter>;

    auto run_root_join = [&](auto type_tag) -> ColumnarTable {
        using T = typename decltype(type_tag)::type;

        UnchainedHashMap<T, size_t> hash_table;

        ExecuteResult& build_table = build_left ? left : right;
        size_t build_col_idx = build_left ? join.left_attr : join.right_attr;

        { PHASE_TIMER(root_build); hash_table.build(build_table, build_col_idx); }

        ExecuteResult& probe_table = build_left ? right : left;
        size_t probe_idx = build_left ? join.right_attr : join.left_attr;
        size_t left_table_cols = left.size();

        std::vector<RootResultType> thread_results;
        {
            PHASE_TIMER(root_probe);
            thread_results = JoinLogic::run_probe<T, RootResultType>(
                hash_table, probe_table, probe_idx, build_left, output_attrs.size(),
                [&](RootResultType& writers, size_t l_idx, size_t r_idx) {
                    if (writers.empty()) writers.resize(output_attrs.size());
                    if (!writers[0].col) {
                        for (size_t i = 0; i < output_attrs.size(); ++i) {
                            writers[i].init(std::get<1>(output_attrs[i]));
                        }
                    }
                    for (size_t out_i = 0; out_i < output_attrs.size(); ++out_i) {
                        auto [src_col, dtype] = output_attrs[out_i];
                        value_t val;
                        if (src_col < left_table_cols) val = left[src_col][l_idx];
                        else val = right[src_col - left_table_cols][r_idx];
                        writers[out_i].add_val(val, plan);
                    }
                }
            );
        }

        PHASE_TIMER(root_finalize);
        ColumnarTable result_table;

        result_table.columns.reserve(output_attrs.size());

        for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
            DataType type = std::get<1>(output_attrs[col_idx]);
            result_table.columns.emplace_back(type);
            Column& global_col = result_table.columns.back();

            for (auto& t_vec : thread_results) {
                if (col_idx >= t_vec.size()) continue;
                auto& writer = t_vec[col_idx];

                if (writer.col) {
                    writer.finalize();
                    global_col.pages.insert(global_col.pages.end(),
                                            writer.col->pages.begin(),
                                            writer.col->pages.end());
                    writer.col->pages.clear();
                }
            }
        }

        if (!result_table.columns.empty()) {
            size_t rows = 0;
            for (auto* page : result_table.columns[0].pages) {
                rows += *reinterpret_cast<uint16_t*>(page->data);
            }
            result_table.num_rows = rows;
        } else {
            result_table.num_rows = 0;
        }

#ifdef PHASE_TIMERS
        size_t str_cols = 0;
        for (auto& [_, type] : output_attrs) if (type == DataType::VARCHAR) ++str_cols;
        g_timers.root_rows       += result_table.num_rows;
        g_timers.root_str_values += result_table.num_rows * str_cols;
#endif

        return result_table;
    };

    struct IntTag { using type = int32_t; };
    DataType key_type = build_left ? std::get<1>(left_types[join.left_attr])
                                        : std::get<1>(right_types[join.right_attr]);

    if (key_type == DataType::INT32) {
        return run_root_join(IntTag{});
    } else {
        throw std::runtime_error("Unsupported join key type in root join");
    }
}

} // namespace Contest
