#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <memory>
#include <iterator>
#include <cassert>

// ========================================================================
//  PART 1: STRING & VALUE TYPES
// ========================================================================

struct StringRef {
    uint64_t raw;

    StringRef() : raw(0) {}

    StringRef(uint8_t table, uint8_t column, uint16_t page, uint32_t offset, bool is_long_str = false) {
        raw = 0;
        raw |= (uint64_t(is_long_str) << 62);
        raw |= (uint64_t(table) << 54);
        raw |= (uint64_t(column) << 46);
        raw |= (uint64_t(page) << 30);
        raw |= (offset & 0x3FFFFFFF); 
    }

    static StringRef null() {
        StringRef s;
        s.raw = (1ULL << 63);
        return s;
    }

    bool is_null() const { return (raw >> 63) & 1; }
    bool is_long() const { return (raw >> 62) & 1; }
    uint8_t table_id() const { return (raw >> 54) & 0xFF; }
    uint8_t column_id() const { return (raw >> 46) & 0xFF; }
    uint16_t page_id() const { return (raw >> 30) & 0xFFFF; }
    uint32_t offset() const { return raw & 0x3FFFFFFF; }
};

struct value_t {
    enum class Type : uint8_t { INT32, STRING, NULLVAL };

    Type type;
    union {
        int32_t i32;
        StringRef str;
    };

    value_t() : type(Type::NULLVAL), i32(0) {}
    explicit value_t(int32_t val) : type(Type::INT32), i32(val) {}
    explicit value_t(const StringRef& s) : type(Type::STRING), str(s) {}

    bool is_null() const { return type == Type::NULLVAL || (type == Type::STRING && str.is_null()); }
};



constexpr size_t PAGE_CAPACITY = 8192;

struct ColPage {
    value_t data[PAGE_CAPACITY];
};

class column_t {
public:

    std::vector<std::unique_ptr<ColPage>> pages;
    size_t total_items = 0;

    column_t() : total_items(0) {}

    column_t(column_t&&) = default;
    column_t& operator=(column_t&&) = default;
    column_t(const column_t&) = delete;
    column_t& operator=(const column_t&) = delete;

    void push_back(const value_t& val) {
        size_t offset = total_items % PAGE_CAPACITY;
        if (offset == 0) {
            pages.push_back(std::make_unique<ColPage>());
        }
        pages.back()->data[offset] = val;
        total_items++;
    }

    size_t size() const {
        return total_items;
    }

    value_t operator[](size_t idx) const {
        return pages[idx / PAGE_CAPACITY]->data[idx % PAGE_CAPACITY];
    }
};