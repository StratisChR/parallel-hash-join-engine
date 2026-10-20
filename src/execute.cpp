#include <hardware.h>
#include <plan.h>
#include <hopscotch_hash.h>
#include <table.h>
#include "indexing.h"
#include <stdexcept>
#include <variant>
#include <thread>
#include <atomic>
#include <mutex>
#include <cassert>
#include <algorithm>
#include "unchained_map.h"
#include "threads.h"

namespace Contest {
    
#define PHASE_TIMERS   // comment this line out to switch all timing off

#ifdef PHASE_TIMERS
#include <atomic>
#include <chrono>
#include <cstdio>

struct PhaseTimers {
    std::atomic<uint64_t> scan{0}, build{0}, probe{0}, merge{0},
                          root_build{0}, root_probe{0}, root_finalize{0};
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

using ExecuteResult = std::vector<column_t>;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// Helper: Materializes a StringRef into a standard std::string.
// Handles potentially segmented (long) strings across pages.
static std::string materialize_string_from_value(const Plan& plan, const value_t& vv) {
    auto sref = vv.get_stringref();
    if (sref.is_null()) return std::string();
    
    auto table_id = sref.table_id();
    auto col_id = sref.column_id();
    auto page_id = sref.page_id();
    auto off = sref.offset();
    const auto& column = plan.inputs[table_id].columns[col_id];
    
    if (sref.is_long()) {
        std::string out;
        size_t idx = page_id;
        do {
            auto* page = column.pages[idx]->data;
            auto n = *reinterpret_cast<uint16_t*>(page + 2);
            out.append(reinterpret_cast<char*>(page + 4), n);
            ++idx;
        } while (idx < column.pages.size() &&
                *reinterpret_cast<uint16_t*>(column.pages[idx]->data) == 0xfffe);
        return out;
    } else {
        // Short string optimization: Contained within a single page
        auto* page = column.pages[page_id]->data;
        auto num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
        // Skip header and bitmask
        auto* data_begin = reinterpret_cast<char*>(page + 4 + num_non_null * 2);
        
        uint32_t start = static_cast<uint32_t>(off >> 16);
        uint32_t len = static_cast<uint32_t>(off & 0xFFFF);
        return std::string(data_begin + start, data_begin + start + len);
    }
}

// Wrapper for writing to columnar tables during Root Join materialization
struct ColumnWriter {
    std::unique_ptr<Column> col;
    std::unique_ptr<ColumnInserter<int32_t>> int_inserter;
    std::unique_ptr<ColumnInserter<std::string>> str_inserter;
    DataType type;

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
        if (v.is_null()) str_inserter->insert_null();
        else str_inserter->insert(materialize_string_from_value(plan, v));
    }
    
    void finalize_int() { int_inserter->finalize(); }
    void finalize_str() { str_inserter->finalize(); }
};


// Copies `count` values from src (starting at row 0) into dst starting at row dst_start.
// Each run ends at whichever page boundary comes first (source or destination),
// so every run is a plain contiguous copy instead of one division per value.
static void copy_column_range(column_t& dst, size_t dst_start, const column_t& src, size_t count) {
    assert(!src.is_view && !dst.is_view);
    size_t i = 0;
    while (i < count) {
        size_t s_page = i / PAGE_CAPACITY,               s_off = i % PAGE_CAPACITY;
        size_t d_page = (dst_start + i) / PAGE_CAPACITY, d_off = (dst_start + i) % PAGE_CAPACITY;
        size_t run = std::min({count - i, PAGE_CAPACITY - s_off, PAGE_CAPACITY - d_off});
        std::copy_n(&src.pages[s_page]->data[s_off], run, &dst.pages[d_page]->data[d_off]);
        i += run;
    }
}

// Merges thread-local results into the final result columns.
// Uses parallel copying if the dataset size exceeds PARALLEL_THRESHOLD.
static void parallel_merge_columns(
    std::vector<column_t>& results,
    const std::vector<ExecuteResult>& thread_results,
    size_t num_output_cols) 
{
    size_t num_threads = thread_results.size();
    
    // offsets[col][thread_id] = global_start_index
    std::vector<std::vector<size_t>> offsets(num_output_cols, std::vector<size_t>(num_threads + 1, 0));
    size_t total_rows_any = 0;

    for (size_t c = 0; c < num_output_cols; ++c) {
        size_t accum = 0;
        for (size_t t = 0; t < num_threads; ++t) {
            offsets[c][t] = accum;
            if (c < thread_results[t].size()) {
                accum += thread_results[t][c].size();
            }
        }
        offsets[c][num_threads] = accum;
        if (c == 0) total_rows_any = accum;
    }

    // Pre-allocate Pages serially to avoid allocation contention
    for (size_t c = 0; c < num_output_cols; ++c) {
        size_t total_items = offsets[c][num_threads];
        results[c].total_items = total_items;
        size_t num_pages = (total_items + PAGE_CAPACITY - 1) / PAGE_CAPACITY;
        if (total_items == 0) num_pages = 0;
        
        results[c].pages.reserve(num_pages);
        for(size_t p=0; p<num_pages; ++p) {
            results[c].pages.push_back(acquire_page());
        }
    }

    const size_t PARALLEL_THRESHOLD = 8192; 

    auto run_copy_task = [&](size_t t) {
        for (size_t c = 0; c < num_output_cols; ++c) {
            if (c >= thread_results[t].size()) continue;
            const auto& src_col = thread_results[t][c];
            if (src_col.size() == 0) continue;
            copy_column_range(results[c], offsets[c][t], src_col, src_col.size());
        }
    };

    if (total_rows_any < PARALLEL_THRESHOLD) {
        for (size_t t = 0; t < num_threads; ++t) run_copy_task(t);
    } else {
        // Worker w handles thread results w, w + pool size, w + 2·pool size, ...
        // so every result is copied even if there are more results than workers.
        pool().run([&](size_t w) {
            for (size_t t = w; t < num_threads; t += pool().size()) run_copy_task(t);
        });
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
        // 3a. thread count:
        size_t num_threads = pool().size();
        std::vector<ResultContainer> thread_results(num_threads);
        for (auto& r : thread_results) r.resize(num_output_cols);   // was inside the spawn loop

        auto& probe_col = probe_table[probe_col_idx];
        size_t total_rows = probe_col.total_items; 

        std::atomic<size_t> next_batch_idx{0};
        const size_t BATCH_SIZE = 65536; 

        pool().run([&](size_t t) 
            {
                auto& local_res = thread_results[t];

                while (true) {
                    size_t start = next_batch_idx.fetch_add(BATCH_SIZE, std::memory_order_relaxed);
                    if (start >= total_rows) break;
                    size_t end = std::min(start + BATCH_SIZE, total_rows);

                    // Optimization: If using view pages, we can prefetch and process blocks
                    if (probe_col.is_view) {
                        auto it = std::upper_bound(probe_col.view_page_offsets.begin(), 
                                                   probe_col.view_page_offsets.end(), start);
                        size_t page_idx = (it == probe_col.view_page_offsets.begin()) 
                                          ? 0 : std::distance(probe_col.view_page_offsets.begin(), it) - 1;

                        while (page_idx < probe_col.view_pages.size()) {
                            size_t page_start = probe_col.view_page_offsets[page_idx];
                            if (page_start >= end) break;

                            std::byte* raw_page = probe_col.view_pages[page_idx];
                            uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw_page);
                            size_t page_end = page_start + num_rows;

                            size_t work_start = std::max(start, page_start);
                            size_t work_end = std::min(end, page_end);

                            if (work_start < work_end) {
                                const int32_t* data = reinterpret_cast<const int32_t*>(raw_page + 4);
                                size_t offset = work_start - page_start;
                                size_t count = work_end - work_start;

                                for (size_t i = 0; i < count; ++i) {
                                    // if (i + 4 < count) { 
                                    //     hash_table.prefetch_directory(data[offset + i + 4]);
                                    // }
                                    
                                    T key = static_cast<T>(data[offset + i]);
                                    
                                    hash_table.lookup(key, [&](size_t build_idx) {
                                        size_t cur_idx = work_start + i;
                                        materialize_fn(local_res, build_left ? build_idx : cur_idx, 
                                                                  build_left ? cur_idx : build_idx);
                                    });
                                }
                            }
                            page_idx++;
                        }
                    } 
                    else {
                        // Standard page iteration
                        constexpr size_t PAGE_CAPACITY = 4096;
                        size_t cur = start;
                        while(cur < end) {
                            size_t page_idx = cur / PAGE_CAPACITY;
                            size_t offset = cur % PAGE_CAPACITY;
                            
                            if (page_idx >= probe_col.pages.size()) break;
                            
                            const auto* page = probe_col.pages[page_idx].get();
                            size_t count = std::min(end - cur, PAGE_CAPACITY - offset);

                            for(size_t i=0; i<count; ++i) {
                                value_t cell = page->data[offset + i];
                                if (!cell.is_null()) {
                                    T key = static_cast<T>(cell.get_int());
                                    
                                    hash_table.lookup(key, [&](size_t build_idx) {
                                        size_t cur_idx = cur + i;
                                        materialize_fn(local_res, build_left ? build_idx : cur_idx, 
                                                                  build_left ? cur_idx : build_idx);
                                    });
                                }
                            }
                            cur += count;
                        }
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

    std::string materialize_string(const value_t& vv) const {
        return materialize_string_from_value(*plan_ptr, vv);
    }

    template <class T>
    auto run() {
#ifdef PHASE_TIMERS                                                       // NEW
        auto join_start = std::chrono::steady_clock::now();              // NEW
#endif                                                                    // NEW
        UnchainedHashMap<T, size_t> hash_table;

        // Build Phase
        ExecuteResult& build_table = build_left ? left : right;
        size_t build_idx = build_left ? left_col : right_col;
        { PHASE_TIMER(build); hash_table.build(build_table, build_idx); }   // CHANGED: wrapped

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

        std::vector<ExecuteResult> thread_results;                         // CHANGED: declared first
        {                                                                  // NEW
            PHASE_TIMER(probe);                                            // NEW
            thread_results = JoinLogic::run_probe<T, ExecuteResult>(       // CHANGED: no "auto"
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
        }                                                                  // NEW

        // Give every output column the row count that column 0 tracked.
        for (auto& local : thread_results)
            for (size_t c = 1; c < num_cols; ++c) local[c].total_items = local[0].total_items;

        // Merge Phase
        {                                                                  // NEW
            PHASE_TIMER(merge);                                            // NEW
            results.resize(output_attrs.size());
            parallel_merge_columns(results, thread_results, output_attrs.size());
        }                                                                  // NEW

#ifdef PHASE_TIMERS                                                       // NEW
        size_t b = build_table[build_idx].size();                         // NEW
        int k = b < 1000 ? 0 : (b < 100000 ? 1 : 2);                       // NEW
        g_timers.joins_by_size[k]++;                                      // NEW
        g_timers.us_by_size[k] += std::chrono::duration_cast<std::chrono::microseconds>(   // NEW
                                      std::chrono::steady_clock::now() - join_start).count();  // NEW
#endif                                                                    // NEW
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

    ExecuteResult results;
    results.reserve(output_attrs.size());
    for(size_t i = 0; i < output_attrs.size(); ++i) {
        results.emplace_back(); 
    }

    JoinAlgorithm join_algorithm{
        .build_left    = join.build_left,
        .left          = left,
        .right         = right,
        .results       = results,
        .left_col      = join.left_attr,
        .right_col     = join.right_attr,
        .output_attrs  = output_attrs,
        .plan_ptr      = &plan
    };

    if (join.build_left) {
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
    auto& left_types  = plan.nodes[join.left].output_attrs;
    auto& right_types = plan.nodes[join.right].output_attrs;

    using RootResultType = std::vector<ColumnWriter>;

    auto run_root_join = [&](auto type_tag) -> ColumnarTable {
        using T = typename decltype(type_tag)::type;

        UnchainedHashMap<T, size_t> hash_table;

        ExecuteResult& build_table = join.build_left ? left : right;
        size_t build_col_idx = join.build_left ? join.left_attr : join.right_attr;

        { PHASE_TIMER(root_build); hash_table.build(build_table, build_col_idx); }   // CHANGED: wrapped

        ExecuteResult& probe_table = join.build_left ? right : left;
        size_t probe_idx = join.build_left ? join.right_attr : join.left_attr;
        size_t left_table_cols = left.size();

        std::vector<RootResultType> thread_results;                          // CHANGED: declared first
        {                                                                    // NEW
            PHASE_TIMER(root_probe);                                         // NEW
            thread_results = JoinLogic::run_probe<T, RootResultType>(        // CHANGED: no "auto"
                hash_table, probe_table, probe_idx, join.build_left, output_attrs.size(),
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
        }                                                                    // NEW

        PHASE_TIMER(root_finalize);   // NEW: measures from here until the lambda returns
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

        return result_table;
    };
    
    struct IntTag { using type = int32_t; };
    DataType key_type = join.build_left ? std::get<1>(left_types[join.left_attr]) 
                                        : std::get<1>(right_types[join.right_attr]);

    if (key_type == DataType::INT32) {
        return run_root_join(IntTag{});
    } else {
        throw std::runtime_error("Unsupported join key type in root join");
    }
}

// Converts one input column (any column the zero-copy view can't handle) into value_t pages.
// Pass 1 reads only page headers to find where each input page's rows go in the output;
// pass 2 converts the pages in parallel, each into its own output positions.
static void scan_copy_column(const Column& column, size_t table_id, size_t in_col_idx, column_t& target_col) {
    const size_t n_src = column.pages.size();

    // Pass 1 (serial, headers only): first output row of every input page.
    std::vector<size_t> first_row(n_src + 1, 0);
    for (size_t p = 0; p < n_src; ++p) {
        uint16_t n = *reinterpret_cast<const uint16_t*>(column.pages[p]->data);
        size_t rows = (n == 0xFFFF) ? 1 : (n == 0xFFFE) ? 0 : n;   // long-string start / continuation / normal
        first_row[p + 1] = first_row[p] + rows;
    }
    const size_t total_rows = first_row[n_src];

    target_col.total_items = total_rows;
    size_t num_pages = (total_rows + PAGE_CAPACITY - 1) / PAGE_CAPACITY;
    target_col.pages.reserve(num_pages);
    for (size_t i = 0; i < num_pages; ++i) target_col.pages.push_back(acquire_page());

    const auto tid = static_cast<uint8_t>(table_id);
    const auto cid = static_cast<uint8_t>(in_col_idx);

    // Pass 2: convert input page p into output rows [first_row[p], first_row[p+1]).
    auto convert_page = [&](size_t p) {
        const std::byte* page = column.pages[p]->data;
        const uint16_t header = *reinterpret_cast<const uint16_t*>(page);
        if (header == 0xFFFE) return;                               // long-string continuation: no row

        size_t dst = first_row[p];
        auto put = [&](value_t v) {
            target_col.pages[dst / PAGE_CAPACITY]->data[dst % PAGE_CAPACITY] = v;
            ++dst;
        };

        if (header == 0xFFFF) {                                     // long-string start: one row
            auto num_chars = *reinterpret_cast<const uint16_t*>(page + 2);
            put(value_t(StringRef{tid, cid, static_cast<uint16_t>(p), static_cast<uint32_t>(num_chars), true}));
            return;
        }

        const uint16_t rows   = header;
        const auto*    bitmap = reinterpret_cast<const uint8_t*>(page + PAGE_SIZE - (rows + 7) / 8);
        if (column.type == DataType::INT32) {
            const auto* int_data = reinterpret_cast<const int32_t*>(page + 4);
            uint16_t data_idx = 0;
            for (uint16_t i = 0; i < rows; ++i)
                put((bitmap[i / 8] & (1u << (i % 8))) ? value_t(int_data[data_idx++]) : value_t());
        } else {                                                    // VARCHAR
            const auto* str_offsets = reinterpret_cast<const uint16_t*>(page + 4);
            uint16_t data_idx = 0;
            uint32_t prev_end = 0;
            for (uint16_t i = 0; i < rows; ++i) {
                if (bitmap[i / 8] & (1u << (i % 8))) {
                    uint32_t end_off = str_offsets[data_idx++];
                    uint32_t packed  = (prev_end << 16) | ((end_off - prev_end) & 0xFFFF);
                    prev_end = end_off;
                    put(value_t(StringRef{tid, cid, static_cast<uint16_t>(p), packed, false}));
                } else {
                    put(value_t());
                }
            }
        }
    };

    // Small columns: not worth waking the pool.
    constexpr size_t PARALLEL_MIN_PAGES = 16;
    if (n_src < PARALLEL_MIN_PAGES) {
        for (size_t p = 0; p < n_src; ++p) convert_page(p);
    } else {
        std::atomic<size_t> next{0};
        pool().run([&](size_t) {
            for (size_t p; (p = next.fetch_add(1, std::memory_order_relaxed)) < n_src;) convert_page(p);
        });
    }
}

ExecuteResult execute_scan(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    PHASE_TIMER(scan);

    auto table_id = scan.base_table_id;
    auto& input   = plan.inputs[table_id];

    ExecuteResult results;
    results.resize(output_attrs.size());
    
    for (size_t out_col = 0; out_col < output_attrs.size(); ++out_col) {
        auto in_col_idx = std::get<0>(output_attrs[out_col]);
        auto& column    = input.columns[in_col_idx];
        auto& target_col = results[out_col];

        // =========================================================
        // ZERO-COPY VIEW OPTIMIZATION
        // =========================================================
        // If the column has no nulls and is INT32, we can reuse the 
        // raw page buffers directly without copying data.
        if (column.type == DataType::INT32) {
            bool all_no_nulls = true;
            size_t total_items = 0;
            for (auto* p : column.pages) {
                auto* raw = p->data;
                uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw);
                uint16_t num_non_null = *reinterpret_cast<uint16_t*>(raw + 2);
                total_items += num_rows;
                if (num_non_null != num_rows) { all_no_nulls = false; break; }
            }
            if (all_no_nulls) {
                std::vector<std::byte*> page_data_ptrs;
                page_data_ptrs.reserve(column.pages.size());
                for (auto* p : column.pages) page_data_ptrs.push_back(p->data);
                target_col.set_view_pages(page_data_ptrs, total_items, DataType::INT32);
                continue; 
            }
        }

        // Standard Copy Path
        scan_copy_column(column, table_id, in_col_idx, target_col);
    }
    return results;
}

ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>) {
                return execute_hash_join(plan, value, node.output_attrs);
            } else {
                return execute_scan(plan, value, node.output_attrs);
            }
        },
        node.data);
}

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    auto& root_node = plan.nodes[plan.root];
    auto& j = std::get<JoinNode>(root_node.data);
    return execute_hash_join_root(plan, j, root_node.output_attrs);
}

void* build_context() {
    pool();          // start the worker threads once, before any query runs
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
#endif
}

} // namespace Contest