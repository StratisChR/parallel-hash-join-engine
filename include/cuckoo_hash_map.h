#pragma once

#include <vector>
#include <utility>
#include <functional>
#include <cstddef>
#include <int_hasher.h>

namespace Contest {


template <class Key, class Value, class Hasher = std::hash<Key>>
class CuckooHashMap {
private:
    
    // Bucket structure to hold key-value pairs.
    struct Bucket {
        Key   key;
        Value value;
        bool  used = false;
    };

    // Our tables.
    std::vector<Bucket> _table1;
    std::vector<Bucket> _table2;
    size_t              _size = 0; // Total number of items.
    Hasher              _hasher;

    size_t              _capacity_mask;

    // Size of "cycle". After this many kicks, we rehash.
    size_t _max_kicks = 100;

public:

    class iterator {
    private:
        Bucket* _bucket;
        
        // A proxy to allow the 'itr->second' syntax.
        // That way we only change "unordered_map" to "CuckooHashMap" in the code.
        // It holds a reference to the value in the bucket.
        // We also use it in RobinHood.
        struct PairProxy {
            Value& second;
        };
        PairProxy _proxy;

    public:
        // Constructor for a valid iterator.
        iterator(Bucket* b) : _bucket(b), _proxy{b ? b->value : *((Value*)nullptr)} {}
        // Constructor for end iterator.
        iterator() : _bucket(nullptr), _proxy{*((Value*)nullptr)} {} // End iterator

        // When we call itr->second.push_back() this is what gets called.
        // It returns a pointer to our proxy object.
        // So it inserts _proxy.second which is a reference to the value in the bucket.
        PairProxy* operator->() { return &_proxy; }

        bool operator==(const iterator& other) const {
            return _bucket == other._bucket;
        }
        bool operator!=(const iterator& other) const {
            return _bucket != other._bucket;
        }
    };

private:
    size_t hash1(const Key& key) const {
        return _hasher(key) & _capacity_mask;
    }

    // Just a mixed-bits version of hash1.
    size_t hash2(const Key& key) const {
        size_t hash = _hasher(key);

        return (hash ^ (hash >> 16) + 0x9e3779b9) & _capacity_mask;
    }

    // Get the current capacity of table 1.
    size_t capacity() const {
        return _table1.size();
    }

public:
    // The operations:

    explicit CuckooHashMap(size_t initial_capacity = 128) {
        // Ensure capacity is a power of 2, and at least 1
        size_t cap = 1;
        while (cap < initial_capacity) cap <<= 1;
        
        _table1.resize(cap);
        _table2.resize(cap);
        // Set max_kicks based on capacity. A rule of thumb is log(N).
        // Let's set it to 100 for simplicity.

        _capacity_mask = cap - 1;

        _max_kicks = cap > 100 ? 100 : cap;
    }    

    // Our 'end' iterator is just a nullptr
    iterator end() {
        return iterator(nullptr);
    }

    iterator find(const Key& key) {
        if (capacity() == 0) return end();

        // Check table 1
        size_t pos1 = hash1(key);
        if (_table1[pos1].used && _table1[pos1].key == key) {
            return iterator(&_table1[pos1]);
        }

        // Check table 2
        size_t pos2 = hash2(key);
        if (_table2[pos2].used && _table2[pos2].key == key) {
            return iterator(&_table2[pos2]);
        }

        return end(); // Not found
    }

    

private:
    // Internal insert function. 
    bool insert_internal(Key& key, Value& value) {
        
        for (size_t i = 0; i < _max_kicks; ++i) {

            // 1. Try to insert into table 1.
            size_t pos1 = hash1(key);
            if (!_table1[pos1].used) {
                _table1[pos1] = {key, value, true};
                return true; // Success.
            }

            // 2. Slot is full.
            // We swap the items.
            std::swap(key, _table1[pos1].key);
            std::swap(value, _table1[pos1].value);

            // 3. Try to insert the "kicked" item into table 2.
            size_t pos2 = hash2(key);
            if (!_table2[pos2].used) {
                _table2[pos2] = {key, value, true};
                return true; // Success.
            }

            // 4. Slot is full.
            //    Swap items again.
            std::swap(key, _table2[pos2].key);
            std::swap(value, _table2[pos2].value);
        }
        // If we don't insert it within max_kicks:
        // We need to rehash.
        return false;
    }

    void rehash(Key item_in_hand_key, Value item_in_hand_val) {
        // We first save all the old items.
        std::vector<std::pair<Key, Value>> old_items;
        old_items.reserve(_size + 1);

        // Loops over all buckets in both tables, saving
        // the tuples of (key,value) in the old_items vector.
        for (auto& bucket : _table1) {
            if (bucket.used) {
                old_items.push_back({bucket.key, bucket.value});
            }
        }
        for (auto& bucket : _table2) {
            if (bucket.used) {
                old_items.push_back({bucket.key, bucket.value});
            }
        }
        // We also need to add the item that caused the rehash.
        old_items.push_back({item_in_hand_key, item_in_hand_val});

        bool rehash_succeeded = false;
        while (!rehash_succeeded) {
            rehash_succeeded = true;

            // Double the capacity
            size_t new_cap = (capacity() == 0) ? 128 : capacity() * 2;
            _table1.clear(); _table1.resize(new_cap);
            _table2.clear(); _table2.resize(new_cap);
            
            // Update mask for correct hashing.
            _capacity_mask = new_cap - 1;

            _size = 0; // Reset the size

            // 3. Re-insert all old items using insert_internal
            for (const auto& pair : old_items) {
                Key k_copy = pair.first;
                Value v_copy = pair.second;
                // We call insert_internal directly, NOT emplace
                if (insert_internal(k_copy, v_copy)) {
                    _size++; // Manually increment size
                } else {
                    // Very rare to need rehash immdetiately after 
                    // rehashing. Just to be sure we check.
                    rehash_succeeded = false;
                    break; // Break from this `for` loop
                }
            }
        }
    }

public:
    // External insert function.
    void emplace(Key key, Value value) {
        // First, check if key already exists.
        // `emplace` does nothing if the key is already present.
        // JoinAlgorithm already checks so we do a check that isn't really needed.
        // However, it seems like a good practice.
        // Also, we check in the unit tests if it accepts duplicate keys.
        if (find(key) != end()) {
            return;
        }

        Key k_copy = key;
        Value v_copy = value;

        // Rehash if _size > capacity()
        if (_size + 1 > (size_t)(capacity() * 0.8)) {
            rehash(k_copy, v_copy);
            return;
        }

        if (!insert_internal(k_copy, v_copy)) {
            // We failed to insert, we need to rehash.
            rehash(k_copy, v_copy); // Rehash with the item we couldn't insert
            // The rehash will handle the insertion.
            return; 
        }

        _size++;
    }
};

}