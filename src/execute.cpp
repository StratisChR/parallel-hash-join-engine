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
#include "unchained_map.h"
#include "threads.h"

namespace Contest {

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
            results[c].pages.push_back(std::make_unique<ColPage>());
        }
    }

    const size_t PARALLEL_THRESHOLD = 8192; 

    auto run_copy_task = [&](size_t t_start, size_t t_end) {
        for (size_t t = t_start; t < t_end; ++t) {
            for (size_t c = 0; c < num_output_cols; ++c) {
                if (c >= thread_results[t].size()) continue;

                const auto& src_col = thread_results[t][c];
                size_t count = src_col.size();
                if (count == 0) continue;

                size_t global_start = offsets[c][t];
                
                // Fast Path Copy
                for (size_t i = 0; i < count; ++i) {
                    size_t dst_idx = global_start + i;
                    size_t page_idx = dst_idx / PAGE_CAPACITY;
                    size_t page_off = dst_idx % PAGE_CAPACITY;
                    results[c].pages[page_idx]->data[page_off] = src_col[i];
                }
            }
        }
    };

    if (total_rows_any < PARALLEL_THRESHOLD) {
        run_copy_task(0, num_threads);
    } else {
        pool().run([&](size_t t) {
            if (t < num_threads) run_copy_task(t, t + 1);
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
                                    if (i + 4 < count) { 
                                        hash_table.prefetch_directory(data[offset + i + 4]);
                                    }
                                    
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
        UnchainedHashMap<T, size_t> hash_table;

        // Build Phase
        ExecuteResult& build_table = build_left ? left : right;
        size_t build_idx = build_left ? left_col : right_col;
        hash_table.build(build_table, build_idx);

        // Probe Phase
        ExecuteResult& probe_table = build_left ? right : left;
        size_t probe_idx = build_left ? right_col : left_col;
        size_t left_table_cols = left.size();

        auto thread_results = JoinLogic::run_probe<T, ExecuteResult>(
            hash_table, probe_table, probe_idx, build_left, output_attrs.size(),
            [&](ExecuteResult& local, size_t l_idx, size_t r_idx) {
                for (size_t out_i = 0; out_i < output_attrs.size(); ++out_i) {
                    auto [src_col, _] = output_attrs[out_i];
                    if (src_col < left_table_cols) {
                        local[out_i].push_back(left[src_col][l_idx]);
                    } else {
                        local[out_i].push_back(right[src_col - left_table_cols][r_idx]);
                    }
                }
            }
        );

        // Merge Phase
        results.resize(output_attrs.size());
        parallel_merge_columns(results, thread_results, output_attrs.size());
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
        
        hash_table.build(build_table, build_col_idx);

        ExecuteResult& probe_table = join.build_left ? right : left;
        size_t probe_idx = join.build_left ? join.right_attr : join.left_attr;
        size_t left_table_cols = left.size();
        
        auto thread_results = JoinLogic::run_probe<T, RootResultType>(
            hash_table, probe_table, probe_idx, join.build_left, output_attrs.size(),
            [&](RootResultType& writers, size_t l_idx, size_t r_idx) {
                
                if (writers.empty()) writers.resize(output_attrs.size());
                if (!writers[0].col) {
                    for(size_t i=0; i<output_attrs.size(); ++i) {
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


ExecuteResult execute_scan(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {

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
        size_t total_rows = 0;
        for (auto* p : column.pages) {
            uint16_t n = *reinterpret_cast<uint16_t*>(p->data);
            if (n == 0xFFFF) total_rows += 1;      // Long string start
            else if (n == 0xFFFE) total_rows += 0; // Continuation
            else total_rows += n;                  // Standard
        }

        target_col.total_items = total_rows;
        size_t num_pages = (total_rows + PAGE_CAPACITY - 1) / PAGE_CAPACITY;
        target_col.pages.reserve(num_pages);
        for(size_t i=0; i<num_pages; ++i) {
            target_col.pages.push_back(std::make_unique<ColPage>());
        }

        size_t current_row = 0;
        size_t page_idx = 0; 
        ColPage* curr_target_page = num_pages > 0 ? target_col.pages[0].get() : nullptr;

        for (size_t src_p_idx = 0; src_p_idx < column.pages.size(); ++src_p_idx) {
            auto* page = column.pages[src_p_idx]->data;
            uint16_t val_at_0 = *reinterpret_cast<uint16_t*>(page);
            
            uint16_t num_logical_rows = val_at_0;
            bool is_long_start = false;

            if (val_at_0 == 0xFFFF) {
                num_logical_rows = 1;
                is_long_start = true;
            } else if (val_at_0 == 0xFFFE) {
                continue; 
            }

            auto* int_data = reinterpret_cast<int32_t*>(page + 4);
            auto* str_offsets = reinterpret_cast<uint16_t*>(page + 4);
            auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_logical_rows + 7) / 8);

            size_t in_page_idx = current_row % PAGE_CAPACITY;

            if (column.type == DataType::INT32) {
                uint16_t data_idx = 0;
                for (uint16_t i = 0; i < num_logical_rows; ++i) {
                    value_t v;
                    if (bitmap[i/8] & (1u << (i%8))) v = value_t(int_data[data_idx++]);
                    else v = value_t();

                    curr_target_page->data[in_page_idx++] = v;
                    
                    if (in_page_idx == PAGE_CAPACITY) {
                        if (++page_idx < num_pages) curr_target_page = target_col.pages[page_idx].get();
                        in_page_idx = 0;
                    }
                }
            } else if (column.type == DataType::VARCHAR) {
                if (is_long_start) {
                     auto num_chars = *reinterpret_cast<uint16_t*>(page + 2);

                     StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), 
                                static_cast<uint16_t>(src_p_idx), static_cast<uint32_t>(num_chars), true};

                     curr_target_page->data[in_page_idx++] = value_t(s);

                     if (in_page_idx == PAGE_CAPACITY) {
                        if (++page_idx < num_pages) curr_target_page = target_col.pages[page_idx].get();
                        in_page_idx = 0;
                     }
                } else {
                    uint16_t data_idx = 0;
                    uint32_t prev_end = 0;
                    for (uint16_t i = 0; i < num_logical_rows; ++i) {
                        value_t v;
                        if (bitmap[i/8] & (1u << (i%8))) {
                            auto end_off = str_offsets[data_idx++];
                            uint32_t len = static_cast<uint32_t>(end_off) - prev_end;
                            uint32_t packed = (prev_end << 16) | (len & 0xFFFF);
                            prev_end = end_off;
                            StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), 
                                        static_cast<uint16_t>(src_p_idx), packed, false};
                            v = value_t(s);
                        } else {
                            v = value_t();
                        }

                        curr_target_page->data[in_page_idx++] = v;
                        if (in_page_idx == PAGE_CAPACITY) {
                            if (++page_idx < num_pages) curr_target_page = target_col.pages[page_idx].get();
                            in_page_idx = 0;
                        }
                    }
                }
            }
            current_row += num_logical_rows;
        }
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
void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest