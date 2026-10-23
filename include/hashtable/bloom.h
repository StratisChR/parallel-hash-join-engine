#pragma once

#include <cstdint>
#include <array>
#include <random>
#include "hashtable/hash.h"

inline std::array<uint16_t, 2048> bloom_tags = {};

// We compute once and save time from recomputing bloom tags every time.
// It doesn't matter if it is not EXACTLY 4 bits per tag.
inline void precompute_bloom_tags() {
    size_t n = 0;
    for (uint32_t m = 0; m < 65536; ++m)
        if (__builtin_popcount(m) == 4) bloom_tags[n++] = m;   // n == 1820
    std::mt19937 rng(42);
    for (size_t i = n; i < 2048; ++i) bloom_tags[i] = bloom_tags[rng() % 1820];
}

// We get 11 bits from the hash, and use it to index 
// into the precomputed tags.
inline uint16_t compute_bloom_bits(uint64_t hash) {
    uint16_t index = static_cast<uint16_t>((hash >> (32 - 11)) & 0x7FF);
    return bloom_tags[index];
}

// Again just implementing the paper's algorithm from FIgure 6.
// We simply get the 11 bits used to choose a bloom tag.
inline bool could_contain_64(uint16_t entry_bloom, uint64_t hash) {
    uint16_t index = static_cast<uint16_t>((hash >> (32 - 11)) & 0x7FF);
    uint16_t tag = bloom_tags[index];
    return !(tag & ~entry_bloom);
}

