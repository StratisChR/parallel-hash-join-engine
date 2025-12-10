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
#include "ex2_task3_hashmap.h"

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
    if (vv.type != value_t::Type::STRING) {
        throw std::runtime_error("materialize_string on non-string value");
    }
    auto sref = vv.str;
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

    template <class T, class HasherT>
    auto run() {
        namespace views = ranges::views;

        auto create_hash_table = [&]() {
            if constexpr (CURRENT_STRATEGY == JoinStrategy::ROBIN_HOOD) {
                return RobinHoodHashMap<T, std::vector<size_t>, HasherT>();
            } 
            else if constexpr (CURRENT_STRATEGY == JoinStrategy::HOPSCOTCH) {
                return HopscotchHash<T, std::vector<size_t>>();
            }
            else if constexpr (CURRENT_STRATEGY == JoinStrategy::CUCKOO) {
                return CuckooHashMap<T, std::vector<size_t>, HasherT>();
            }
            else {
                return std::unordered_map<T, std::vector<size_t>>();
            }
        };

        // auto hash_table = create_hash_table();
        auto hash_table = UnchainedHashMap<T, std::vector<size_t>>();

        if (build_left) {
            hash_table.create_directory(left, left_col);

            //build left
            auto& key_col_obj = left[left_col];
            size_t total_rows = key_col_obj.size();
            size_t global_idx = 0;

            for (const auto& page_ptr : key_col_obj.pages) {
                value_t* data = page_ptr->data;
                size_t limit = (global_idx + PAGE_CAPACITY > total_rows) ? total_rows - global_idx : PAGE_CAPACITY;

                for (size_t i = 0; i < limit; ++i) {
                    value_t cell = data[i];
                    size_t idx = global_idx + i; 

                    if (cell.is_null()) continue;
                    
                    T key;
                    if constexpr (std::is_same_v<T, int32_t>) {
                        if (cell.type != value_t::Type::INT32) throw std::runtime_error("wrong type");
                        key = static_cast<T>(cell.i32);
                    } 
                    // else {
                    //     if (cell.type != value_t::Type::STRING) throw std::runtime_error("wrong type");
                    //     key = materialize_string(cell);
                    // }


                    hash_table.insert(key, std::vector<size_t>(1, idx));
                    // if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                    //     hash_table.emplace(key, std::vector<size_t>(1, idx));
                    // } else {
                    //     itr->second.push_back(idx);
                    // }
                }
                global_idx += limit;
            }

            // probe right
            auto& probe_col_obj = right[right_col];
            total_rows = probe_col_obj.size();
            global_idx = 0;

            for (const auto& page_ptr : probe_col_obj.pages) {
                value_t* data = page_ptr->data;
                size_t limit = (global_idx + PAGE_CAPACITY > total_rows) ? total_rows - global_idx : PAGE_CAPACITY;

                for (size_t i = 0; i < limit; ++i) {
                    value_t cell = data[i];
                    size_t right_idx_val = global_idx + i; 

                    if (cell.is_null()) continue;
                    bool type_ok = true;
                    if constexpr (std::is_same_v<T, int32_t>) type_ok = (cell.type == value_t::Type::INT32);
                    // else type_ok = (cell.type == value_t::Type::STRING);
                    if (!type_ok) continue;

                    T key;
                    if constexpr (std::is_same_v<T, int32_t>) key = static_cast<T>(cell.i32);
                    // else key = materialize_string(cell);

                    if (auto* itr = hash_table.lookup(key); itr != nullptr) {
                        for (auto left_idx : itr->second) {
                            // Construct Result (Column Store)
                            size_t left_table_cols = left.size();
                            for (size_t out_i = 0; out_i < output_attrs.size(); ++out_i) {
                                auto [src_col, _] = output_attrs[out_i];
                                if (src_col < left_table_cols) {
                                    results[out_i].push_back(left[src_col][left_idx]);
                                } else {
                                    results[out_i].push_back(right[src_col - left_table_cols][right_idx_val]);
                                }
                            }
                        }
                    }
                }
                global_idx += limit;
            }
        } else {
            hash_table.create_directory(right, right_col);
            // build right
            auto& key_col_obj = right[right_col];
            size_t total_rows = key_col_obj.size();
            size_t global_idx = 0;

            for (const auto& page_ptr : key_col_obj.pages) {
                value_t* data = page_ptr->data;
                size_t limit = (global_idx + PAGE_CAPACITY > total_rows) ? total_rows - global_idx : PAGE_CAPACITY;

                for (size_t i = 0; i < limit; ++i) {
                    value_t cell = data[i];
                    size_t idx = global_idx + i;
                    if (cell.is_null()) continue;
                    
                    T key;
                    if constexpr (std::is_same_v<T, int32_t>) {
                        if (cell.type != value_t::Type::INT32) throw std::runtime_error("wrong type");
                        key = static_cast<T>(cell.i32);
                    } 
                    // else {
                    //     if (cell.type != value_t::Type::STRING) throw std::runtime_error("wrong type");
                    //     key = materialize_string(cell);
                    // }

                    hash_table.insert(key, std::vector<size_t>(1, idx));
                    // if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                    //     hash_table.emplace(key, std::vector<size_t>(1, idx));
                    // } else {
                    //     itr->second.push_back(idx);
                    // }
                }
                global_idx += limit;
            }

            // probe left
            auto& probe_col_obj = left[left_col];
            total_rows = probe_col_obj.size();
            global_idx = 0;

            for (const auto& page_ptr : probe_col_obj.pages) {
                value_t* data = page_ptr->data;
                size_t limit = (global_idx + PAGE_CAPACITY > total_rows) ? total_rows - global_idx : PAGE_CAPACITY;

                for (size_t i = 0; i < limit; ++i) {
                    value_t cell = data[i];
                    size_t left_idx_val = global_idx + i; 

                    if (cell.is_null()) continue;
                    bool type_ok = true;
                    if constexpr (std::is_same_v<T, int32_t>) type_ok = (cell.type == value_t::Type::INT32);
                    // else type_ok = (cell.type == value_t::Type::STRING);
                    if (!type_ok) continue;

                    T key;
                    if constexpr (std::is_same_v<T, int32_t>) key = static_cast<T>(cell.i32);
                    // else key = materialize_string(cell);

                    if (auto* itr = hash_table.lookup(key); itr != nullptr) {
                        for (auto right_idx : itr->second) {
                            size_t left_table_cols = left.size();
                            for (size_t out_i = 0; out_i < output_attrs.size(); ++out_i) {
                                auto [src_col, _] = output_attrs[out_i];
                                if (src_col < left_table_cols) {
                                    results[out_i].push_back(left[src_col][left_idx_val]);
                                } else {
                                    results[out_i].push_back(right[src_col - left_table_cols][right_idx]);
                                }
                            }
                        }
                    }
                }
                global_idx += limit;
            }
        }
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
            case DataType::INT32:   join_algorithm.run<int32_t, IntHasher>(); break;
            // case DataType::VARCHAR: join_algorithm.run<std::string, std::hash<std::string>>(); break;
            default: throw std::runtime_error("unsupported join key type");
        }
    } else {
        switch (std::get<1>(right_types[join.right_attr])) {
            case DataType::INT32:   join_algorithm.run<int32_t, IntHasher>(); break;
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

    std::vector<std::vector<Data>> final_columns(output_attrs.size());
    for(auto& col : final_columns) col.reserve(1024); 

    auto run_root_join = [&](auto type_tag, auto hasher_tag) {
        using T = typename decltype(type_tag)::type;
        using HasherT = typename decltype(hasher_tag)::type;
        
        auto create_hash_table = [&]() {
            if constexpr (CURRENT_STRATEGY == JoinStrategy::ROBIN_HOOD) return RobinHoodHashMap<T, std::vector<size_t>, HasherT>();
            else if constexpr (CURRENT_STRATEGY == JoinStrategy::HOPSCOTCH) return HopscotchHash<T, std::vector<size_t>>();
            else if constexpr (CURRENT_STRATEGY == JoinStrategy::CUCKOO) return CuckooHashMap<T, std::vector<size_t>, HasherT>();
            else return std::unordered_map<T, std::vector<size_t>>();
        };
        // auto hash_table = create_hash_table(); 
        auto hash_table = UnchainedHashMap<T, std::vector<size_t>>();

        // build
        ExecuteResult& build_table = join.build_left ? left : right;
        size_t build_col_idx = join.build_left ? join.left_attr : join.right_attr;
        auto& build_column = build_table[build_col_idx];

        size_t total_rows = build_column.size();
        size_t global_idx = 0;

        hash_table.create_directory(build_table, build_col_idx);

        for (const auto& page_ptr : build_column.pages) {
            value_t* data = page_ptr->data;
            size_t limit = (global_idx + PAGE_CAPACITY > total_rows) ? total_rows - global_idx : PAGE_CAPACITY;
            for (size_t i = 0; i < limit; ++i) {
                value_t cell = data[i];
                if (cell.is_null()) continue;
                T key;
                if constexpr (std::is_same_v<T, int32_t>) key = static_cast<T>(cell.i32);
                // else key = materialize_string_from_value(plan, cell);
                
                hash_table.insert(key, std::vector<size_t>{global_idx + i});
                // if (auto it = hash_table.find(key); it == hash_table.end()) {
                //     hash_table.emplace(key, std::vector<size_t>{global_idx + i});
                // } else {
                //     it->second.push_back(global_idx + i);
                // }
            }
            global_idx += limit;
        }

        // probe
        ExecuteResult& probe_table = join.build_left ? right : left;
        size_t probe_col_idx = join.build_left ? join.right_attr : join.left_attr;
        auto& probe_column = probe_table[probe_col_idx];
        total_rows = probe_column.size();
        global_idx = 0;
        size_t left_table_cols = left.size();

        for (const auto& page_ptr : probe_column.pages) {
            value_t* data = page_ptr->data;
            size_t limit = (global_idx + PAGE_CAPACITY > total_rows) ? total_rows - global_idx : PAGE_CAPACITY;
            for (size_t i = 0; i < limit; ++i) {
                value_t cell = data[i];
                if (cell.is_null()) continue;
                bool type_ok = true;
                if constexpr (std::is_same_v<T, int32_t>) type_ok = (cell.type == value_t::Type::INT32);
                else type_ok = (cell.type == value_t::Type::STRING);
                if (!type_ok) continue;

                T key;
                if constexpr (std::is_same_v<T, int32_t>) key = static_cast<T>(cell.i32);
                // else key = materialize_string_from_value(plan, cell);

                // if (auto it = hash_table.find(key); it != hash_table.end()) {
                if (auto* it = hash_table.lookup(key); it != nullptr) {
                    for (auto build_idx : it->second) {
                        size_t current_probe_idx = global_idx + i;
                        size_t left_idx  = join.build_left ? build_idx : current_probe_idx;
                        size_t right_idx = join.build_left ? current_probe_idx : build_idx;

                        for (size_t out_i = 0; out_i < output_attrs.size(); ++out_i) {
                            auto [src_col, dtype] = output_attrs[out_i];
                            value_t val;
                            if (src_col < left_table_cols) val = left[src_col][left_idx];
                            else val = right[src_col - left_table_cols][right_idx];

                            if (val.is_null()) {
                                final_columns[out_i].emplace_back(std::monostate{});
                            } else if (dtype == DataType::INT32) {
                                final_columns[out_i].emplace_back(static_cast<int32_t>(val.i32));
                            } else {
                                final_columns[out_i].emplace_back(materialize_string_from_value(plan, val));
                            }
                        }
                    }
                }
            }
            global_idx += limit;
        }
    };

    struct IntTag { using type = int32_t; };
    struct StrTag { using type = std::string; };
    struct IntHasherTag { using type = IntHasher; }; 
    struct StrHasherTag { using type = std::hash<std::string>; };

    DataType key_type = join.build_left ? std::get<1>(left_types[join.left_attr]) 
                                        : std::get<1>(right_types[join.right_attr]);

    if (key_type == DataType::INT32) run_root_join(IntTag{}, IntHasherTag{});
    // else run_root_join(StrTag{}, StrHasherTag{});

    // Transpose to Rows
    size_t num_out_rows = final_columns.empty() ? 0 : final_columns[0].size();
    std::vector<std::vector<Data>> rows(num_out_rows);
    for(size_t r=0; r<num_out_rows; ++r) {
        rows[r].resize(output_attrs.size());
        for(size_t c=0; c<output_attrs.size(); ++c) {
            rows[r][c] = std::move(final_columns[c][r]);
        }
    }

    auto ret_types = output_attrs | ranges::views::transform([](const auto& v) { return std::get<1>(v); })
                      | ranges::to<std::vector<DataType>>();
    Table table{std::move(rows), std::move(ret_types)};
    return table.to_columnar();
}


ExecuteResult execute_scan(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {

    auto table_id = scan.base_table_id;
    auto& input   = plan.inputs[table_id];

    ExecuteResult results;
    results.reserve(output_attrs.size());
    for(size_t i = 0; i < output_attrs.size(); ++i) {
        results.emplace_back(); 
    }

    for (size_t out_col = 0; out_col < output_attrs.size(); ++out_col) {
        auto in_col_idx = std::get<0>(output_attrs[out_col]);
        auto& column    = input.columns[in_col_idx];
        auto& target_col = results[out_col];

        size_t page_idx = 0;
        for (auto* page_obj : column.pages) {
            auto* page = page_obj->data;
            auto num_rows = *reinterpret_cast<uint16_t*>(page);

            if (column.type == DataType::INT32) {
                auto* data_begin = reinterpret_cast<int32_t*>(page + 4);
                auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                uint16_t data_idx = 0;
                for (uint16_t i = 0; i < num_rows; ++i) {
                    if (bitmap[i/8] & (1u << (i%8))) target_col.push_back(value_t(data_begin[data_idx++]));
                    else target_col.push_back(value_t());
                }
            } else if (column.type == DataType::VARCHAR) {
                if (num_rows == 0xffff) {
                    auto num_chars = *reinterpret_cast<uint16_t*>(page + 2);
                    StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), static_cast<uint16_t>(page_idx), static_cast<uint32_t>(num_chars), true};
                    target_col.push_back(value_t(s));
                } else if (num_rows != 0xfffe) {
                    auto num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
                    auto* offset_begin = reinterpret_cast<uint16_t*>(page + 4);
                    auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    uint32_t prev_end = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (bitmap[i/8] & (1u << (i%8))) {
                            auto end_off = offset_begin[data_idx++];
                            uint32_t len = static_cast<uint32_t>(end_off) - prev_end;
                            uint32_t packed = (prev_end << 16) | (len & 0xFFFF);
                            prev_end = end_off;
                            StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), static_cast<uint16_t>(page_idx), packed, false};
                            target_col.push_back(value_t(s));
                        } else {
                            target_col.push_back(value_t());
                        }
                    }
                }
            } else {
                for(uint16_t i=0; i<num_rows; ++i) target_col.push_back(value_t());
            }
            ++page_idx;
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
    if (std::holds_alternative<JoinNode>(root_node.data)) {
        auto& j = std::get<JoinNode>(root_node.data);
        return execute_hash_join_root(plan, j, root_node.output_attrs);
    }

    // fallback for Scans/Filters
    auto ret = execute_impl(plan, plan.root);
    auto ret_types = plan.nodes[plan.root].output_attrs
                   | views::transform([](const auto& v) { return std::get<1>(v); })
                   | ranges::to<std::vector<DataType>>();

    size_t num_rows = ret.empty() ? 0 : ret[0].size();
    std::vector<std::vector<Data>> converted(num_rows);
    for(auto& r : converted) r.resize(ret.size());

    for (size_t col_idx = 0; col_idx < ret.size(); ++col_idx) {
        auto& column = ret[col_idx];
        DataType dtype = ret_types[col_idx];
        size_t row_idx = 0;
        for (const auto& page_ptr : column.pages) {
            size_t limit = (row_idx + PAGE_CAPACITY > num_rows) ? num_rows - row_idx : PAGE_CAPACITY;
            for (size_t i = 0; i < limit; ++i) {
                value_t vv = page_ptr->data[i];
                if (dtype == DataType::INT32) {
                    if (!vv.is_null() && vv.type == value_t::Type::INT32) converted[row_idx + i][col_idx] = Data(static_cast<int32_t>(vv.i32));
                    else converted[row_idx + i][col_idx] = Data(std::monostate{});
                } else {
                    if (!vv.is_null() && vv.type == value_t::Type::STRING) converted[row_idx + i][col_idx] = Data(materialize_string_from_value(plan, vv));
                    else converted[row_idx + i][col_idx] = Data(std::monostate{});
                }
            }
            row_idx += limit;
        }
    }
    Table table{std::move(converted), std::move(ret_types)};
    return table.to_columnar();
}

void* build_context() { return nullptr; }
void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest