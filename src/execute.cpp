#include <hardware.h>
#include <plan.h>
#include <hopscotch_hash.h>
#include <table.h>

#include "indexing.h"

#include "hopscotch_hash.h"
#include "robin_hood_hash_map.h"
#include "cuckoo_hash_map.h"
#include "int_hasher.h"

namespace Contest {

enum class JoinStrategy {
STD_UNORDERED_MAP, 
HOPSCOTCH,         // Panagiotis Papapostolou
ROBIN_HOOD,         // Theodoros Mpeioglou
CUCKOO            // Stratis Charisiadis-Roussis
};

constexpr JoinStrategy CURRENT_STRATEGY = JoinStrategy::HOPSCOTCH;

using ExecuteResult = std::vector<std::vector<value_t>>;

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// Helper: materialize a string (std::string) from a value_t containing a StringRef
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
        // first page should be 0xffff
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
    bool                                             build_left;
    ExecuteResult&                                   left;
    ExecuteResult&                                   right;
    ExecuteResult&                                   results;
    size_t                                           left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>& output_attrs;
    const Plan*                                      plan_ptr;

    // materialize a string from a value_t (which wraps a StringRef)
    std::string materialize_string(const value_t& vv) const {
        if (vv.type != value_t::Type::STRING) {
            throw std::runtime_error("materialize_string on non-string value");
        }
        auto sref = vv.str;
        if (sref.is_null()) return std::string();
        auto table_id = sref.table_id();
        auto col_id = sref.column_id();
        auto page_id = sref.page_id();
        auto off = sref.offset();
        const auto& column = plan_ptr->inputs[table_id].columns[col_id];
        // handle long strings
        if (sref.is_long()) {
            std::string out;
            size_t idx = page_id;
            // first page should be 0xffff
            while (idx < column.pages.size()) {
                auto* page = column.pages[idx]->data;
                auto num_rows = *reinterpret_cast<uint16_t*>(page);
                if (num_rows != 0xffff && num_rows != 0xfffe) break;
                auto num_chars = *reinterpret_cast<uint16_t*>(page + 2);
                auto* data_begin = reinterpret_cast<char*>(page + 4);
                out.append(data_begin, data_begin + num_chars);
                ++idx;
                if (num_rows == 0xfffe) continue;
                // if it was 0xffff then we still check following 0xfffe pages
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


    template <class T, class HasherT>
    auto run() {
        namespace views = ranges::views;

        auto create_hash_table = [&]() {
            if constexpr (CURRENT_STRATEGY == JoinStrategy::ROBIN_HOOD) {
                // Θοδωρής.
                return RobinHoodHashMap<T, std::vector<size_t>, HasherT>();
            } 
            else if constexpr (CURRENT_STRATEGY == JoinStrategy::HOPSCOTCH) {
                // Παναγιώτης.
                return HopscotchHash<T, std::vector<size_t>>();
            }
            else if constexpr (CURRENT_STRATEGY == JoinStrategy::CUCKOO) {
                // Στρατής.
                return CuckooHashMap<T, std::vector<size_t>, HasherT>();
            }
            else /* STD_UNORDERED_MAP */ {
                // Βασικό. 
                return std::unordered_map<T, std::vector<size_t>>();
            }
        };

        // Create the one hash table we will use
        auto hash_table = create_hash_table();

        if (build_left) {
            // Loop iterates over the 'left' table
            for (auto&& [idx, record]: left | views::enumerate) {
                const auto& cell = record[left_col];
                if (cell.is_null()) continue;
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (cell.type != value_t::Type::INT32) {
                        throw std::runtime_error("wrong type of field (expected INT32)");
                    }
                    auto key = static_cast<T>(cell.i32);
                    if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                        hash_table.emplace(key, std::vector<size_t>(1, idx));
                    } else {
                        itr->second.push_back(idx);
                    }
                } else if constexpr (std::is_same_v<T, std::string>) {
                    if (cell.type != value_t::Type::STRING) {
                        throw std::runtime_error("wrong type of field (expected STRING)");
                    }
                    auto key = materialize_string(cell);
                    if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                        hash_table.emplace(key, std::vector<size_t>(1, idx));
                    } else {
                        itr->second.push_back(idx);
                    }
                } else {
                    throw std::runtime_error("unsupported join key type in new engine");
                }
            }
            // Loop iterates over the 'right' table
            for (auto& right_record: right) {
                const auto& cell = right_record[right_col];
                if (cell.is_null()) continue;
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (cell.type != value_t::Type::INT32) continue;
                    auto key = static_cast<T>(cell.i32);
                    if (auto itr = hash_table.find(key); itr != hash_table.end()) {
                        for (auto left_idx: itr->second) {
                            auto& left_record = left[left_idx];
                            std::vector<value_t> new_record;
                            new_record.reserve(output_attrs.size());
                            for (auto [col_idx, _]: output_attrs) {
                                if (col_idx < left_record.size()) {
                                    new_record.emplace_back(left_record[col_idx]);
                                } else {
                                    new_record.emplace_back(
                                        right_record[col_idx - left_record.size()]);
                                }
                            }
                            results.emplace_back(std::move(new_record));
                        }
                    }
                } else if constexpr (std::is_same_v<T, std::string>) {
                    if (cell.type != value_t::Type::STRING) continue;
                    auto key = materialize_string(cell);
                    if (auto itr = hash_table.find(key); itr != hash_table.end()) {
                        for (auto left_idx: itr->second) {
                            auto& left_record = left[left_idx];
                            std::vector<value_t> new_record;
                            new_record.reserve(output_attrs.size());
                            for (auto [col_idx, _]: output_attrs) {
                                if (col_idx < left_record.size()) {
                                    new_record.emplace_back(left_record[col_idx]);
                                } else {
                                    new_record.emplace_back(
                                        right_record[col_idx - left_record.size()]);
                                }
                            }
                            results.emplace_back(std::move(new_record));
                        }
                    }
                } else {
                    throw std::runtime_error("unsupported join key type in new engine");
                }
            }
        } else {
            // Loop iterates over the 'right' table
            for (auto&& [idx, record]: right | views::enumerate) {
                const auto& cell = record[right_col];
                if (cell.is_null()) continue;
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (cell.type != value_t::Type::INT32) {
                        throw std::runtime_error("wrong type of field (expected INT32)");
                    }
                    auto key = static_cast<T>(cell.i32);
                    if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                        hash_table.emplace(key, std::vector<size_t>(1, idx));
                    } else {
                        itr->second.push_back(idx);
                    }
                } else if constexpr (std::is_same_v<T, std::string>) {
                    if (cell.type != value_t::Type::STRING) {
                        throw std::runtime_error("wrong type of field (expected STRING)");
                    }
                    auto key = materialize_string(cell);
                    if (auto itr = hash_table.find(key); itr == hash_table.end()) {
                        hash_table.emplace(key, std::vector<size_t>(1, idx));
                    } else {
                        itr->second.push_back(idx);
                    }
                } else {
                    throw std::runtime_error("unsupported join key type in new engine");
                }
            }
            // Loop iterates over the 'left' table
            for (auto& left_record: left) {
                const auto& cell = left_record[left_col];
                if (cell.is_null()) continue;
                if constexpr (std::is_same_v<T, int32_t>) {
                    if (cell.type != value_t::Type::INT32) continue;
                    auto key = static_cast<T>(cell.i32);
                    if (auto itr = hash_table.find(key); itr != hash_table.end()) {
                        for (auto right_idx: itr->second) {
                            auto& right_record = right[right_idx];
                            std::vector<value_t> new_record;
                            new_record.reserve(output_attrs.size());
                            for (auto [col_idx, _]: output_attrs) {
                                if (col_idx < left_record.size()) {
                                    new_record.emplace_back(left_record[col_idx]);
                                } else {
                                    new_record.emplace_back(
                                        right_record[col_idx - left_record.size()]);
                                }
                            }
                            results.emplace_back(std::move(new_record));
                        }
                    }
                } else if constexpr (std::is_same_v<T, std::string>) {
                    if (cell.type != value_t::Type::STRING) continue;
                    auto key = materialize_string(cell);
                    if (auto itr = hash_table.find(key); itr != hash_table.end()) {
                        for (auto right_idx: itr->second) {
                            auto& right_record = right[right_idx];
                            std::vector<value_t> new_record;
                            new_record.reserve(output_attrs.size());
                            for (auto [col_idx, _]: output_attrs) {
                                if (col_idx < left_record.size()) {
                                    new_record.emplace_back(left_record[col_idx]);
                                } else {
                                    new_record.emplace_back(
                                        right_record[col_idx - left_record.size()]);
                                }
                            }
                            results.emplace_back(std::move(new_record));
                        }
                    }
                } else {
                    throw std::runtime_error("unsupported join key type in new engine");
                }
            }
        }
    }
};

ExecuteResult execute_hash_join(const Plan&          plan,
    const JoinNode&                                  join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto                           left_idx    = join.left;
    auto                           right_idx   = join.right;
    auto&                          left_node   = plan.nodes[left_idx];
    auto&                          right_node  = plan.nodes[right_idx];
    auto&                          left_types  = left_node.output_attrs;
    auto&                          right_types = right_node.output_attrs;
    auto                           left        = execute_impl(plan, left_idx);
    auto                           right       = execute_impl(plan, right_idx);
    std::vector<std::vector<value_t>> results;

    JoinAlgorithm join_algorithm{.build_left = join.build_left,
        .left                                = left,
        .right                               = right,
        .results                             = results,
        .left_col                            = join.left_attr,
        .right_col                           = join.right_attr,
        .output_attrs                        = output_attrs,
        .plan_ptr                            = &plan};
    if (join.build_left) {
        switch (std::get<1>(left_types[join.left_attr])) {
    case DataType::INT32:   join_algorithm.run<int32_t, IntHasher>(); break;
    case DataType::VARCHAR: join_algorithm.run<std::string, std::hash<std::string>>(); break;
    default: throw std::runtime_error("unsupported join key type for build side");
        }
    } else {
        switch (std::get<1>(right_types[join.right_attr])) {
    case DataType::INT32:   join_algorithm.run<int32_t, IntHasher>(); break;
    case DataType::VARCHAR: join_algorithm.run<std::string, std::hash<std::string>>(); break;
    default: throw std::runtime_error("unsupported join key type for build side");
        }
    }

    return results;
}

// Special-case join that is at the root: directly produce ColumnarTable by
// emitting Data rows while probing (materialize strings only when they
// appear in the final output). This avoids creating an intermediate
// ExecuteResult for the final result.
ColumnarTable execute_hash_join_root(const Plan&          plan,
    const JoinNode&                                  join,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto                           left_idx    = join.left;
    auto                           right_idx   = join.right;
    auto&                          left_node   = plan.nodes[left_idx];
    auto&                          right_node  = plan.nodes[right_idx];
    auto&                          left_types  = left_node.output_attrs;
    auto&                          right_types = right_node.output_attrs;
    auto                           left        = execute_impl(plan, left_idx);
    auto                           right       = execute_impl(plan, right_idx);

    // Collect rows without variant wrapping - use row-store with direct columnar conversion
    std::vector<std::vector<value_t>> rows;
    rows.reserve(1024);

    // Build-probe logic similar to execute_hash_join but emit Data rows
    if (join.build_left) {
        // build hash on left
        auto build_key_int = std::unordered_map<int32_t, std::vector<size_t>>();
        std::unordered_map<std::string, std::vector<size_t>> build_key_str;

        // decide key type from left_types[left_attr]
        auto left_key_type = std::get<1>(left_types[join.left_attr]);
        if (left_key_type == DataType::INT32) {
            for (auto&& [idx, record] : left | ranges::views::enumerate) {
                const auto& cell = record[join.left_attr];
                if (cell.is_null()) continue;
                if (cell.type != value_t::Type::INT32) continue;
                int32_t key = static_cast<int32_t>(cell.i32);
                if (auto it = build_key_int.find(key); it == build_key_int.end()) {
                    build_key_int.emplace(key, std::vector<size_t>{idx});
                } else {
                    it->second.push_back(idx);
                }
            }

            // probe with right, collect rows without variant
            for (auto& right_record: right) {
                const auto& cell = right_record[join.right_attr];
                if (cell.is_null() || cell.type != value_t::Type::INT32) continue;
                int32_t key = static_cast<int32_t>(cell.i32);
                if (auto it = build_key_int.find(key); it != build_key_int.end()) {
                    for (auto left_idx_row: it->second) {
                        auto& left_record = left[left_idx_row];
                        std::vector<value_t> row(output_attrs.size());
                        for (size_t col_out = 0; col_out < output_attrs.size(); ++col_out) {
                            auto [col_idx, dtype] = output_attrs[col_out];
                            if (col_idx < left_record.size()) row[col_out] = left_record[col_idx];
                            else row[col_out] = right_record[col_idx - left_record.size()];
                        }
                        rows.emplace_back(std::move(row));
                    }
                }
            }
        } else if (left_key_type == DataType::VARCHAR) {
            for (auto&& [idx, record] : left | ranges::views::enumerate) {
                const auto& cell = record[join.left_attr];
                if (cell.is_null()) continue;
                if (cell.type != value_t::Type::STRING) continue;
                auto key = materialize_string_from_value(plan, cell);
                if (auto it = build_key_str.find(key); it == build_key_str.end()) {
                    build_key_str.emplace(key, std::vector<size_t>{idx});
                } else {
                    it->second.push_back(idx);
                }
            }

            for (auto& right_record: right) {
                const auto& cell = right_record[join.right_attr];
                if (cell.is_null() || cell.type != value_t::Type::STRING) continue;
                auto key = materialize_string_from_value(plan, cell);
                if (auto it = build_key_str.find(key); it != build_key_str.end()) {
                    for (auto left_idx_row: it->second) {
                        auto& left_record = left[left_idx_row];
                        std::vector<value_t> row(output_attrs.size());
                        for (size_t col_out = 0; col_out < output_attrs.size(); ++col_out) {
                            auto [col_idx, dtype] = output_attrs[col_out];
                            if (col_idx < left_record.size()) row[col_out] = left_record[col_idx];
                            else row[col_out] = right_record[col_idx - left_record.size()];
                        }
                        rows.emplace_back(std::move(row));
                    }
                }
            }
        } else {
            throw std::runtime_error("unsupported join key type for root build side");
        }
    } else {
        // build on right
        auto right_key_type = std::get<1>(right_types[join.right_attr]);
        std::unordered_map<int32_t, std::vector<size_t>> build_key_int;
        std::unordered_map<std::string, std::vector<size_t>> build_key_str;

        if (right_key_type == DataType::INT32) {
            for (auto&& [idx, record] : right | ranges::views::enumerate) {
                const auto& cell = record[join.right_attr];
                if (cell.is_null()) continue;
                if (cell.type != value_t::Type::INT32) continue;
                int32_t key = static_cast<int32_t>(cell.i32);
                if (auto it = build_key_int.find(key); it == build_key_int.end()) {
                    build_key_int.emplace(key, std::vector<size_t>{idx});
                } else {
                    it->second.push_back(idx);
                }
            }

            for (auto& left_record: left) {
                const auto& cell = left_record[join.left_attr];
                if (cell.is_null() || cell.type != value_t::Type::INT32) continue;
                int32_t key = static_cast<int32_t>(cell.i32);
                if (auto it = build_key_int.find(key); it != build_key_int.end()) {
                    for (auto right_idx_row: it->second) {
                        auto& right_record = right[right_idx_row];
                        std::vector<value_t> row(output_attrs.size());
                        for (size_t col_out = 0; col_out < output_attrs.size(); ++col_out) {
                            auto [col_idx, dtype] = output_attrs[col_out];
                            if (col_idx < left_record.size()) row[col_out] = left_record[col_idx];
                            else row[col_out] = right_record[col_idx - left_record.size()];
                        }
                        rows.emplace_back(std::move(row));
                    }
                }
            }
        } else if (right_key_type == DataType::VARCHAR) {
            for (auto&& [idx, record] : right | ranges::views::enumerate) {
                const auto& cell = record[join.right_attr];
                if (cell.is_null()) continue;
                if (cell.type != value_t::Type::STRING) continue;
                auto key = materialize_string_from_value(plan, cell);
                if (auto it = build_key_str.find(key); it == build_key_str.end()) {
                    build_key_str.emplace(key, std::vector<size_t>{idx});
                } else {
                    it->second.push_back(idx);
                }
            }

            for (auto& left_record: left) {
                const auto& cell = left_record[join.left_attr];
                if (cell.is_null() || cell.type != value_t::Type::STRING) continue;
                auto key = materialize_string_from_value(plan, cell);
                if (auto it = build_key_str.find(key); it != build_key_str.end()) {
                    for (auto right_idx_row: it->second) {
                        auto& right_record = right[right_idx_row];
                        std::vector<value_t> row(output_attrs.size());
                        for (size_t col_out = 0; col_out < output_attrs.size(); ++col_out) {
                            auto [col_idx, dtype] = output_attrs[col_out];
                            if (col_idx < left_record.size()) row[col_out] = left_record[col_idx];
                            else row[col_out] = right_record[col_idx - left_record.size()];
                        }
                        rows.emplace_back(std::move(row));
                    }
                }
            }
        } else {
            throw std::runtime_error("unsupported join key type for root build side");
        }
    }

    // Convert rows (value_t row-store) directly to ColumnarTable
    // by constructing Data rows only for final materialization
    std::vector<std::vector<Data>> converted;
    converted.reserve(rows.size());

    for (auto& row: rows) {
        std::vector<Data> data_row(row.size());
        for (size_t col = 0; col < row.size(); ++col) {
            auto& vv = row[col];
            auto dtype = std::get<1>(output_attrs[col]);
            if (dtype == DataType::INT32) {
                if (vv.type == value_t::Type::INT32) {
                    data_row[col] = Data(static_cast<int32_t>(vv.i32));
                } else {
                    data_row[col] = Data(std::monostate{});
                }
            } else if (dtype == DataType::VARCHAR) {
                if (vv.type == value_t::Type::STRING && !vv.str.is_null()) {
                    auto s = materialize_string_from_value(plan, vv);
                    data_row[col] = Data(std::move(s));
                } else {
                    data_row[col] = Data(std::monostate{});
                }
            } else {
                data_row[col] = Data(std::monostate{});
            }
        }
        converted.emplace_back(std::move(data_row));
    }

    auto ret_types = output_attrs | ranges::views::transform([](const auto& v) { return std::get<1>(v); })
                     | ranges::to<std::vector<DataType>>();
    Table table{std::move(converted), std::move(ret_types)};
    return table.to_columnar();
}

ExecuteResult execute_scan(const Plan&               plan,
    const ScanNode&                                  scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto                           table_id = scan.base_table_id;
    auto&                          input    = plan.inputs[table_id];
    // Produce a row-store of value_t: ints materialized, strings as StringRef
    namespace views = ranges::views;
    ExecuteResult results(input.num_rows, std::vector<value_t>(output_attrs.size()));

    for (size_t out_col = 0; out_col < output_attrs.size(); ++out_col) {
        auto in_col_idx = std::get<0>(output_attrs[out_col]);
        auto& column = input.columns[in_col_idx];
        size_t row_idx = 0;
        size_t page_idx = 0;
        for (auto* page_obj : column.pages) {
            auto* page = page_obj->data;
            auto num_rows = *reinterpret_cast<uint16_t*>(page);
            if (column.type == DataType::INT32) {
                auto* data_begin = reinterpret_cast<int32_t*>(page + 4);
                auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                uint16_t data_idx = 0;
                for (uint16_t i = 0; i < num_rows; ++i) {
                    if (bitmap[i/8] & (1u << (i%8))) {
                        auto value = data_begin[data_idx++];
                        if (row_idx >= input.num_rows) throw std::runtime_error("row_idx");
                        results[row_idx][out_col] = value_t(static_cast<int32_t>(value));
                    }
                    ++row_idx;
                }
            } else if (column.type == DataType::VARCHAR) {
                if (num_rows == 0xffff) {
                    // first page of a long string
                    auto num_chars = *reinterpret_cast<uint16_t*>(page + 2);
                    auto* data_begin = reinterpret_cast<char*>(page + 4);
                    // create StringRef pointing to this page
                    if (row_idx >= input.num_rows) throw std::runtime_error("row_idx");
                    StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), static_cast<uint16_t>(page_idx), static_cast<uint32_t>(num_chars), true};
                    results[row_idx][out_col] = value_t(s);
                    ++row_idx;
                } else if (num_rows == 0xfffe) {
                    // continuation page for previous long string: nothing to do for row index
                    // long strings are referenced by their starting page; skip
                } else {
                    auto num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
                    auto* offset_begin = reinterpret_cast<uint16_t*>(page + 4);
                    auto* data_begin = reinterpret_cast<char*>(page + 4 + num_non_null * 2);
                    auto* bitmap = reinterpret_cast<uint8_t*>(page + PAGE_SIZE - (num_rows + 7) / 8);
                    uint16_t data_idx = 0;
                    uint32_t prev_end = 0;
                    for (uint16_t i = 0; i < num_rows; ++i) {
                        if (bitmap[i/8] & (1u << (i%8))) {
                            auto end_off = offset_begin[data_idx++];
                            uint32_t start = prev_end;
                            uint32_t len = static_cast<uint32_t>(end_off) - prev_end;
                            prev_end = end_off;
                            if (row_idx >= input.num_rows) throw std::runtime_error("row_idx");
                            // pack start and len into 30-bit offset: start<<16 | len
                            uint32_t packed = (start << 16) | (len & 0xFFFF);
                            StringRef s{static_cast<uint8_t>(table_id), static_cast<uint8_t>(in_col_idx), static_cast<uint16_t>(page_idx), packed, false};
                            results[row_idx][out_col] = value_t(s);
                        }
                        ++row_idx;
                    }
                }
            } else {
                // unsupported types for new row-store -> leave nulls
                row_idx += num_rows;
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
    // If the root is a Join node, use the specialized root join that emits the
    // final ColumnarTable directly (materializing strings only for final outputs).
    auto& root_node = plan.nodes[plan.root];
    if (std::holds_alternative<JoinNode>(root_node.data)) {
        auto& j = std::get<JoinNode>(root_node.data);
        return execute_hash_join_root(plan, j, root_node.output_attrs);
    }

    // otherwise fall back to executing the plan and converting the row-store
    // result (value_t) into a ColumnarTable (legacy path)
    auto ret = execute_impl(plan, plan.root);
    auto ret_types = plan.nodes[plan.root].output_attrs
                   | views::transform([](const auto& v) { return std::get<1>(v); })
                   | ranges::to<std::vector<DataType>>();

    // Convert row-store of value_t back into Data (materialize strings)
    std::vector<std::vector<Data>> converted;
    converted.reserve(ret.size());
    for (auto& row: ret) {
        std::vector<Data> crec;
        crec.reserve(row.size());
        for (size_t col = 0; col < row.size(); ++col) {
            auto& vv = row[col];
            auto dtype = ret_types[col];
            if (dtype == DataType::INT32) {
                if (vv.type == value_t::Type::INT32) {
                    crec.emplace_back(static_cast<int32_t>(vv.i32));
                } else {
                    crec.emplace_back(std::monostate{});
                }
            } else if (dtype == DataType::VARCHAR) {
                if (vv.type == value_t::Type::STRING) {
                    // materialize string from StringRef
                    StringRef sref = vv.str;
                    if (sref.is_null()) {
                        crec.emplace_back(std::monostate{});
                    } else {
                        const auto& column = plan.inputs[sref.table_id()].columns[sref.column_id()];
                        if (sref.is_long()) {
                            std::string out;
                            size_t idx = sref.page_id();
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
                            crec.emplace_back(std::move(out));
                        } else {
                            auto page = column.pages[sref.page_id()]->data;
                            auto num_non_null = *reinterpret_cast<uint16_t*>(page + 2);
                            auto* data_begin = reinterpret_cast<char*>(page + 4 + num_non_null * 2);
                            uint32_t packed = sref.offset();
                            uint32_t start = static_cast<uint32_t>(packed >> 16);
                            uint32_t len = static_cast<uint32_t>(packed & 0xFFFF);
                            crec.emplace_back(std::string(data_begin + start, data_begin + start + len));
                        }
                    }
                } else {
                    crec.emplace_back(std::monostate{});
                }
            } else {
                // unsupported types: return NULL
                crec.emplace_back(std::monostate{});
            }
        }
        converted.emplace_back(std::move(crec));
    }

    Table table{std::move(converted), std::move(ret_types)};
    return table.to_columnar();
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {}

} // namespace Contest