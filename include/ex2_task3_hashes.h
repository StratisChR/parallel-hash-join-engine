#pragma once

#include <cstdint>
#include <nmmintrin.h>

// I got the functions exactly as they were described in the paper.
// to keep the look exactly the same, I created the really
// actually useless wrapper functions.

inline uint32_t crc32(uint32_t seed, uint32_t value) {
    return _mm_crc32_u32(seed, value);
}

inline uint32_t crc32_64(uint32_t seed, uint64_t value) {
    return _mm_crc32_u64(seed, value);
}

inline uint64_t hash32(uint32_t key, uint32_t seed) {
    uint64_t k = 0x8648DBDBULL;
    uint32_t crc = crc32(seed, key);
    return crc* ((k << 32) + 1);
}

inline uint64_t hash64(uint64_t key, uint32_t seed1, uint32_t seed2) {
    uint64_t k = 0x2545F4914F6CDD1DULL;
    uint32_t crc1 = crc32(seed1, key);
    uint32_t crc2 = crc32(seed2, key);
    uint64_t upper = static_cast<uint64_t>(crc2) << 32;
    uint64_t combined = upper | crc1;
    return combined * k;
}

inline uint64_t fibonacci_hash(uint64_t key) {
    const uint64_t k = 11400714819323198485ULL;
    return key * k;
}