#ifdef __INTELLISENSE__
#include "unchained_map.h"
#include "threads.h"
#endif



template <class Key, class Value>
UnchainedHashMap<Key, Value>::UnchainedHashMap(int initial_shift) 
    : shift(initial_shift), num_elements(0) {
    
    capacity = 1ULL << (64 - shift);
    directory_mem.resize(capacity);  
    precompute_bloom_tags();
}

template <class Key, class Value>
void UnchainedHashMap<Key, Value>::build(std::vector<column_t>& build_side_table, size_t key_column) {

    size_t total_rows = build_side_table[key_column].size();
    num_elements = total_rows; 
    if (total_rows > UINT32_MAX) throw std::runtime_error("build side too large for 32-bit row ids");

    calc_capacity();
    directory_mem.assign(capacity + 1, 0);

    directory = directory_mem.data() + 1;

    size_t num_threads = pool().size();
    size_t num_partitions = calc_partition_num(total_rows);

    
    if (num_partitions > capacity) {
        num_partitions = capacity;
    }
    
    int partition_bits = __builtin_ctzll(num_partitions); 
    int partition_shift = 64 - partition_bits;

    // We will store the hashes to avoid re-evaluation in Phase 2.
    // This correlates row_indices to hashes (of the key of each row).
    // std::vector<uint64_t> hashed_keys(total_rows);
    
    std::vector<ThreadLocalState> thread_states;
    thread_states.reserve(num_threads);

    // Phase 1: Tuple collection. Thread t collects rows [starts[t], starts[t+1]).
    std::vector<size_t> starts(num_threads + 1);
    for (size_t t = 0; t < num_threads; t++)
        starts[t + 1] = starts[t] + total_rows / num_threads + (t < total_rows % num_threads ? 1 : 0);

    for (size_t t = 0; t < num_threads; t++)
        thread_states.emplace_back(num_partitions, partition_shift, build_side_table, key_column);

    pool().run([&](size_t t) { collect(starts[t], starts[t + 1], thread_states[t]); });

    // Now, between the two phases (Figure 9 and Figure 10), we must count
    // how many tuples are in each partition. We do that by summing all "counts[i]" 
    // in the thread local states.
    std::vector<size_t> partition_offsets(num_partitions, 0);
    size_t current_global_offset = 0;

    for (size_t p = 0; p < num_partitions; ++p) {
        partition_offsets[p] = current_global_offset;
        
        // Sum up the counts for this partition from ALL threads.
        size_t partition_total_size = 0;
        for (const auto& tls : thread_states) {
            partition_total_size += tls.counts[p];
        }
        current_global_offset += partition_total_size;
    }

    data.resize(current_global_offset);

    std::vector<std::vector<BumpAlloc>> partition_queues(num_partitions);

    for(auto& q : partition_queues) q.reserve(num_threads);

    for (auto& tls : thread_states) {
        for (size_t p = 0; p < num_partitions; ++p) {
            // We use std::move to transfer ownership cheaply
            // tls.allocators_level3[p] becomes empty/null after this.
            if (tls.allocators_level3[p].head != nullptr) {
                partition_queues[p].push_back(std::move(tls.allocators_level3[p]));
            }
        }
    }

    next_partition_idx.store(0);

    // Phase 2 (Figure 10): workers take partitions from the shared atomic counter.
    pool().run([&](size_t) { count_and_copy(partition_offsets, partition_queues); });
}

template <class Key, class Value>
template <typename Func>
void UnchainedHashMap<Key, Value>::lookup(Key key, Func process_match) {
    uint64_t hashed_key = hash32(static_cast<uint32_t>(key), 0);
    // uint64_t hashed_key = fibonacci_hash(static_cast<uint64_t>(key));
    uint64_t slot = get_slot(hashed_key);
    
    auto seg_info = get_segment_info(slot);

    if (!could_contain_64(seg_info.bloom, hashed_key)) return;

    for (uint64_t i = seg_info.start; i < seg_info.end; i++) {
        if (data[i].key == key) {
            process_match(data[i].row);
        }
    }
}

// Not currently used:
template <class Key, class Value>
void UnchainedHashMap<Key, Value>::prefetch_directory(Key key) const {
    uint64_t hashed_key = hash32(static_cast<uint32_t>(key), 0);
    // uint64_t hashed_key = fibonacci_hash(static_cast<uint64_t>(key));
    uint64_t slot = get_slot(hashed_key);
    
    __builtin_prefetch(&directory[slot], 0, 3);
}