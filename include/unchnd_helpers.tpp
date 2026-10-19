#ifdef __INTELLISENSE__
#include "unchained_map.h"
#endif

template <class Key, class Value>
void UnchainedHashMap<Key, Value>::calc_capacity() {
    // The formula used for size by the paper is " size 𝑚 = 2^⌈log2 (1.125𝑛)⌉ ".
    // This has an average load factor of 65%, because of the rounding up to the
    // next pwoer of two.

    // Base case.
    if (num_elements == 0) {
        shift = 53;
        capacity = 1ULL << (64 - shift);
        return;
    }

    uint64_t target_size = (num_elements * 9 + 7) / 8;
    // The following command is simply bit_ceil, so we simply round up to the next power of 2.
    uint64_t capacity_val = (target_size <= 1) ? 1 : 
        (1ULL << (64 - __builtin_clzll(target_size - 1)));

    // We count trailing zeros to understand which power of two we have.
    int k = __builtin_ctzll(capacity_val);
    // We calculate the shift.
    int computed_shift = 64 - k;

    shift = (computed_shift < 53) ? computed_shift : 53;
    capacity = 1ULL << (64 - shift);
}

template <class Key, class Value>
size_t UnchainedHashMap<Key, Value>::calc_partition_num(size_t total_rows) {
    
    constexpr size_t TUPLE_SIZE = sizeof(BuildTuple);
    // Total size of build side tuples.
    size_t total_size_bytes = total_rows * TUPLE_SIZE;
    // According to the paper, we want (TotalSize / Partitions) <= L1_Size.
    size_t min_partitions = (total_size_bytes + L1_CACHE_SIZE - 1) / L1_CACHE_SIZE;
    
    // Next power of 2.
    size_t temp = std::max({min_partitions, num_worker_threads(), size_t(2)});
    size_t final_partitions = (temp <= 1) ? 1 : 
        (1ULL << (64 - __builtin_clzll(temp - 1)));

    // According to the paper, the most important factor for speed is the L1 misses.
    // To prioritize this, we cap the number of partitions to 16384. This, would be
    // bad for TLB misses if we are using 4KB pages, but not if we use Huge Pages. 
    // So, I'll have to configure my system accrodingly.
    if (final_partitions > 16384) final_partitions = 16384;

    return final_partitions;
}