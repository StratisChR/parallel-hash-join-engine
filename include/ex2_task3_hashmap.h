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
   static constexpr uint64_t MAX_TUPLES_PER_SEGMENT = 6;
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

    // bool needs_array_rehash() const {
    //     return get_load_factor() > MAX_LOAD_FACTOR;
    // }

    bool needs_directory_rehash(uint64_t entry) const{
        return entry > MAX_TUPLES_PER_SEGMENT;
    }

    void calc_capacity(){
        if (num_elements == 0) {
            shift = 53;
            capacity = 1ULL << (64 - shift);  // 2048
            return;
        }
        uint64_t min_required = (num_elements * 100) / MAX_LOAD_FACTOR;

        // // We round up min_required to the next power of two
        // uint64_t n = min_required - 1;
        // n |= n >> 1;
        // n |= n >> 2;
        // n |= n >> 4;
        // n |= n >> 8;
        // n |= n >> 16;
        // n |= n >> 32;
        // uint64_t capacity = n + 1;

        
        // // We compute the k required for 2^k = capacity
        // int k = 0;
        // uint64_t temp = capacity;
        // while (temp > 1) {
        //     temp >>= 1;
        //     k++;
        // }
        // int computed_shift = 64 - k;

        // The following might be faster than the old version
        uint64_t capacity_val = (min_required == 0) ? 1 : 
            (1ULL << (64 - __builtin_clzll(min_required - 1)));
        
        int k = __builtin_ctzll(capacity_val);
        int computed_shift = 64 - k;
        
        //    We keep at least a size of 2^11
        shift = (computed_shift < 53) ? computed_shift : 53;
        capacity = 1ULL << (64 - shift);
    }

private: // Everything related to rehashing/resizing:

    void double_directory(){
        if (shift <= 35) {
            throw std::runtime_error("Cannot double directory further - shift would be negative");
        }
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
    
        std::fill(data.begin(), data.end(), std::pair<Key, Value>{EMPTY_KEY, Value()});

        // Build prefix sum for pointers
        uint64_t cumsum = 0;
        for (uint64_t i = 0; i < capacity; i++) {
            cumsum += directory[i];
            directory[i] = cumsum << 16;  // Store as pointer
        }
    }

    // // Depending on how I implement the resize, this might be useful, but probably not 
    // // since each insert already updates the bloom filter.
    // void rebuild_bloom_for_slot(u64 slot);

private: // Support for the external operations:

    // I might use something like this to exactly find the available position for 
    // a new tuple in the segment when inserting.
    uint64_t find_insert_position(uint64_t hash){
        uint64_t slot_in_directory = get_slot(hash);
        
        // Calculate start and end of segment
        uint64_t seg_start = (slot_in_directory == 0) ? 0 : get_slot_pointer(slot_in_directory - 1);
        uint64_t seg_end = get_slot_pointer(slot_in_directory);
        
        for(uint64_t i = seg_start; i < seg_end; i++){
            if(data[i].first == EMPTY_KEY){
                return i;
            }
        }
        return NOT_FOUND;
    }

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
    void create_directory(std::vector<std::vector<value_t>>& build_side_table, size_t key_column){
        // Count unique keys first
        std::unordered_set<int32_t> unique_keys;
        unique_keys.reserve(build_side_table.size());

        for (const auto& row : build_side_table) {
            unique_keys.insert(row[key_column].i32);
        }
        
        num_elements = unique_keys.size();  // Use unique key count!
        calc_capacity();
        if (capacity != 2048){
            grow_directory();
        }

        bool start_again = true;
        while(start_again){
            start_again = false;
            std::fill(directory.begin(), directory.end(), 0);

            // Pre-hash all keys once
            std::vector<std::pair<uint64_t, int32_t>> hashed_keys;
            hashed_keys.reserve(unique_keys.size());
            
            for (const auto& key : unique_keys) {
                uint64_t hashed = hash32(static_cast<uint32_t>(key), 0);
                hashed_keys.emplace_back(hashed, key);
            }

            for (const auto& [hashed, _] : hashed_keys) {
                uint64_t slot = get_slot(hashed);
                directory[slot]++;
                
                if (directory[slot] > MAX_TUPLES_PER_SEGMENT) {
                    double_directory();
                    start_again = true;
                    break;
                }
            }
        }
        build_data_array();
    }

    // Might call it emplace to make simple drop in for execute.cpp possible
    void insert(const Key& key, const Value& value){
        uint64_t hashed_key = hash32(static_cast<uint32_t>(key), 0);
        uint64_t slot = get_slot(hashed_key);

        auto seg_info = get_segment_info(slot);

        // Check for existing key AND find empty slot in one pass
        uint64_t empty_pos = NOT_FOUND;
        for(uint64_t i = seg_info.start; i < seg_info.end; i++){
            if (data[i].first == key) {
                // Found existing - append and return
                data[i].second.insert(data[i].second.end(), value.begin(), value.end());
                return;
            }
            if (empty_pos == NOT_FOUND && data[i].first == EMPTY_KEY) {
                empty_pos = i;
            }
        }
        
        // Not found - insert at empty position
        if(empty_pos == NOT_FOUND){
            throw std::runtime_error("Segment full!");
        }
        
        data[empty_pos] = {key, value};

        // uint64_t position = find_insert_position(hashed_key);
        // if(position == NOT_FOUND){
        //     throw std::runtime_error("Segment full! We must have done something wrong when creating directory and array");
        // }
        // // add key and  value
        // data[position] = {key, value};

        uint16_t bloom_tag = compute_bloom_bits(hashed_key);
        update_bloom_filter(slot, bloom_tag);
    }

    // lookup can call produce matches.
    // It returns a pointer to our values, not our actual values.
    std::pair<Key, Value>* lookup(Key key){
        uint64_t hashed_key = hash32(static_cast<uint32_t>(key), 0);
        uint64_t slot = get_slot(hashed_key);

        auto seg_info = get_segment_info(slot);

        if(!could_contain(seg_info.bloom, hashed_key)){
            return nullptr;
        }

        for (uint64_t i = seg_info.start; i < seg_info.end; i++) {
            if (data[i].first == key) {
                return &data[i];
            }  
        }

        return nullptr;
    }
    
    // std::pair<Key, Value>* produce_matches(Key key, uint64_t slot, uint64_t entry){
    //     uint64_t start;
    //     if (slot == 0)
    //     {
    //         start = 0;
    //     }else{
    //         start = get_slot_pointer(slot-1);
    //     }
    //     uint64_t end = entry >> 16;

    //     for (uint64_t array_index = start; array_index < end; array_index++)
    //     {
    //         if (data[array_index].first == key)
    //         {
    //             return &data[array_index];
    //         }  
    //     }
    //     return nullptr;
    // }
};