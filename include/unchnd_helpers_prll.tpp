#ifdef __INTELLISENSE__
#include "unchained_map.h"
#endif

template <class Key, class Value>
void UnchainedHashMap<Key, Value>::collect( size_t start_index, 
                                            size_t end_index, 
                                            ThreadLocalState& tls) 
{
    GlobalAllocator& level1 = *tls.allocator_level1;

    BumpAlloc& level2 = tls.allocator_level2;
    std::vector<BumpAlloc>& level3 = tls.allocators_level3;

    // The indexes are rows in the build side table. 
    for(size_t i = start_index; i < end_index; i++){
        int32_t key = (*tls.shared_table)[tls.key_col][i].get_int(); 
        uint64_t hash = hash32(static_cast<uint32_t>(key), 0);

        // The hashed_keys vector correlates row_indices with the hashes
        // of the keys in those rows. That way, we have no race conditions.
        // (*tls.hashed_keys)[i] = hash;

        // The tuples are partitioned based on their hash. This is crucial to 
        // the next steps, the ones described in Figure 10. 
        uint64_t partition_id = hash >> tls.partition_shift;

        // Exactly what Figure 9 describes.
        // If level 1 has no space available.
        if (!level3[partition_id].hasSpace(sizeof(Tuple))) {
                
            // We check level 2.
            if (!level2.hasSpace(SMALL_CHUNK)) {
                
                // We allocate memory in level 1.
                void* huge_mem = level1.allocate_large_chunk();
                level2.addSpace(huge_mem, HUGE_CHUNK);
            }

            // We give a small chunk from level 2 to level 3.
            void* small_mem = level2.allocate(SMALL_CHUNK);
            level3[partition_id].addSpace(small_mem, SMALL_CHUNK);
        }

        // We allocate the size of a Tuple from the available space in level3.
        // We then cast it to a Tuple pointer and fill in the values.
        Tuple* t = (Tuple*)level3[partition_id].allocate(sizeof(Tuple));
        t->key = key;
        t->hash_high = static_cast<uint32_t>(hash >> 32);
        t->index = i; 

        // We increment the count for this partition.
        tls.counts[partition_id]++;
    }
}

template <class Key, class Value>
void UnchainedHashMap<Key, Value>::count_and_copy(  // size_t starting_partition,
                                            // size_t end_partition,
                                            const std::vector<size_t>& partition_offsets,
                                            const std::vector<std::vector<BumpAlloc>>& partition_queues
                                            //const std::vector<uint64_t>& hashed_keys
                                        ) 
{
    const size_t CHUNK_DATA_CAPACITY = 4096 - sizeof(BumpAlloc::Chunk);

    size_t num_parts = partition_offsets.size(); 
    size_t part;

    // Figure ten provides pseudocode for 1 partition. Here we loop.
    while ((part = next_partition_idx.fetch_add(1, std::memory_order_relaxed)) < num_parts)
    {
        // First loop of Figure 10.
        
        // const BumpAlloc& allocator = merged_level3[part];
        // BumpAlloc::Chunk* chunk = allocator.head;

        const auto& allocators = partition_queues[part];

        // We count how many tuples go into each slot. I might put the bloom bit calculation
        // here to follow the paper exactly. NOt right now.
        for( const auto& allocator : allocators){
            BumpAlloc::Chunk* chunk = allocator.head;
            while (chunk) {

                if (chunk->next) {
                    __builtin_prefetch(chunk->next, 0, 1);
                }

                size_t bytes_used = (chunk == allocator.head) 
                                    ? (size_t)(allocator.ptr - chunk->data) 
                                    : CHUNK_DATA_CAPACITY;
                
                size_t num_tuples = bytes_used / sizeof(Tuple);
                Tuple* tuples = reinterpret_cast<Tuple*>(chunk->data);

                for (size_t i = 0; i < num_tuples; ++i) {

                    // uint64_t hash = hashed_keys[tuples[i].index];
                    // uint64_t slot = get_slot(hash);
                    // I want to avoid the above commented out code.
                    uint64_t hash_approx = (static_cast<uint64_t>(tuples[i].hash_high) << 32);
                    uint64_t slot = get_slot(hash_approx);
                    directory[slot]++; 
                }
                chunk = chunk->next;
            }
        }
        // Convert offsets inside the array, to slots in the directory.
        size_t current_offset = partition_offsets[part];
        size_t slots_per_partition = capacity / partition_queues.size();
        size_t start_slot = part * slots_per_partition;
        size_t end_slot = (part + 1) * slots_per_partition;

        // Each slot stores the START of the segment in the array. We move the pointer to
        // the end during the copying.
        for (size_t s = start_slot; s < end_slot; ++s) {
            uint64_t count = directory[s];
            directory[s] = (current_offset << 16);
            current_offset += count;
        }

        // Third loop of Figure 10.
        for( const auto& allocator : allocators){
            BumpAlloc::Chunk* chunk = allocator.head;
            while (chunk) {

                if (chunk->next) {
                    __builtin_prefetch(chunk->next, 0, 1);
                }

                size_t bytes_used = (chunk == allocator.head) 
                                    ? (size_t)(allocator.ptr - chunk->data) 
                                    : CHUNK_DATA_CAPACITY; 

                size_t num_tuples = bytes_used / sizeof(Tuple);
                Tuple* tuples = reinterpret_cast<Tuple*>(chunk->data);

                for (size_t i = 0; i < num_tuples; ++i) {
                    // uint64_t hash = hashed_keys[tuples[i].index];
                    uint64_t hash_approx = (static_cast<uint64_t>(tuples[i].hash_high) << 32);
                    uint64_t slot = get_slot(hash_approx);
                    
                    // Write position.
                    uint64_t write_idx = directory[slot] >> 16;
                    
                    // Write to final array.
                    data[write_idx] = {tuples[i].key, tuples[i].hash_high, tuples[i].index};
                    
                    // Increment pointer.
                    directory[slot] += (1ULL << 16);
                    
                    // Update Bloom Filter.
                    directory[slot] |= compute_bloom_bits(tuples[i].hash_high); 
                }
                chunk = chunk->next;
            }
        }
        
    }
}

