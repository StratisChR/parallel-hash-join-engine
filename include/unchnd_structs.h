#pragma once
#include <cstdint>
#include <vector>
#include "unchnd_slab_alloc.h" 
#include "indexing.h"

// Info for segments: I used to have different helpers to get info for each segment
// of the directory. Using this inside "get_segment_info" is cleaner.
struct SegmentCache {
    uint64_t start;
    uint64_t end;
    uint16_t bloom;
};

// The struct our 3 slab allocator uses to save tuples.
// I added 'hash_high' because we already have a padding, so it doesn't 
// take more space, but possibly helps because we can get rid of 'hashed_keys'.
struct Tuple{
    int32_t key;
    uint32_t hash_high;
    uint64_t index;
};

// Thread local state for parallel building. Contains all the useful 
// info our threads need to function.
struct ThreadLocalState {
    std::unique_ptr<GlobalAllocator> allocator_level1; 

    BumpAlloc allocator_level2;
    std::vector<BumpAlloc> allocators_level3; 
    // We keep track of counts per partition. Each thread counts its own tuples.
    // After all threads finish their work, we add all counts for each partition.
    // Then, we can use these total counts to create our directory and data array.
    std::vector<size_t> counts;

    // Reference to the build side table for all threads.
    const std::vector<column_t>* shared_table;
    // // To avoid recomputing hashes. However, this might be creating 
    // // a critical section. It's possible I pass copies of the vector and 
    // // combine it later.
    // std::vector<uint64_t>* hashed_keys;

    // To get the key from the table.
    size_t key_col;
    // For slot calculation inside level3.
    int partition_shift;

    ThreadLocalState(   size_t num_partitions,
                        int part_shift,
                        const std::vector<column_t>& table_ref, 
                        // std::vector<uint64_t>& hashes_ref, 
                        size_t key_column
                        )
        : allocators_level3(num_partitions),
          counts(num_partitions, 0),
          shared_table(&table_ref),
          // hashed_keys(&hashes_ref),
          key_col(key_column),
          partition_shift(part_shift)
        {
            allocator_level1 = std::make_unique<GlobalAllocator>();
        }
};