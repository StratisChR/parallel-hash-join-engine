#ifdef __INTELLISENSE__
#include "hashtable/unchained_map.h"
#endif

template <class Key, class Value>
void UnchainedHashMap<Key, Value>::collect(size_t start_index, 
                                           size_t end_index, 
                                           ThreadLocalState& tls) 
{
    GlobalAllocator& level1 = *tls.allocator_level1;
    BumpAlloc& level2 = tls.allocator_level2;
    std::vector<BumpAlloc>& level3 = tls.allocators_level3;

    const auto& column = (*tls.shared_table)[tls.key_col];

    // Helper lambda to insert a key. Reduces code duplication between the two paths.
    auto insert_key = [&](int32_t key, size_t row_idx) {
        uint64_t hash = hash32(static_cast<uint32_t>(key), 0);
        // uint64_t hash = fibonacci_hash(static_cast<uint64_t>(key));
        uint64_t partition_id = hash >> tls.partition_shift;

        if (!level3[partition_id].hasSpace(sizeof(BuildTuple))) {
            if (!level2.hasSpace(SMALL_CHUNK)) {
                void* huge_mem = level1.allocate_large_chunk();
                level2.addSpace(huge_mem, HUGE_CHUNK);
            }
            void* small_mem = level2.allocate(SMALL_CHUNK);
            level3[partition_id].addSpace(small_mem, SMALL_CHUNK);
        }

        BuildTuple* t = (BuildTuple*)level3[partition_id].allocate(sizeof(BuildTuple));
        t->key  = key;
        t->row  = static_cast<uint32_t>(row_idx);
        t->hash = hash;
        
        tls.counts[partition_id]++;
    };

    // --- PATH 1: VIEW MODE (Optimized, packed int32, no nulls) ---
    if (column.is_view) {
        // Binary search to find the page containing 'start_index'
        auto it = std::upper_bound(column.view_page_offsets.begin(), 
                                   column.view_page_offsets.end(), 
                                   start_index);
        
        size_t page_idx = (it == column.view_page_offsets.begin()) 
                          ? 0 
                          : std::distance(column.view_page_offsets.begin(), it) - 1;

        while (page_idx < column.view_pages.size()) {
            size_t page_start_global = column.view_page_offsets[page_idx];
            
            // Optimization: If this page starts after our end_index, we are done
            if (page_start_global >= end_index) break;

            std::byte* raw_page = column.view_pages[page_idx];
            // Header: [Row Count (u16)] [Non-Null Count (u16)] -> [Data...]
            uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw_page);
            size_t page_end_global = page_start_global + num_rows;

            // Calculate intersection of [page_start, page_end) and [start_index, end_index)
            size_t work_start = std::max(start_index, page_start_global);
            size_t work_end = std::min(end_index, page_end_global);

            if (work_start < work_end) {
                // Skip 4-byte header to get to data
                const int32_t* data_ptr = reinterpret_cast<const int32_t*>(raw_page + 4);
                size_t start_offset = work_start - page_start_global;
                size_t count = work_end - work_start;

                for (size_t i = 0; i < count; ++i) {
                    insert_key(data_ptr[start_offset + i], work_start + i);
                }
            }
            page_idx++;
        }
    } 
    // --- PATH 2: MATERIALIZED MODE (value_t structs, potential nulls) ---
    else {
        constexpr size_t PAGE_CAPACITY = 4096;
        size_t current_row = start_index;

        while (current_row < end_index) {
            size_t page_idx = current_row / PAGE_CAPACITY;
            size_t offset = current_row % PAGE_CAPACITY;

            if (page_idx >= column.pages.size()) break;

            // Access via unique_ptr
            const auto* page = column.pages[page_idx].get();
            if (!page) { // Should not happen, but safe
                current_row += (PAGE_CAPACITY - offset);
                continue;
            }

            size_t count = std::min(end_index - current_row, PAGE_CAPACITY - offset);

            for (size_t i = 0; i < count; ++i) {
                value_t val = page->data[offset + i];
                // Only insert if not null (INT32 assumption)
                if (!val.is_null()) {
                    insert_key(val.get_int(), current_row + i);
                }
            }
            current_row += count;
        }
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


        // This partition's slice of the directory. It was allocated without zeroing,
        // so clear it here (in parallel across partitions) before counting into it.
        size_t slots_per_partition = capacity / partition_queues.size();
        size_t start_slot = part * slots_per_partition;
        size_t end_slot   = (part + 1) * slots_per_partition;
        std::fill(directory + start_slot, directory + end_slot, uint64_t{0});

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
                
                size_t num_tuples = bytes_used / sizeof(BuildTuple);
                BuildTuple* tuples = reinterpret_cast<BuildTuple*>(chunk->data);

                for (size_t i = 0; i < num_tuples; ++i) {

                    uint64_t slot = get_slot(tuples[i].hash);
                    directory[slot]++; 
                }
                chunk = chunk->next;
            }
        }
        // Convert offsets inside the array, to slots in the directory.
        size_t current_offset = partition_offsets[part];

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

                size_t num_tuples = bytes_used / sizeof(BuildTuple);
                BuildTuple* tuples = reinterpret_cast<BuildTuple*>(chunk->data);

                for (size_t i = 0; i < num_tuples; ++i) {
                    uint64_t slot = get_slot(tuples[i].hash);
                    
                    // Write position.
                    uint64_t write_idx = directory[slot] >> 16;
                    
                    // Write to final array.
                    data[write_idx] = {tuples[i].key, tuples[i].row};
                    
                    // Increment pointer.
                    directory[slot] += (1ULL << 16);
                    
                    // Update Bloom Filter.
                    directory[slot] |= compute_bloom_bits(tuples[i].hash);
                }
                chunk = chunk->next;
            }
        }
        
    }
}

