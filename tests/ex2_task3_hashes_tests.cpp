#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "ex2_task3_hashes.h"  

#include <vector>
#include <random>
#include <cmath>

TEST_CASE("Hash32 is deterministic", "[hash]") {
    uint32_t key = 12345;
    uint64_t h1 = hash32(key, 0);
    uint64_t h2 = hash32(key, 0);
    REQUIRE(h1 == h2);
}

TEST_CASE("Hash32 produces different outputs for different inputs", "[hash]") {
    uint32_t key1 = 12345;
    uint32_t key2 = 12346;
    uint64_t h1 = hash32(key1, 0);
    uint64_t h2 = hash32(key2, 0);
    REQUIRE(h1 != h2);
}

TEST_CASE("Hash32 produces different outputs for different seeds", "[hash]") {
    uint32_t key = 12345;
    uint64_t h1 = hash32(key, 0);
    uint64_t h2 = hash32(key, 1);
    REQUIRE(h1 != h2);
}

TEST_CASE("Hash64 is deterministic", "[hash]") {
    uint64_t key = 0xABCDEF1234567890ULL;
    uint64_t h1 = hash64(key, 0, 1);
    uint64_t h2 = hash64(key, 0, 1);
    REQUIRE(h1 == h2);
}

TEST_CASE("Hash32 has good distribution", "[hash]") {
    const int NUM_BUCKETS = 1024;
    int buckets[NUM_BUCKETS] = {0};
    
    for (uint32_t i = 0; i < 100000; i++) {
        uint64_t h = hash32(i, 0);
        int bucket = h % NUM_BUCKETS;
        buckets[bucket]++;
    }
    
    int min = buckets[0], max = buckets[0];
    for (int i = 1; i < NUM_BUCKETS; i++) {
        if (buckets[i] < min) min = buckets[i];
        if (buckets[i] > max) max = buckets[i];
    }
    
    double expected = 100000.0 / NUM_BUCKETS;
    INFO("Expected per bucket: " << expected);
    INFO("Min: " << min << ", Max: " << max);
    INFO("Range: " << (max - min));
    
    // Range should be less than 20% of expected
    REQUIRE(max - min < expected * 0.2);
}

// THe following tests are used to benchmark the hash functions.
// Basically we just call hashes repeatedly and measure the time taken.
// The results are:
// hash32: ~1.63 ns
// hash64: ~1.67 ns
// So, ~600 million hashes per second per core.

TEST_CASE("Hash32 benchmark", "[hash][!benchmark]") {
    uint32_t key = 12345;
    BENCHMARK("hash32") {
        return hash32(key, 0);
    };
}
 
TEST_CASE("Hash64 benchmark", "[hash][!benchmark]") {
    uint64_t key = 0xABCDEF1234567890ULL;
    BENCHMARK("hash64") {
        return hash64(key, 0, 1);
    };
}

TEST_CASE("Hash32 with varying inputs", "[hash][!benchmark]") {
    std::vector<uint32_t> keys(10000);
    for (size_t i = 0; i < keys.size(); i++) {
        keys[i] = static_cast<uint32_t>(i * 997);  // Prime number for distribution
    }
    
    size_t idx = 0;
    BENCHMARK("hash32 varying inputs") {
        return hash32(keys[idx++ % keys.size()], 0);
    };
}

TEST_CASE("Hash64 with varying inputs", "[hash][!benchmark]") {
    std::vector<uint64_t> keys(10000);
    for (size_t i = 0; i < keys.size(); i++) {
        keys[i] = static_cast<uint64_t>(i * 997);
    }
    
    size_t idx = 0;
    BENCHMARK("hash64 varying inputs") {
        return hash64(keys[idx++ % keys.size()], 0, 1);
    };
}

TEST_CASE("Hash32 with random data", "[hash][!benchmark]") {
    std::vector<uint32_t> keys(10000);
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;
    
    for (auto& k : keys) {
        k = dis(gen);
    }
    
    size_t idx = 0;
    BENCHMARK("hash32 random inputs") {
        return hash32(keys[idx++ % keys.size()], 0);
    };
}

TEST_CASE("Hash64 with random data", "[hash][!benchmark]") {
    std::vector<uint64_t> keys(10000);
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    
    for (auto& k : keys) {
        k = dis(gen);
    }
    
    size_t idx = 0;
    BENCHMARK("hash64 random inputs") {
        return hash64(keys[idx++ % keys.size()], 0, 1);
    };
}