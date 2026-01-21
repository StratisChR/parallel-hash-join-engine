#include <hardware.h>
#include <plan.h>
#include <hopscotch_hash.h>
#include <table.h>
#include "indexing.h"

#include "hopscotch_hash.h"
#include "robin_hood_hash_map.h"
#include "cuckoo_hash_map.h"
#include "int_hasher.h"
#include <stdexcept>
#include <variant>
#include "unchained_map.h"

namespace Contest {

enum class JoinStrategy {
    STD_UNORDERED_MAP, 
    HOPSCOTCH,         // Panagiotis Papapostolou
    ROBIN_HOOD,        // Theodoros Mpeioglou
    CUCKOO             // Stratis Charisiadis-Roussis
};

// Choose join strategy here
constexpr JoinStrategy CURRENT_STRATEGY = JoinStrategy::HOPSCOTCH;

// --- CHANGED TO COLUMN STORE ---
using ExecuteResult = std::vector<column_t>;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// Helper function definition
static std::string materialize_string_from_value(const Plan& plan, const value_t& vv) {
    if (!vv.is_string()) {
        throw std::runtime_error("materialize_string on non-string value");
    }
    auto sref = vv.get_stringref();
    if (sref.is_null()) return std::string();
    auto table_id = sref.table_id();
    auto col_id = sref.column_id();
    auto page_id = sref.page_id();
    auto off = sref.offset();
    const auto& column = plan.inputs[table_id].columns[col_id];
    
    // handle long strings
    if (sref.is_long()) {
        std::string out;
        size_t idx = page_id;
        while (idx < column.pages.size()) {
            auto* page = column.pages[idx]->data;
            auto num_rows = *reinterpret_cast<uint16_t*>(page);
            if (num_rows != 0xffff && num_rows != 0xfffe) break;
            auto num_chars = *reinterpret_cast<uint16_t*>(page + 2);
            auto* data_begin = reinterpret_cast<char*>(page + 4);
            out.append(data_begin, data_begin + num_chars);
            ++idx;
            if (num_rows == 0xfffe) continue;
        }
        return out;
    } else {
        // normal short string in single page
        auto* page = column.pages[page_id]->data;
        auto num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
        auto* data_begin = reinterpret_cast<char*>(page + 4 + num_non_null * 2);
        uint32_t start = static_cast<uint32_t>(off >> 16);
        uint32_t len = static_cast<uint32_t>(off & 0xFFFF);
        return std::string(data_begin + start, data_begin + start + len);
    }
}

struct ColumnWriter {
    std::unique_ptr<Column> col;
    std::unique_ptr<ColumnInserter<int32_t>> int_inserter;
    std::unique_ptr<ColumnInserter<std::string>> str_inserter;
    DataType type;

    ColumnWriter() = default; 
    
    ColumnWriter(ColumnWriter&&) = default;
    ColumnWriter& operator=(ColumnWriter&&) = default;

    void reserve(size_t) {}

    void init(DataType t) {
        type = t;
        col = std::make_unique<Column>(t);
        if (t == DataType::INT32) {
            int_inserter = std::make_unique<ColumnInserter<int32_t>>(*col);
        } else {
            str_inserter = std::make_unique<ColumnInserter<std::string>>(*col);
        }
    }

    // Insert data directly into the binary page format
    void add_val(const value_t& v, const Plan& plan) {
        if (v.is_null()) {
            if (type == DataType::INT32) int_inserter->insert_null();
            else str_inserter->insert_null();
        } else {
            if (type == DataType::INT32) {
                int_inserter->insert(v.get_int());
            } else {
                // We still need to materialize the string to copy it into the page.
                // However, we avoid storing it in a variant or vector<string>.
                std::string s = materialize_string_from_value(plan, v);
                str_inserter->insert(s);
            }
        }
    }

    void finalize() {
        if (col) {
            if (type == DataType::INT32) int_inserter->finalize();
            else str_inserter->finalize();
        }
    }
};

static void parallel_merge_columns(
    std::vector<column_t>& results,
    const std::vector<ExecuteResult>& thread_results,
    size_t num_output_cols) 
{
    size_t num_threads = thread_results.size();
    
    // We calculate offsets.
    // offsets[col][thread_id] = start_index_in_global_column
    std::vector<std::vector<size_t>> offsets(num_output_cols, std::vector<size_t>(num_threads + 1, 0));
    
    // Check total rows in the first column to decide strategy.
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

    // Allocate Pages.
    // We do this serially regardless, as page allocation is fast but allocation contention is bad.
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

    // Thread creation overhead is ~20-50us. Copying 10k rows is often faster than that.
    // Only parallelize if we have > 32k rows (approx 8 pages).
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
        std::vector<std::thread> workers;
        
        for (size_t t = 0; t < num_threads; ++t) {
            workers.emplace_back([&, t]() {
                run_copy_task(t, t + 1);
            });
        }
        for (auto& w : workers) w.join();
    }
}

struct JoinLogic{
    // I was getting tired of seeing the same code in join and in join root, 
    // so it was time to create some shared functions.

    // ResultContainer is where we store the results
    // MaterializeFn is a lambda.
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
        // Parallel probing configuration.
        size_t num_threads = std::thread::hardware_concurrency();
        std::vector<ResultContainer> thread_results(num_threads);
        std::vector<std::thread> threads;

        auto& probe_col_obj = probe_table[probe_col_idx];
        size_t total_rows = probe_col_obj.size();

        // We tried various batching strategies here, none really stood out.
        // We got miniscule speed improvements.
        std::atomic<size_t> next_batch_idx{0};
        const size_t BATCH_SIZE = PAGE_CAPACITY * 4; 

        for(size_t t=0; t < num_threads; ++t){

            // Initialize buffers.
            if constexpr (std::is_same_v<ResultContainer, ExecuteResult>) {
                thread_results[t].resize(num_output_cols);
            } else {
                thread_results[t].resize(num_output_cols);
                for(auto& col : thread_results[t]) col.reserve(PAGE_CAPACITY); 
            }

            threads.emplace_back([&, t]() {
                auto& local_res = thread_results[t];

                while (true) {
                    // Claim batch.
                    size_t start = next_batch_idx.fetch_add(BATCH_SIZE, std::memory_order_relaxed);
                    
                    if (start >= total_rows) break; // No work left

                    size_t end = std::min(start + BATCH_SIZE, total_rows);

                    // Process Batch.
                    for (size_t global_idx = start; global_idx < end; global_idx += PAGE_CAPACITY) {
                        size_t limit = std::min(PAGE_CAPACITY, end - global_idx);
                        
                        // Inner loop over the page/chunk.
                        for (size_t i = 0; i < limit; ++i) {
                            size_t current_probe_idx = global_idx + i;
                            value_t cell = probe_col_obj[current_probe_idx];

                            if (cell.is_null()) continue;
                            
                            bool type_ok = true;
                            if constexpr (std::is_same_v<T, int32_t>) type_ok = (cell.type() == value_t::Type::INT32);
                            if (!type_ok) continue;

                            T key;
                            if constexpr (std::is_same_v<T, int32_t>) key = static_cast<T>(cell.get_int());

                            hash_table.lookup(key, [&](size_t build_idx) {
                                size_t left_idx  = build_left ? build_idx : current_probe_idx;
                                size_t right_idx = build_left ? current_probe_idx : build_idx;
                                materialize_fn(local_res, left_idx, right_idx);
                            });
                        }
                    }
                }
            });
        }

        for (auto& t : threads) t.join();
        return thread_results;
    }

    // For root join.
    template <typename InnerType>
    static void parallel_merge_vectors(
        // [thread][col][row]
        std::vector<std::vector<std::vector<InnerType>>>& thread_results, 
        // [col][row]
        std::vector<std::vector<InnerType>>& global_results,              
        size_t num_cols
    )
    {
        size_t num_src_threads = thread_results.size();
        
        // We need to know where every chunk goes before we can parallelize the copy.
        std::vector<std::vector<size_t>> thread_offsets(num_cols, std::vector<size_t>(num_src_threads + 1, 0));
        
        for (size_t c = 0; c < num_cols; ++c) {
            size_t current_size = 0;
            for (size_t t = 0; t < num_src_threads; ++t) {
                thread_offsets[c][t] = current_size;
                current_size += thread_results[t][c].size();
            }
            // Allocate the final memory once.
            global_results[c].resize(current_size);
        }

        
        // Instead of 1 task per thread, we split large buffers into chunks.
        // This handles skew: if Thread 0 has 10M rows, it generates many tasks 
        // that Threads 1..N can help process.
        struct CopyTask {
            size_t src_thread_idx;
            size_t col_idx;
            size_t src_offset;
            size_t dst_offset;
            size_t count;
        };

        std::vector<CopyTask> tasks;
        
        const size_t CHUNK_SIZE = 16384;

        for (size_t c = 0; c < num_cols; ++c) {
            for (size_t t = 0; t < num_src_threads; ++t) {
                size_t total_items = thread_results[t][c].size();
                if (total_items == 0) continue;

                size_t start_dst = thread_offsets[c][t];

                for (size_t i = 0; i < total_items; i += CHUNK_SIZE) {
                    size_t count = std::min(CHUNK_SIZE, total_items - i);
                    tasks.push_back({t, c, i, start_dst + i, count});
                }
            }
        }

        // Copy in parallel (with work stealing).
        size_t num_worker_threads = std::thread::hardware_concurrency();
        std::vector<std::thread> workers;
        std::atomic<size_t> next_task_idx{0};

        for (size_t w = 0; w < num_worker_threads; ++w) {
            workers.emplace_back([&]() {
                while (true) {
                    // Worker gets task.
                    size_t task_id = next_task_idx.fetch_add(1, std::memory_order_relaxed);
                    if (task_id >= tasks.size()) break;

                    const auto& task = tasks[task_id];
                    auto& src_vec = thread_results[task.src_thread_idx][task.col_idx];
                    auto& dst_vec = global_results[task.col_idx];

                    // Move/Copy. We take the tuples from thread_results and copy them in global.
                    std::copy(std::make_move_iterator(src_vec.begin() + task.src_offset), 
                              std::make_move_iterator(src_vec.begin() + task.src_offset + task.count), 
                              dst_vec.begin() + task.dst_offset);
                }
            });
        }

        for (auto& w : workers) w.join();
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
        namespace views = ranges::views;

        UnchainedHashMap<T, size_t> hash_table;

        // Build side:
        ExecuteResult& build_table = build_left ? left : right;
        size_t build_idx = build_left ? left_col : right_col;
        hash_table.build(build_table, build_idx);

        ExecuteResult& probe_table = build_left ? right : left;
        size_t probe_idx = build_left ? right_col : left_col;
        size_t left_table_cols = left.size();

        // parallel probe:
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

    // Prepare Result Columns
    ExecuteResult results;
    results.reserve(output_attrs.size());
    for(size_t i = 0; i < output_attrs.size(); ++i) {
        results.emplace_back(); // Constructs an empty column_t
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
            // case DataType::VARCHAR: join_algorithm.run<std::string, std::hash<std::string>>(); break;
            default: throw std::runtime_error("unsupported join key type");
        }
    } else {
        switch (std::get<1>(right_types[join.right_attr])) {
            case DataType::INT32:   join_algorithm.run<int32_t>(); break;
            // case DataType::VARCHAR: join_algorithm.run<std::string, std::hash<std::string>>(); break;
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

    // std::vector<std::vector<Data>> final_columns(output_attrs.size());
    // for(auto& col : final_columns) col.reserve(1024); 
    using RootResultType = std::vector<ColumnWriter>;

    auto run_root_join = [&](auto type_tag) -> ColumnarTable {
        using T = typename decltype(type_tag)::type;

        UnchainedHashMap<T, size_t> hash_table;

        // build
        ExecuteResult& build_table = join.build_left ? left : right;
        size_t build_col_idx = join.build_left ? join.left_attr : join.right_attr;
        
        hash_table.build(build_table, build_col_idx);

        ExecuteResult& probe_table = join.build_left ? right : left;
        size_t probe_idx = join.build_left ? join.right_attr : join.left_attr;
        size_t left_table_cols = left.size();
        
        // parallel probe
        auto thread_results = JoinLogic::run_probe<T, RootResultType>(
            hash_table, probe_table, probe_idx, join.build_left, output_attrs.size(),
            [&](RootResultType& writers, size_t l_idx, size_t r_idx) {
                
                // Initialize writers on first use for this thread
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

        // Merge Phase: Concatenate pages from thread-local columns to the final table
        ColumnarTable result_table;
        result_table.columns.reserve(output_attrs.size());
        
        for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
            DataType type = std::get<1>(output_attrs[col_idx]);
            // Emplace returns reference to the new column
            result_table.columns.emplace_back(type);
            Column& global_col = result_table.columns.back();
            
            for (auto& t_vec : thread_results) {
                if (col_idx >= t_vec.size()) continue;
                auto& writer = t_vec[col_idx];
                
                // If this thread processed any rows
                if (writer.col) {
                    writer.finalize(); // Flush last page
                    
                    // Steal pages: move ptrs from writer to global
                    // Column destructor deletes pages, so we must clear the writer's vector 
                    // after copying pointers to avoid double free.
                    global_col.pages.insert(global_col.pages.end(), 
                                            writer.col->pages.begin(), 
                                            writer.col->pages.end());
                    writer.col->pages.clear();
                }
            }
        }
        
        // Calculate total rows from the first column (if exists)
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
    
    // Iterate serially (Fastest for memory-bound tasks)
    for (size_t out_col = 0; out_col < output_attrs.size(); ++out_col) {
        auto in_col_idx = std::get<0>(output_attrs[out_col]);
        auto& column    = input.columns[in_col_idx];
        auto& target_col = results[out_col];

        // -------------------------------------------------------
        // 1. Zero-Copy View Optimization (INT32 only)
        // -------------------------------------------------------
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

        // -------------------------------------------------------
        // 2. Pre-calculate Size & Allocate
        // -------------------------------------------------------
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

        // -------------------------------------------------------
        // 3. Serial Direct Write (No push_back overhead)
        // -------------------------------------------------------
        size_t current_row = 0;
        size_t page_idx = 0; 
        // Cache the pointer to the current destination page
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

            // Pointers for reading source data
            auto* int_data = reinterpret_cast<int32_t*>(page + 4);
            auto* str_offsets = reinterpret_cast<uint16_t*>(page + 4);
            auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_logical_rows + 7) / 8);

            // Current writing position within the page
            size_t in_page_idx = current_row % PAGE_CAPACITY;

            if (column.type == DataType::INT32) {
                uint16_t data_idx = 0;
                for (uint16_t i = 0; i < num_logical_rows; ++i) {
                    value_t v;
                    if (bitmap[i/8] & (1u << (i%8))) v = value_t(int_data[data_idx++]);
                    else v = value_t();

                    curr_target_page->data[in_page_idx++] = v;
                    
                    // Handle page boundary
                    if (in_page_idx == PAGE_CAPACITY) {
                        if (++page_idx < num_pages) curr_target_page = target_col.pages[page_idx].get();
                        in_page_idx = 0;
                    }
                }
            } else if (column.type == DataType::VARCHAR) {
                if (is_long_start) {
                     auto num_chars = *reinterpret_cast<uint16_t*>(page + 2);
                     StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), static_cast<uint16_t>(src_p_idx), static_cast<uint32_t>(num_chars), true};
                     
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
                            StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), static_cast<uint16_t>(src_p_idx), packed, false};
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
    namespace views = ranges::views;

    auto& root_node = plan.nodes[plan.root];
    auto& j = std::get<JoinNode>(root_node.data);
    return execute_hash_join_root(plan, j, root_node.output_attrs);
}

void* build_context() { return nullptr; }
void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest