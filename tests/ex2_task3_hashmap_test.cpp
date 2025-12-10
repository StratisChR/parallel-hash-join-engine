#include <catch2/catch_test_macros.hpp>
#include <vector>
#include <random>
#include <unordered_map>
#include <algorithm> 
#include "ex2_task3_hashmap.h"

// Helper functions for Columnar tables

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
        table[0].push_back(value_t(static_cast<int32_t>(i)));
        table[1].push_back(value_t(static_cast<int32_t>(i * 10)));
    }
    return table;
}

// iterates keys 0-9 
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
    std::uniform_int_distribution<int32_t> dist(0, 10000);
    auto table = create_empty_table(2);
    for (size_t i = 0; i < num_rows; i++) {
        int32_t key = dist(gen);
        int32_t val = dist(gen);
        table[0].push_back(value_t(key));
        table[1].push_back(value_t(val));
    }
    return table;
}

// Helper to get value from column_t at specific index
int32_t get_int_value(const column_t& col, size_t idx) {
    return col[idx].i32;
}


// Tests

TEST_CASE("UnchainedHashMap - Basic workflow with small table", "[hashmap][basic]") {
    size_t num_rows = 10;
    auto build_table = create_simple_table(num_rows);
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;

    SECTION("Create directory and insert values") {
        hash_map.create_directory(build_table, key_column);

        for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
            int32_t key = get_int_value(build_table[key_column], row_idx);
            hash_map.insert(key, std::vector<size_t>{row_idx});
        }

        SECTION("Lookup all existing keys") {
            for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
                int32_t key = get_int_value(build_table[key_column], row_idx);
                auto* result = hash_map.lookup(key);
                
                REQUIRE(result != nullptr);
                CHECK(result->first == key);
                REQUIRE(result->second.size() == 1);
                CHECK(result->second[0] == row_idx);
            }
        }

        SECTION("Lookup non-existent keys") {
            for (int32_t key = 100; key < 110; key++) {
                auto* result = hash_map.lookup(key);
                CHECK(result == nullptr);
            }
        }
    }
}

TEST_CASE("UnchainedHashMap - Medium table workflow", "[hashmap][medium]") {
    size_t num_rows = 100;
    auto build_table = create_simple_table(num_rows);
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;
    hash_map.create_directory(build_table, key_column);

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        hash_map.insert(key, std::vector<size_t>{row_idx});
    }

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        auto* result = hash_map.lookup(key);
        
        REQUIRE(result != nullptr);
        CHECK(result->first == key);
        REQUIRE(result->second.size() == 1);
        CHECK(result->second[0] == row_idx);
    }
}

TEST_CASE("UnchainedHashMap - Large table workflow", "[hashmap][large]") {
    size_t num_rows = 10000;
    auto build_table = create_simple_table(num_rows);
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;
    hash_map.create_directory(build_table, key_column);

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        hash_map.insert(key, std::vector<size_t>{row_idx});
    }

    SECTION("Sample lookups across the table") {
        for (size_t row_idx = 0; row_idx < num_rows; row_idx += 100) {
            int32_t key = get_int_value(build_table[key_column], row_idx);
            auto* result = hash_map.lookup(key);
            
            REQUIRE(result != nullptr);
            CHECK(result->first == key);
            REQUIRE(result->second.size() == 1);
            CHECK(result->second[0] == row_idx);
        }
    }

    SECTION("Lookup first and last elements") {
        auto* first = hash_map.lookup(0);
        REQUIRE(first != nullptr);
        CHECK(first->first == 0);
        
        bool found_0 = false;
        for(auto idx : first->second) if(idx == 0) found_0 = true;
        CHECK(found_0);

        auto* last = hash_map.lookup(9999);
        REQUIRE(last != nullptr);
        CHECK(last->first == 9999);
        bool found_last = false;
        for(auto idx : last->second) if(idx == 9999) found_last = true;
        CHECK(found_last);
    }
}

TEST_CASE("UnchainedHashMap - Duplicate keys", "[hashmap][duplicates]") {
    size_t num_rows = 100;
    auto build_table = create_table_with_duplicates(num_rows);
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;
    hash_map.create_directory(build_table, key_column);

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        // Duplicate handling happens inside insert()
        hash_map.insert(key, std::vector<size_t>{row_idx});
    }

    SECTION("Lookup each unique key and verify all row indices") {
        for (int32_t key = 0; key < 10; key++) {
            auto* result = hash_map.lookup(key);
            REQUIRE(result != nullptr);
            CHECK(result->first == key);
            
            REQUIRE(result->second.size() == 10);
            
            for (size_t i = 0; i < result->second.size(); i++) {
                CHECK((result->second[i] % 10) == static_cast<size_t>(key));
            }
        }
    }
}

TEST_CASE("UnchainedHashMap - Random data", "[hashmap][random]") {
    size_t num_rows = 1000;
    auto build_table = create_random_table(num_rows, 42);
    size_t key_column = 0;

    std::unordered_map<int32_t, std::vector<size_t>> ground_truth;
    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        ground_truth[key].push_back(row_idx);
    }

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;
    hash_map.create_directory(build_table, key_column);

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        hash_map.insert(key, std::vector<size_t>{row_idx});
    }

    SECTION("Verify all keys and their row indices") {
        for (const auto& [key, expected_rows] : ground_truth) {
            auto* result = hash_map.lookup(key);
            REQUIRE(result != nullptr);
            CHECK(result->first == key);
            REQUIRE(result->second.size() == expected_rows.size());
            
            for (size_t i = 0; i < expected_rows.size(); i++) {
                CHECK(result->second[i] == expected_rows[i]);
            }
        }
    }
}

TEST_CASE("UnchainedHashMap - Empty table", "[hashmap][edge]") {
    auto empty_table = create_empty_table(2); 
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;
    hash_map.create_directory(empty_table, key_column);

    SECTION("Lookup in empty table returns nullptr") {
        auto* result = hash_map.lookup(0);
        CHECK(result == nullptr);
        
        result = hash_map.lookup(100);
        CHECK(result == nullptr);
    }
}

TEST_CASE("UnchainedHashMap - Single element", "[hashmap][edge]") {
    auto build_table = create_simple_table(1);
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;
    hash_map.create_directory(build_table, key_column);

    int32_t key = get_int_value(build_table[key_column], 0);
    hash_map.insert(key, std::vector<size_t>{0});

    SECTION("Lookup single element") {
        auto* result = hash_map.lookup(key);
        REQUIRE(result != nullptr);
        CHECK(result->first == key);
        REQUIRE(result->second.size() == 1);
        CHECK(result->second[0] == 0);
    }

    SECTION("Lookup non-existent element") {
        auto* result = hash_map.lookup(key + 1);
        CHECK(result == nullptr);
    }
}

TEST_CASE("UnchainedHashMap - Directory rehashing stress test", "[hashmap][stress]") {
    auto build_table = create_empty_table(2);
    size_t num_rows = 50;
    
    for (int i = 0; i < 50; i++) {
        build_table[0].push_back(value_t(static_cast<int32_t>(i)));
        build_table[1].push_back(value_t(static_cast<int32_t>(i * 100)));
    }
    
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;
    hash_map.create_directory(build_table, key_column);

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        hash_map.insert(key, std::vector<size_t>{row_idx});
    }

    for (size_t row_idx = 0; row_idx < num_rows; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        auto* result = hash_map.lookup(key);
        
        REQUIRE(result != nullptr);
        CHECK(result->first == key);
        REQUIRE(result->second.size() == 1);
        CHECK(result->second[0] == row_idx);
    }
}

TEST_CASE("UnchainedHashMap - Complete workflow verification", "[hashmap][workflow]") {
    const size_t TABLE_SIZE = 500;
    auto build_table = create_simple_table(TABLE_SIZE);
    size_t key_column = 0;

    UnchainedHashMap<int32_t, std::vector<size_t>> hash_map;

    SECTION("Phase 1: Create directory") {
        REQUIRE_NOTHROW(hash_map.create_directory(build_table, key_column));
    }

    hash_map.create_directory(build_table, key_column);

    SECTION("Phase 2: Insert all values") {
        for (size_t row_idx = 0; row_idx < TABLE_SIZE; row_idx++) {
            int32_t key = get_int_value(build_table[key_column], row_idx);
            REQUIRE_NOTHROW(hash_map.insert(key, std::vector<size_t>{row_idx}));
        }
    }
    
    for (size_t row_idx = 0; row_idx < TABLE_SIZE; row_idx++) {
        int32_t key = get_int_value(build_table[key_column], row_idx);
        hash_map.insert(key, std::vector<size_t>{row_idx});
    }

    SECTION("Phase 3: Probe - verify all inserts") {
        size_t found_count = 0;
        for (size_t row_idx = 0; row_idx < TABLE_SIZE; row_idx++) {
            int32_t key = get_int_value(build_table[key_column], row_idx);
            auto* result = hash_map.lookup(key);
            
            if (result != nullptr && result->first == key && 
                result->second.size() == 1 && result->second[0] == row_idx) {
                found_count++;
            }
        }
        CHECK(found_count == TABLE_SIZE);
    }
}