#include <hopscotch_hash.h>
#include <catch2/catch_test_macros.hpp>



TEST_CASE("HopscotchHash basic insert/find", "[hopscotch]") {
    HopscotchHash<int, std::string> table(8);

    REQUIRE(table.empty());
    REQUIRE(table.size() == 0);

    table.emplace(1, "A");
    table.emplace(2, "B");
    table.emplace(3, "C");

    REQUIRE_FALSE(table.empty());
    REQUIRE(table.size() == 3);

    auto it = table.find(1);
    REQUIRE(it != table.end());
    REQUIRE(it->second == "A");

    it = table.find(2);
    REQUIRE(it != table.end());
    REQUIRE(it->second == "B");

    it = table.find(99);
    REQUIRE(it == table.end());
}

TEST_CASE("HopscotchHash handles duplicates correctly", "[hopscotch][duplicates]") {
    HopscotchHash<int, std::vector<int>> table(16);

    // First insert
    auto [it1, inserted1] = table.emplace(10, std::vector<int>{0});
    REQUIRE(inserted1);
    REQUIRE(it1 != table.end());
    REQUIRE(it1->second.size() == 1);

    // Insert duplicate key (simulate join duplicate)
    auto it2 = table.find(10);
    REQUIRE(it2 != table.end());
    it2->second.push_back(1);  // manually append to vector

    // Check stored duplicates
    it2 = table.find(10);
    REQUIRE(it2->second.size() == 2);
    REQUIRE(it2->second[0] == 0);
    REQUIRE(it2->second[1] == 1);

    // Another distinct key
    table.emplace(20, std::vector<int>{2});
    REQUIRE(table.size() == 2);
}

TEST_CASE("HopscotchHash rehash behavior", "[hopscotch][rehash]") {
    HopscotchHash<int, int> table(4);

    for (int i = 0; i < 20; ++i)
        table.emplace(i, i * 10);

    REQUIRE(table.size() == 20);

    for (int i = 0; i < 20; i += 5) {
        auto it = table.find(i);
        REQUIRE(it != table.end());
        REQUIRE(it->second == i * 10);
    }
}

TEST_CASE("HopscotchHash hop-info bits set correctly (debug accessors)", "[hopscotch][bitmap]") {
    HopscotchHash<int, int> table(8);

    table.emplace(1, 100);
    table.emplace(9, 200); // likely collides mod 8 → stored within neighborhood

    size_t home = table.debug_bucket_index(1);
    const auto &t = table.debug_table();
    size_t m = table.debug_mask();

    uint32_t hop = t[home].hop_info;
    REQUIRE(hop != 0u);

    // For each set bit in hop, the corresponding bucket must be occupied
    while (hop) {
        unsigned int bit = __builtin_ctz(hop);
        size_t pos = (home + bit) & m;
        REQUIRE(t[pos].occupied);
        hop &= ~(1u << bit);
    }
}




// Test helpers for debugging and readability
static inline std::vector<int> get_occupied_indices(const HopscotchHash<int,int>& table) {
    std::vector<int> result;
    const auto& t = table.debug_table();
    for (size_t i = 0; i < t.size(); ++i)
        if (t[i].occupied)
            result.push_back(static_cast<int>(i));
    return result;
}

TEST_CASE("HopscotchHash: final table structure is consistent", "[hopscotch][structure]") {
    HopscotchHash<int,int> table(16);

    // Insert several keys with predictable hash collisions
    // Assuming hash<int> is identity for small ints
    table.emplace(1, 10);
    table.emplace(17, 20);  // collides with 1 (same home = 1 mod 16)
    table.emplace(33, 30);  // collides with 1 and 17
    table.emplace(2, 40);   // separate bucket
    table.emplace(18, 50);  // collides with 2

    const auto& t = table.debug_table();
    const size_t m = table.debug_mask();

    
    for (auto [k, v] : { std::pair{1,10}, {17,20}, {33,30}, {2,40}, {18,50} }) {
        auto it = table.find(k);
        REQUIRE(it != table.end());
        REQUIRE(it->second == v);
    }

  
    // For every home bucket, each bit set must point to an occupied bucket
    for (size_t i = 0; i < t.size(); ++i) {
        uint32_t hop = t[i].hop_info;
        while (hop) {
            unsigned int bit = __builtin_ctz(hop);
            size_t pos = (i + bit) & m;
            REQUIRE(t[pos].occupied);
            // Each occupied bucket must have home = i for at least one hop bit
            hop &= ~(1u << bit);
        }
    }


    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].occupied) {
            size_t home = table.debug_bucket_index(t[i].key);
            size_t distance = (i + t.size() - home) & m;
            REQUIRE(distance < 32); // within NEIGHBORHOOD window
        }
    }

}

TEST_CASE("HopscotchHash: hop bits reflect exact displacement", "[hopscotch][bitmap-exact]") {
    HopscotchHash<int,int> table(8);

    // Force known pattern (these keys share the same home modulo 8)
    table.emplace(1, 10);
    table.emplace(9, 20);
    table.emplace(17, 30);

    const auto& t = table.debug_table();
    size_t m = table.debug_mask();
    size_t home = table.debug_bucket_index(1);

    uint32_t hop = t[home].hop_info;
    REQUIRE(hop != 0u);

    // Verify each set hop bit corresponds exactly to an occupied displaced bucket
    while (hop) {
        unsigned int bit = __builtin_ctz(hop);
        size_t pos = (home + bit) & m;
        REQUIRE(t[pos].occupied);
        REQUIRE(t[pos].key % 8 == home); // all should hash to same home
        hop &= ~(1u << bit);
    }
}

TEST_CASE("HopscotchHash: after rehash, all elements are preserved and valid", "[hopscotch][rehash-structure]") {
    HopscotchHash<int,int> table(8);

    for (int i = 0; i < 100; ++i)
        table.emplace(i, i * 10);

    const auto& t = table.debug_table();

    // Table should have grown
    REQUIRE(t.size() >= 100);

    // All keys must still be findable
    for (int i = 0; i < 100; ++i) {
        auto it = table.find(i);
        REQUIRE(it != table.end());
        REQUIRE(it->second == i * 10);
    }

    // Hop-info sanity: all set bits point to occupied slots
    size_t m = table.debug_mask();
    for (size_t i = 0; i < t.size(); ++i) {
        uint32_t hop = t[i].hop_info;
        while (hop) {
            unsigned int bit = __builtin_ctz(hop);
            size_t pos = (i + bit) & m;
            REQUIRE(t[pos].occupied);
            hop &= ~(1u << bit);
        }
    }
}







TEST_CASE("HopscotchHash: table layout invariants", "[hopscotch][layout]") {
    HopscotchHash<int,int> table(32);

    // Insert keys deliberately spaced to create collisions
    for (int i = 0; i < 64; ++i)
        table.emplace(i * 7, std::move(i));// hash collisions modulo 32 guaranteed

    const auto& t = table.debug_table();
    const size_t m = table.debug_mask();
    const size_t N = t.size();
    const size_t NEIGHBORHOOD = 32;

    // Map from occupied index -> home bucket that claims it
    std::vector<size_t> claimed_by(N, SIZE_MAX);

   
    for (size_t i = 0; i < N; ++i) {
        if (!t[i].occupied) continue;

        size_t home = table.debug_bucket_index(t[i].key);
        size_t dist = (i + N - home) & m;
        REQUIRE(dist < NEIGHBORHOOD);
    }

  
    for (size_t home = 0; home < N; ++home) {
        uint32_t hop = t[home].hop_info;
        while (hop) {
            unsigned int bit = __builtin_ctz(hop);
            size_t pos = (home + bit) & m;

            // Bucket must be occupied
            REQUIRE(t[pos].occupied);

            // The key in that bucket must hash to this home
            REQUIRE(table.debug_bucket_index(t[pos].key) == home);

            // This bucket should not be claimed by another home
            REQUIRE((claimed_by[pos] == SIZE_MAX || claimed_by[pos] == home));

            claimed_by[pos] = home;
            hop &= ~(1u << bit);
        }
    }

    //Every occupied bucket must be claimed by exactly one home bucket
    for (size_t i = 0; i < N; ++i) {
        if (t[i].occupied) {
            REQUIRE(claimed_by[i] != SIZE_MAX);
        }
    }

    // No false claims (unoccupied buckets must not appear in any hop bitmap)
    for (size_t home = 0; home < N; ++home) {
        uint32_t hop = t[home].hop_info;
        while (hop) {
            unsigned int bit = __builtin_ctz(hop);
            size_t pos = (home + bit) & m;
            REQUIRE(t[pos].occupied); // redundant but explicit
            hop &= ~(1u << bit);
        }
    }
}