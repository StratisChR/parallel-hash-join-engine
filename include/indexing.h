#pragma once
#include <cstdint>
#include <cstddef>

// Structure to reference a string in the columnar store (64-bit)
struct StringRef {
    // Layout (64 bits):
    // [63]      : is_null
    // [62]      : is_long
    // [54..61]  : table_id (8 bits)
    // [46..53]  : column_id (8 bits)
    // [30..45]  : page_id (16 bits)
    // [0..29]   : offset within page (30 bits)
    uint64_t raw;

    StringRef() : raw(0) {}

    // Constructor
    StringRef(uint8_t table, uint8_t column, uint16_t page, uint32_t offset, bool is_long_str = false)
    {
        raw = 0;
        raw |= (uint64_t(is_long_str) << 62);
        raw |= (uint64_t(table) << 54);
        raw |= (uint64_t(column) << 46);
        raw |= (uint64_t(page) << 30);
        raw |= (offset & 0x3FFFFFFF); // 30 bits for offset
    }

    // Null constructor
    static StringRef null() {
        StringRef s;
        s.raw = (1ULL << 63); // is_null = 1
        return s;
    }

    bool is_null() const { return (raw >> 63) & 1; }
    bool is_long() const { return (raw >> 62) & 1; }

    uint8_t table_id() const { return (raw >> 54) & 0xFF; }
    uint8_t column_id() const { return (raw >> 46) & 0xFF; }
    uint16_t page_id() const { return (raw >> 30) & 0xFFFF; }
    uint32_t offset() const { return raw & 0x3FFFFFFF; }
};

// Row-store value type
struct value_t {
    enum class Type : uint8_t {
        INT32,
        STRING,
        NULLVAL
    };

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
