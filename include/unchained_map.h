#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <bit>
#include <limits>
#include <thread>
#include <atomic>

#include "ex2_task3_hashes.h"
#include "ex2_task3_bloom.h"
#include "indexing.h"
#include "unchnd_structs.h" 
#include "threads.h"

/*  Hash Map Implementation:
    In the second Assignment we had created the unchained hashmap in the wrong way.
    We feel that with this implementation, everything is working as accepted. 

    Build:
    When we want to build the hash map, we first collect all tuples and separate them
    by partition. We count the offsets of each partition in the final array, and build 
    the directory.
    
    Lookup: 
    The lookup works exactly as described in the paper. */

template <class Key, class Value>
class UnchainedHashMap {
private:
    static constexpr uint64_t BLOOM_MASK   = 0x000000000000FFFF;

    // L1 Data Cache is 32KB for my laptop.
    static constexpr size_t L1_CACHE_SIZE = 32 * 1024;

    static constexpr size_t SMALL_CHUNK = 4096;
    static constexpr size_t HUGE_CHUNK = 2 * 1024 * 1024;
    static_assert(HUGE_CHUNK == ChunkPool::CHUNK_SIZE, "slab chunk size mismatch");

    std::unique_ptr<uint64_t[]> directory_mem;   // capacity + 1 entries, not zero-initialized
    uint64_t* directory;
    std::unique_ptr<StoredTuple[]> data;          // filled completely by count_and_copy

    size_t num_elements;      
    size_t capacity;          
    int shift;                

    std::atomic<size_t> next_partition_idx;
    // GlobalAllocator shared_allocator;

public:
    UnchainedHashMap(int initial_shift = 53);
    ~UnchainedHashMap() = default;

    void build(std::vector<column_t>& build_side_table, size_t key_column);

    template <typename Func>
    void lookup(Key key, Func process_match);

    // Probes a batch of up to PROBE_BATCH keys. Calls on_match(i, build_row) for every
    // build row whose key equals keys[i]. Same results as calling lookup() per key,
    // but organized in three tight loops so the CPU can overlap many memory accesses.
    static constexpr size_t PROBE_BATCH = 256;
    template <typename Func>
    void lookup_batch(const int32_t* keys, size_t n, Func on_match) const;
    

private: 
    // --- Internal Helpers ---
    void calc_capacity();
    void build_data_array();
    void update_bloom_filter(uint64_t slot, uint16_t bloom_tag);

    // --- Parallelization Helpers ---
    size_t calc_partition_num(size_t total_rows);
    // Phase 1 of build:
    void collect(size_t start_index, size_t end_index, ThreadLocalState& tls);
    // Phase 2 of build:
    void count_and_copy(// size_t starting_partition, size_t end_partition, 
                            const std::vector<size_t>& partition_offsets,
                            const std::vector<std::vector<BumpAlloc>>& partition_queues
                            );
    

    inline uint64_t get_slot(uint64_t hash) const { return hash >> shift; }
    
};

#include "unchnd_helpers.tpp"
#include "unchnd_helpers_prll.tpp"
#include "unchnd_public.tpp"