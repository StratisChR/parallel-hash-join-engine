#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <random>
#include <unordered_map>
#include <algorithm> 
#include "ex2_task3_hashmap.h"

// Helper Function for creating columnar tables

std::vector<column_t> create_empty_table(size_t num_cols) {
    std::vector<column_t> table;
    table.reserve(num_cols);
    for(size_t i=0; i<num_cols; ++i) {
        table.emplace_back(); 
    }
    return table;
}

std::vector<column_t> create_simple_table(size_t num_rows) {
    auto table = create_empty_table(2); 
    for (size_t i = 0; i < num_rows; i++) {
        // Keys 0, 1, 2...
        table[0].push_back(value_t(static_cast<int32_t>(i)));
        // Values (not used by hash map, but realistic)
        table[1].push_back(value_t(static_cast<int32_t>(i * 10)));
    }
    return table;
}

// Generates keys 0-9 repeatedly: 0, 1, ... 9, 0, 1, ...
std::vector<column_t> create_table_with_duplicates(size_t num_rows) {
    auto table = create_empty_table(2);
    for (size_t i = 0; i < num_rows; i++) {
        int32_t key = static_cast<int32_t>(i % 10);
        table[0].push_back(value_t(key));
        table[1].push_back(value_t(static_cast<int32_t>(i)));
    }
    return table;
}

std::vector<column_t> create_random_table(size_t num_rows, int seed = 42) {
    std::mt19937 gen(seed);
    std::uniform_int_distribution<int32_t> dist(0, 5000); 
    auto table = create_empty_table(2);
    for (size_t i = 0; i < num_rows; i++) {
        int32_t key = dist(gen);
        int32_t val = dist(gen);
        table[0].push_back(value_t(key));
        table[1].push_back(value_t(val));
    }
    return table;
}

// Helper to get value from column_t
int32_t get_int_value(const column_t& col, size_t idx) {
    return col[idx].i32;
}

// Unit Tests

TEST_CASE("UnchainedHashMap - Basic Workflow (1:1 Mapping)", "[hashmap][basic]") {
    size_t num_rows = 50;
    auto build_table = create_simple_table(num_rows);
    size_t key_column = 0;

    // Value type is size_t (Row Index). We don't want/need vectors.
    UnchainedHashMap<int32_t, size_t> hash_map;

    // BUild phase 
    REQUIRE_NOTHROW(hash_map.create_directory(build_table, key_column));

    SECTION("Probe existing keys") {
        for (size_t expected_row_idx = 0; expected_row_idx < num_rows; expected_row_idx++) {
            int32_t key = get_int_value(build_table[key_column], expected_row_idx);
            
            bool callback_called = false;
            
            // Probe using a lambda function.
            hash_map.lookup(key, [&](size_t found_idx) {
                callback_called = true;
                CHECK(found_idx == expected_row_idx);
            });
            
            CHECK(callback_called);
        }
    }

    SECTION("Probe non-existent keys") {
        // Keys 1000+ do not exist in the table
        for (int32_t key = 1000; key < 1010; key++) {
            bool callback_called = false;
            hash_map.lookup(key, [&](size_t) {
                callback_called = true; 
            });
            CHECK_FALSE(callback_called);
        }
    }
}

TEST_CASE("UnchainedHashMap - Duplicate Keys Handling", "[hashmap][duplicates]") {
    // 100 rows, keys 0-9 repeated 10 times each.
    size_t num_rows = 100;
    auto build_table = create_table_with_duplicates(num_rows);
    size_t key_column = 0;

    UnchainedHashMap<int32_t, size_t> hash_map;
    hash_map.create_directory(build_table, key_column);

    SECTION("Find all rows for a specific key") {
        int32_t target_key = 5; 
        std::vector<size_t> found_indices;

        // The probe should trigger the lambda 10 times.
        hash_map.lookup(target_key, [&](size_t idx) {
            found_indices.push_back(idx);
        });

        // Verification.
        REQUIRE(found_indices.size() == 10);
        
        // Sort to ensure we check contents regardless of retrieval order.
        std::sort(found_indices.begin(), found_indices.end());

        // We expect rows: 5, 15, 25, ... 95
        for (size_t i = 0; i < 10; i++) {
            CHECK(found_indices[i] == (i * 10 + 5));
        }
    }

    SECTION("Verify every key has exactly 10 matches") {
        for (int32_t k = 0; k < 10; k++) {
            size_t count = 0;
            hash_map.lookup(k, [&](size_t) { count++; });
            CHECK(count == 10);
        }
    }
}

TEST_CASE("UnchainedHashMap - Random Data Stress Test", "[hashmap][random]") {
    size_t num_rows = 10000;
    auto build_table = create_random_table(num_rows, 999);
    size_t key_column = 0;

    // Use unordered map to check results.
    std::unordered_map<int32_t, std::vector<size_t>> ground_truth;
    for (size_t i = 0; i < num_rows; i++) {
        int32_t key = get_int_value(build_table[key_column], i);
        ground_truth[key].push_back(i);
    }

    // Build our Unchained Map
    UnchainedHashMap<int32_t, size_t> hash_map;
    hash_map.create_directory(build_table, key_column);

    SECTION("Compare against Ground Truth") {
        for (const auto& [key, expected_indices] : ground_truth) {
            std::vector<size_t> actual_indices;
            
            hash_map.lookup(key, [&](size_t idx) {
                actual_indices.push_back(idx);
            });

            // Sort both for comparison (hash map order is stable-ish but not guaranteed sorted)
            std::vector<size_t> sorted_expected = expected_indices;
            std::sort(sorted_expected.begin(), sorted_expected.end());
            std::sort(actual_indices.begin(), actual_indices.end());

            REQUIRE(actual_indices.size() == sorted_expected.size());
            CHECK(actual_indices == sorted_expected);
        }
    }
}

TEST_CASE("UnchainedHashMap - Edge Cases", "[hashmap][edge]") {
    
    SECTION("Empty Table") {
        auto empty_table = create_empty_table(2);
        UnchainedHashMap<int32_t, size_t> hash_map;
        
        // Should not crash
        REQUIRE_NOTHROW(hash_map.create_directory(empty_table, 0));
        
        // Probe should find nothing
        bool called = false;
        hash_map.lookup(123, [&](size_t){ called = true; });
        CHECK_FALSE(called);
    }

    SECTION("Single Row Table") {
        auto table = create_simple_table(1); // Key: 0, Row: 0
        UnchainedHashMap<int32_t, size_t> hash_map;
        hash_map.create_directory(table, 0);

        bool called = false;
        hash_map.lookup(0, [&](size_t idx){ 
            called = true; 
            CHECK(idx == 0);
        });
        CHECK(called);
    }
}