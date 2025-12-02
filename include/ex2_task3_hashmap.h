#pragma once

#include <cstdint>
#include <vector>
#include <algorithm>

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
   we will store in its segment. If in any segment we have more than
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
   static constexpr uint64_t MAX_TUPLES_PER_SEGMENT = 4;
   static constexpr double MAX_LOAD_FACTOR = 65; // we write it in %
   
   // Each entry in the directory contains a 64 bit unsigned, which is divided in two parts
   // The first 48 bits contain a pointer to the array of tuples. The next 16 bits, which
   // are not used in pointers, contain the bloom filter for each segment.
   // Before it starts doing the operations above, it used to save how many tuples
   // we can fit in each segment.
   std::vector<uint64_t> directory;

   std::vector<std::pair<Key, Value>> data;
    
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
        size_t capacity = 1ULL << (64 - shift);
        directory.resize(capacity, 0);  
        
        // // Initialize data array
        // capacity = 2048;  // Start with some initial capacity
        // data.resize(capacity);  // ← Changed from reserve() to resize()
    
        // // Mark all slots as empty
        // for(auto& pair : data) {
        //     pair.first = EMPTY_KEY;  // Assuming you define EMPTY_KEY
        //     pair.second = 0;
        // }

        // Precompute Bloom filter tags
        precompute_bloom_tags();
    }
    
    // Destructor (automatic with vectors, but explicit is fine)
    ~UnchainedHashMap() = default;

private: // Utilities:

    uint64_t get_slot(uint64_t hash){
        return hash >> shift;
    }

    uint64_t get_entry(uint64_t slot) {
        return directory[slot];
    }

    void set_slot_pointer(uint64_t directory_slot, uint64_t array_index){
        directory[directory_slot] = array_index << 16;
    }

    uint64_t get_slot_pointer(uint64_t slot) {
        uint64_t entry = get_entry(slot);
        uint64_t pointer = (entry & POINTER_MASK) >> 16;
        return pointer;
    }

    uint16_t get_slot_filter(uint64_t slot) {
        uint64_t entry = get_entry(slot);
        uint64_t filter = static_cast<uint16_t>(entry & BLOOM_MASK);
        return filter;

    }

    int get_segment_size(uint64_t hash) const {
         uint64_t slot = get_slot(hash);
         
         if ((slot) == 0) {
             // First segment
             return get_slot_pointer(slot);
         }
         // Pointers store positions in the segments array, not actual memory addresses.
         // So a simple subtraction gives us the size in number of tuples.
        return get_slot_pointer(slot) - get_slot_pointer(slot - 1);
    }

    double get_load_factor() const {
        return static_cast<double>(num_elements) / static_cast<double>(capacity);
    }

    bool needs_array_rehash() const {
        return get_load_factor() > MAX_LOAD_FACTOR;
    }

    bool needs_directory_rehash(uint64_t entry) const{
        return entry > MAX_TUPLES_PER_SEGMENT;
    }

    void calc_capacity(){
        uint64_t min_required = (num_elements * 100) / MAX_LOAD_FACTOR;

        // We round up min_required to the next power of two
        uint64_t n = min_required - 1;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        uint64_t capacity = n + 1;

        
        // We compute the k required for 2^k = capacity
        int k = 0;
        uint64_t temp = capacity;
        while (temp > 1) {
            temp >>= 1;
            k++;
        }
        int computed_shift = 64 - k;

        
        //    We keep at least a size of 2^11
        int shift = (computed_shift < 53) ? computed_shift : 53;

        //    Since we might have changed the shift, the real capacity is:
        uint64_t final_capacity = 1ULL << (64 - shift);
        capacity = static_cast<size_t>(final_capacity);
    }

private: // Everything related to rehashing/resizing:

    void double_directory(){
        shift--;
        capacity = 1ULL << (64 - shift);
        directory.resize(capacity,0);
    }

    // THis function first decreases shift by one. Then we allocate a new directory
    // and redestribute all tuples from the old dir to the new one.
    // While redestributing, we also rebuild the bloom filters.
    void grow_directory(){
        directory.resize(capacity, 0); 
    }

    // Create array, separate segments based on number in each directory slot.
    // It also creates the pointers in the directory.
    void build_data_array(){
        // Initialize data array
        data.resize(num_elements);  // ← Changed from reserve() to resize()
    
        // Mark all slots as empty
        for(auto& pair : data) {
            pair.first = EMPTY_KEY;  // Assuming you define EMPTY_KEY
            pair.second = 0;
        }

        for (uint64_t i = 0; i < capacity; i++)
        {
            uint64_t tuples_in_segment = directory[i];
            if (i == 0)
            {
                set_slot_pointer(i, tuples_in_segment);
            }else {
                uint64_t previous_slot_pointer = get_slot_pointer((i-1));
                set_slot_pointer(i, (previous_slot_pointer + tuples_in_segment));
            }    
        }
    }

    // Depending on how I implement the resize, this might be useful, but probably not 
    // since each insert already updates the bloom filter.
    void rebuild_bloom_for_slot(u64 slot);

private: // Support for the external operations:

    // I might use something like this to exactly find the available position for 
    // a new tuple in the segment when inserting.
    uint64_t find_insert_position(uint64_t hash){
        uint64_t slot_in_directory = get_slot(hash);
        uint64_t seg_pos = get_slot_pointer(slot_in_directory);
        uint64_t seg_size = get_segment_size(hash);
        for(uint64_t i = seg_pos; i < seg_pos + seg_size; i++){
            if(data[i].first == EMPTY_KEY){
                return i;
            }
        }
        return NOT_FOUND;
    }

    // We use it when inserting.
    void update_bloom_filter(uint64_t hash){
        uint16_t bloom_tag = compute_bloom_bits(hash);

        uint64_t slot = get_slot(hash);
        
        uint64_t pointer_part = directory[slot] & POINTER_MASK;
        uint16_t old_bloom = get_slot_filter(slot);
        uint16_t new_bloom = old_bloom | bloom_tag;

        directory[slot] = pointer_part | new_bloom;
    }

public: // External Operations:

    // The following function is called at the start of each join call. 
    // It is used to build the directory that will be used for the hash table when joining
    // the build and probe sides. 
    // We give it our build_side table, then iterate through all its tuples, hash their keys
    // and count how many of them have the same (64-shift) prefix. We do that by incrementing by 1
    // the number stored inside directory[prefix], which has 2^(64-shift) slots and all are initialized to 0.
    void create_directory(std::vector<std::vector<value_t>>& build_side_table, size_t key_column){
        num_elements = build_side_table.size();
        calc_capacity();
        if (capacity == 2024){
            continue;
        }else{
            grow_directory();
        }

        // I need to add the max 4 per slot limit again. I have only added the load factor limit as of now
        for (size_t row_idx = 0; row_idx < build_side_table.size(); ++row_idx) {
            
            // Get the key
            const std::vector<value_t>& row = build_side_table[row_idx];
            int32_t key = row[key_column];

            uint64_t hashed_key = hash32(static_cast<uint32_t>(key), 0);
            uint64_t slot_in_directory = get_slot(hashed_key);
            directory[slot_in_directory]++;
            if (needs_directory_rehash(directory[slot_in_directory]))
            {
                double_directory();
                row_idx = -1;
            }
        }

        build_data_array();
    }

    void insert(const Key& key, const Value& value){
        uint64_t hashed_key = hash32(static_cast<uint32_t>(key), 0);
        uint64_t position = find_insert_position(hashed_key);
        if(position == NOT_FOUND){
            throw std::runtime_error("Segment full! We must have done something wrong
                                    when creating directory and array");
        }
        // add key and  value
        data[position] = {key, value};

        update_bloom_filter(hashed_key);

        num_elements++;
    }

    // lookup has access to both the key and the hash value,
    // so that it can call compute matches.
    void lookup(Key key, uint64_t hash);
    
    void produce_matches(Key key, uint64_t slot, uint64_t entry);
}