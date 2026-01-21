#pragma once

#include <cstdint>
#include <array>
#include "ex2_task3_hashes.h"

inline std::array<uint16_t, 2048> bloom_tags = {};

// Computes bloom filter bits from a given hash value.
// We could also rehash the hash value to choose the bits, but 
// we lose time for no reason. The hash is random enough
// and well distributed.
// Therefore, we simply take 4 different parts, and simply work 
// with those to set our bits.

inline uint16_t compute_bloom_bits_separately(uint64_t hash){
    
    uint16_t bit1 = (hash >> 0) & 0xF;   // bits 0-3
    uint16_t bit2 = (hash >> 16) & 0xF;  // bits 16-19
    uint16_t bit3 = (hash >> 32) & 0xF;  // bits 32-35
    uint16_t bit4 = (hash >> 48) & 0xF;  // bits 48-51

    return (1 << bit1) | (1 << bit2) | (1 << bit3) | (1 << bit4);
}

// We compute once and save time from recomputing bloom tags every time.
// It doesn't matter if it is not EXACTLY 4 bits per tag.
inline void precompute_bloom_tags(){
    for (int i = 0; i < 2048; i++) {
        uint16_t pattern = 0;
        
        // We set, approximately, 4 bits per tag.
        pattern |= (1u << ((i * 7) & 0xF));
        pattern |= (1u << ((i * 13) & 0xF));
        pattern |= (1u << ((i * 19) & 0xF));
        pattern |= (1u << ((i * 23) & 0xF));
        
        bloom_tags[i] = pattern;
    }
}

// We get 11 bits from the hash, and use it to index 
// into the precomputed tags.
inline uint16_t compute_bloom_bits(uint64_t hash) {
    uint16_t index = static_cast<uint16_t>((hash >> (32 - 11)) & 0x7FF);
    return bloom_tags[index];
}

// I needed and use this version because I was trying to make things faster.
inline uint16_t compute_bloom_bits(uint32_t hash_high) {
    // We simply take the bottom 11 bits. 
    // This corresponds to bits 32-42 of the original full hash.
    return bloom_tags[hash_high & 0x7FF];
}

// Again just implementing the paper's algorithm from FIgure 6.
// We simply get the 11 bits used to choose a bloom tag.
inline bool could_contain(uint16_t entry_bloom, uint64_t hash) {

    // uint16_t index = static_cast<uint16_t>((hash >> (32 - 11)) & 0x7FF);
    uint16_t index = static_cast<uint16_t>((hash >> 32) & 0x7FF);

    uint16_t tag = bloom_tags[index];

    // Let's suppose:
    // entry_bloom = 0101 1110 1010 0001
    // tag1 = 0010 0000 0011 0100
    // tag2 = 0100 0100 1010 0000
    //
    // Then:
    // tag1 & ~entry_bloom = 0010 0000 0001 0100 
    // this means that tag1 didn't have the, usually, 4 bits required.
    // tag2 & ~entry_bloom = 0000 0000 0000 0000
    // tag2 is a match!
    return !(tag & ~entry_bloom);
}

// Again just implementing the paper's algorithm from FIgure 6.
// We simply get the 11 bits used to choose a bloom tag.
inline bool could_contain_64(uint16_t entry_bloom, uint64_t hash) {
    uint16_t index = static_cast<uint16_t>((hash >> (32 - 11)) & 0x7FF);
    uint16_t tag = bloom_tags[index];
    return !(tag & ~entry_bloom);
}

