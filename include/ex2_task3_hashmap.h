#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>
#include <unordered_set>

#include "ex2_task3_hashes.h"
#include "ex2_task3_bloom.h"
#include "indexing.h"

/*

This is going to be the hash table implementation.
First, I need to explain how it works in general terms, 
because I hate getting lost after starting to write code.

Initialization:
1. We precompute all bloom tags and put them in table[2^11]. 
   This is enough to save all possible 1820 (16 choose 4) tags, and 
   also some as duplicates.
2. We create a directory with 2^(64-shift) entries. At the start
   we set shift = 53, so we have 2^11 = 2048 entries.
   ( It should also be noted that our tuples are going to be 
    grouped by their (64-shift) prefixes. )
3. During the build phase, the directory is used to save how many tuples 
   we will store in its segment. 
   At the start of the build phase, we check how many rows we have and
   resize our directory, to achieve a load factor less than 65%.
   If in any segment we have more than
   4 (or whatever MAX_TUPLES_PER_SEGMENT is set to) tuples, we trigger a resize(basically
   a rehash). After succesfully creating a directory that can hold all tuples, we create
   an array of tuples of size "sum of all tuples in all segments".
4. Then we reiterate over all tuples. Now all tuples will be saved to their respective 
   segment and the directory will get its final form of each slot containing 
   a pointer and a bloom filter for each segment.
  
Build phase workflow:
1. For each tuple, we hash the key. Let's call this value hashed_key.
2. We take the (64-shift) prefix and increment the value in the directory at that 
   index by one.
3. after succesfully dividing all tuples between segments, we again take those 
   (64-shift) prefix bits and for each tuple use the pointer to find an empty slot 
   in its segment to store it. 
4. then we find the respective bloom filter tag for the hask key and OR it in the 
   directory slots bloom filter.

*/

template <class Key, class Value>
class UnchainedHashMap {
private:

   static constexpr int32_t EMPTY_KEY = std::numeric_limits<int32_t>::min();
   static constexpr uint64_t NOT_FOUND = UINT64_MAX;
   static constexpr uint64_t POINTER_MASK = 0xFFFFFFFFFFFF0000;
   static constexpr uint64_t BLOOM_MASK   = 0x000000000000FFFF;
   // actually the following is not tuples per segment, but unique keys per directory slot.
   // static constexpr uint64_t MAX_TUPLES_PER_SEGMENT = 1;
   static constexpr double MAX_LOAD_FACTOR = 65; // we write it in %
   
   // Each entry in the directory contains a 64 bit unsigned, which is divided in two parts
   // The first 48 bits contain a pointer to the array of tuples. The next 16 bits, which
   // are not used in pointers, contain the bloom filter for each segment.
   // Before it starts doing the operations above, it used to save how many tuples
   // we can fit in each segment.
   std::vector<uint64_t> directory;

   std::vector<std::pair<Key, Value>> data;

   struct SegmentCache {
       uint64_t start;
       uint64_t end;
       uint16_t bloom;
   };
    
   // Metadata for the hash table.
   size_t num_elements;      // How many tuples are currently stored
   size_t capacity;          // Total capacity of data array
   int shift;                // For directory slot calculation


public:

    // Constructor
    UnchainedHashMap(int initial_shift = 53) 
        : shift(initial_shift), num_elements(0) {
        
        // We initialize the directory with 2^(64-shift) entries.
        // At first all are zeroed.
        this->capacity = 1ULL << (64 - shift);
        directory.resize(capacity, 0);  
        
        // Precompute Bloom filter tags
        precompute_bloom_tags();
    }
    
    // Destructor (automatic with vectors, but explicit is fine)
    ~UnchainedHashMap() = default;

private: // Utilities:

    inline uint64_t get_slot(uint64_t hash) const{
        return hash >> shift;
    }

    inline uint64_t get_entry(uint64_t slot) const{
        return directory[slot];
    }

    inline uint64_t get_slot_pointer(uint64_t slot) const {
        return directory[slot] >> 16;
    }

    inline uint16_t get_slot_filter(uint64_t slot) const {
        uint64_t entry = get_entry(slot);
        uint64_t filter = static_cast<uint16_t>(entry & BLOOM_MASK);
        return filter;
    }

    inline SegmentCache get_segment_info(uint64_t slot) const {
        uint64_t entry = directory[slot];
        uint64_t end = entry >> 16;
        uint64_t start = (slot == 0) ? 0 : (directory[slot - 1] >> 16);
        uint16_t bloom = static_cast<uint16_t>(entry & BLOOM_MASK);
        return {start, end, bloom};
    }

    // could use it for testing but nothing more than that.
    double get_load_factor() const {
        return static_cast<double>(num_elements) / static_cast<double>(capacity);
    }

    // I am changing this function to match the paper better.
    void calc_capacity(){
        if (num_elements == 0) {
        shift = 53;
        capacity = 1ULL << (64 - shift);  // 2048 entries
        return;
        }

        // The paper said the hash table is assumed to have size 𝑚 = 2⌈log2 (1.125𝑛) ⌉
        // So to achieve this using integer arithmetic:
        // 1.125n = (9/8)n
        // To simulate the ceiling, we add 7 to 9*n before dividing by 8.
        uint64_t target_size = (num_elements * 9 + 7) / 8;

        // Now we round up to the next power of two.
        uint64_t capacity_val = (target_size <= 1) ? 1 : 
        (1ULL << (64 - __builtin_clzll(target_size - 1)));

        // We compute the shift.
        // capacity = 2^k, so shift = 64 - k
        int k = __builtin_ctzll(capacity_val);
        int computed_shift = 64 - k;

        shift = (computed_shift < 53) ? computed_shift : 53;
        capacity = 1ULL << (64 - shift);
    }

private: // Everything related to rehashing/resizing:

    // Create array, separate segments based on number in each directory slot.
    // It also creates the pointers in the directory.
    void build_data_array(){
        // Set the pointers in the directory.
        uint64_t current_offset = 0;
        for (size_t i = 0; i < capacity; ++i) {
            uint64_t count = directory[i];
            
            directory[i] = (current_offset << 16);
            
            current_offset += count;
        }

        // Resize the contiguous data array to fit exactly all elements
        data.resize(current_offset); 
    }

private: // Support for the external operations:

    // We use it when inserting.
    void update_bloom_filter(uint64_t slot, uint16_t bloom_tag){
        directory[slot] |= bloom_tag;
    }

public: // External Operations:

    // I might make the for loop in create_directory its own function. And also add the column store version.

    // The following function is called at the start of each join call. 
    // It is used to build the directory that will be used for the hash table when joining
    // the build and probe sides. 
    // We give it our build_side table, then iterate through all its tuples, hash their keys
    // and count how many of them have the same (64-shift) prefix. We do that by incrementing by 1
    // the number stored inside directory[prefix], which has 2^(64-shift) slots and all are initialized to 0.

    // updated to keep only unique keys
    void create_directory(std::vector<column_t>& build_side_table, size_t key_column){
        size_t total_rows = build_side_table[key_column].size();

        
        // The calculate capacity function has to change. I had misunderstood the paper. 
        num_elements = total_rows; 
        calc_capacity();
        
        directory.assign(capacity, 0);

        // I might precompute all hashes first to speed up the process.

        // Counting how many tuples go to each slot.
        std::vector<uint64_t> hashed_keys(total_rows);
       
        for (size_t i = 0; i < total_rows; ++i) {
            int32_t key = build_side_table[key_column][i].i32; 
            hashed_keys[i] = hash32(static_cast<uint32_t>(key), 0);
        
            uint64_t slot = get_slot(hashed_keys[i]);
            directory[slot]++;
        }

        build_data_array();

        // Copy the tuples.
        for (size_t i = 0; i < total_rows; ++i) {
            uint64_t hash = hashed_keys[i];
            uint64_t slot = get_slot(hash);

            // We get the write position from the directory.
            uint64_t write_pos = directory[slot] >> 16;
            
            // We write the tuple to the contiguous array.
            int32_t key = build_side_table[key_column][i].i32;
            data[write_pos] = {key, i}; 

            // Move pointer to next free slot. In the end, the pointer will point
            // to the end of the segment.
            directory[slot] += (1ULL << 16);

            // update bloom filter
            directory[slot] |= compute_bloom_bits(hash);
        }
    }
        
    
    template <typename Func>
    void lookup(Key key, Func process_match) {
        uint64_t hashed_key = hash32(static_cast<uint32_t>(key), 0);
        uint64_t slot = get_slot(hashed_key);

        auto seg_info = get_segment_info(slot);

        if (!could_contain_64(seg_info.bloom, hashed_key)) return;

        for (uint64_t i = seg_info.start; i < seg_info.end; i++) {
            if (data[i].first == key) {
                
                // For every match, we run a lambda function provided by the caller.
                process_match(data[i].second);
            }
        }
    }
    
    
};