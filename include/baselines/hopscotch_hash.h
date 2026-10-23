#pragma once
#include <vector>
#include <utility>
#include <functional>
#include <stdexcept>
#include <limits>
#include <cstdint>
#include <type_traits>
#include <algorithm>

/*
 * HopscotchHash<K, V, NEIGHBORHOOD>
 *
 * - Implements hopscotch hashing with hop-information bitmap.
 * - STL-compatible API (find, end, emplace) so it can replace std::unordered_map.
 * - Designed for small value types (like std::vector<size_t>).
 * - NEIGHBORHOOD <= 32 (uses 32-bit hop bitmap).
 */
template <typename Key, typename Value, size_t NEIGHBORHOOD = 32>
class HopscotchHash {
    static_assert(NEIGHBORHOOD > 0 && NEIGHBORHOOD <= 32,
                  "NEIGHBORHOOD must be between 1 and 32");

public:
    struct Bucket {
        Key key{};
        Value second{}; // same naming as std::pair for compatibility
        bool occupied = false;
        uint32_t hop_info = 0; // bitmap: bit i => item hashed here lives at offset i
    };

    // Iterator compatible with join code
    class iterator {
        Bucket* ptr = nullptr;
    public:
        iterator() = default;
        explicit iterator(Bucket* p) : ptr(p) {}
        Bucket& operator*() const { return *ptr; }
        Bucket* operator->() const { return ptr; }
        bool operator==(const iterator& other) const { return ptr == other.ptr; }
        bool operator!=(const iterator& other) const { return ptr != other.ptr; }
        Bucket* get() const { return ptr; }
        operator bool() const { return ptr != nullptr; }
    };

private:
    std::vector<Bucket> table;
    size_t mask = 0;
    size_t sz = 0;

    static constexpr size_t npos = std::numeric_limits<size_t>::max();
    static constexpr uint32_t H_MASK = (NEIGHBORHOOD == 32)
        ? 0xffffffffu
        : ((1u << NEIGHBORHOOD) - 1u);

public:
    explicit HopscotchHash(size_t capacity = 1024) {
        capacity = next_pow2(std::max<size_t>(capacity, 8));
        table.resize(capacity);
        mask = capacity - 1;
        sz = 0;
    }

    size_t size() const { return sz; }
    bool empty() const { return sz == 0; }
    iterator end() { return iterator(nullptr); }
    public:
    
    const std::vector<Bucket>& debug_table() const noexcept { return table; }
    size_t debug_mask() const noexcept { return mask; }
    size_t debug_bucket_index(const Key& k) const noexcept { return bucket_index(k); }


    // ---------------------------------------------------------
    // Find an element within the NEIGHBORHOOD window of its home bucket
    // ---------------------------------------------------------
    iterator find(const Key& key) {
        size_t home = bucket_index(key);
        uint32_t hop = table[home].hop_info;

        while (hop) {
            unsigned int bit = bit_scan_forward(hop);
            size_t pos = (home + bit) & mask;
            if (table[pos].occupied && table[pos].key == key)
                return iterator(&table[pos]);
            hop &= ~(1u << bit);
        }
        return end();
    }

    // ---------------------------------------------------------
    // Emplace (insert) — hopscotch algorithm following the steps you shared
    // ---------------------------------------------------------
    std::pair<iterator, bool> emplace(const Key& key, Value&& val) {
        if (auto it = find(key); it != end()) {
            return {it, false}; // duplicate key
        }

        if (sz * 2 >= table.size())
            rehash(table.size() * 2);

        size_t home = bucket_index(key);

        // Step 1: if hop_info full, table considered full -> rehash
        if ((table[home].hop_info & H_MASK) == H_MASK) {
            rehash(table.size() * 2);
            home = bucket_index(key);
        }

        // Step 2: find first free bucket j
        size_t free_idx = find_free_bucket(home);
        if (free_idx == npos) {
            rehash(table.size() * 2);
            home = bucket_index(key);
            free_idx = find_free_bucket(home);
            if (free_idx == npos)
                throw std::runtime_error("HopscotchHash: no free bucket after rehash");
        }

        // Step 3: move elements to bring the free slot within NEIGHBORHOOD
        while (distance(home, free_idx) >= NEIGHBORHOOD) {
            bool moved = false;

            for (size_t i = NEIGHBORHOOD - 1; i > 0; --i) {
                size_t candidate_home = (free_idx + table.size() - i) & mask;
                uint32_t hop = table[candidate_home].hop_info;

                int chosen_offset = -1;
                for (int o = (int)i - 1; o >= 0; --o) {
                    if (hop & (1u << o)) {
                        chosen_offset = o;
                        break;
                    }
                }

                if (chosen_offset >= 0) {
                    size_t from_pos = (candidate_home + chosen_offset) & mask;

                    table[free_idx].key = std::move(table[from_pos].key);
                    table[free_idx].second = std::move(table[from_pos].second);
                    table[free_idx].occupied = true;

                    table[from_pos].occupied = false;
                    table[candidate_home].hop_info &= ~(1u << chosen_offset);
                    table[candidate_home].hop_info |= (1u << i);

                    free_idx = from_pos;
                    moved = true;
                    break;
                }
            }

            if (!moved) {
                rehash(table.size() * 2);
                home = bucket_index(key);
                free_idx = find_free_bucket(home);
                if (free_idx == npos)
                    throw std::runtime_error("HopscotchHash: cannot move bucket during insert");
            }
        }

        // Step 4: place new element
        table[free_idx].key = key;
        table[free_idx].second = std::move(val);
        table[free_idx].occupied = true;

        size_t offset = distance(home, free_idx);
        table[home].hop_info |= (1u << offset);
        ++sz;
        return {iterator(&table[free_idx]), true};
    }

    void clear() {
        for (auto &b : table) {
            b.occupied = false;
            b.hop_info = 0;
        }
        sz = 0;
    }

private:
    // ---------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------
    size_t bucket_index(const Key& k) const {
        return std::hash<Key>{}(k) & mask;
    }

    static size_t next_pow2(size_t n) {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    size_t distance(size_t from, size_t to) const {
        return (to + table.size() - from) & mask;
    }

    size_t find_free_bucket(size_t start) const {
        for (size_t i = 0; i < table.size(); ++i) {
            size_t idx = (start + i) & mask;
            if (!table[idx].occupied)
                return idx;
        }
        return npos;
    }

    static unsigned int bit_scan_forward(uint32_t x) {
#if defined(_MSC_VER)
        unsigned long idx;
        _BitScanForward(&idx, x);
        return static_cast<unsigned int>(idx);
#else
        return static_cast<unsigned int>(__builtin_ctz(x));
#endif
    }

    void rehash(size_t new_capacity) {
        new_capacity = next_pow2(std::max<size_t>(new_capacity, 8));
        std::vector<Bucket> old_table = std::move(table);
        table.assign(new_capacity, {});
        mask = new_capacity - 1;
        sz = 0;
        for (auto &b : old_table)
            if (b.occupied)
                emplace(b.key, std::move(b.second));
    }
};
