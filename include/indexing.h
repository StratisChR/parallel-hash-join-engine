#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <vector>
#include <memory>
#include <iterator>
#include <cassert>
#include "attribute.h"

// forward-declare Page from plan.h so column_t can hold views to input pages
struct Page;

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



constexpr size_t PAGE_CAPACITY = 4096;

struct ColPage {
    value_t data[PAGE_CAPACITY];
};

class column_t {
public:

    // Owned materialized pages (value_t arrays)
    std::vector<std::unique_ptr<ColPage>> pages;

    // View into existing Plan::Page buffers (non-owning). Stored as raw
    // pointers to the page data (`std::byte*`) so we don't need the full
    // definition of `Page` here.
    std::vector<std::byte*> view_pages;
    DataType view_type = DataType::INT32;
    bool is_view = false; // true when this column is a view into input pages
    // Starting row index for each page in `view_pages` (prefix sums). Used to
    // map a global index -> page when in view mode. view_page_offsets[i] is
    // the starting row index of view_pages[i].
    std::vector<size_t> view_page_offsets;

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

    // Return a value at absolute index. If this column is a view into input
    // pages (is_view == true) we decode directly from the underlying Page
    // memory. Currently only optimized for INT32 no-null pages.
    value_t operator[](size_t idx) const {
        if (!is_view) {
            return pages[idx / PAGE_CAPACITY]->data[idx % PAGE_CAPACITY];
        } else {
            // view backed by Plan::Page pointers
            // find page by offsets (pages can have varying row counts)
            auto it = std::upper_bound(view_page_offsets.begin(), view_page_offsets.end(), idx);
            size_t page_idx = (it == view_page_offsets.begin()) ? 0 : (size_t)(it - view_page_offsets.begin() - 1);
            size_t in_page_idx = idx - view_page_offsets[page_idx];
            std::byte* raw = view_pages[page_idx];
            // num_rows stored at offset 0, num_non_null at offset 2
            uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw);
            if (in_page_idx >= num_rows) return value_t();
            if (view_type == DataType::INT32) {
                // For view mode we only enable columns where every row is non-null
                // so values are densely stored starting at offset 4.
                int32_t val = *reinterpret_cast<int32_t*>(raw + 4 + in_page_idx * sizeof(int32_t));
                return value_t(val);
            }
            // Fallback: return null for unsupported view types
            return value_t();
        }
    }

    // Configure this column as a view into existing Plan::Page buffers.
    // We accept a vector of raw `std::byte*` pointers to the page data so
    // this header doesn't need the full `Page` definition.
    // Caller must ensure pages contain only fully non-null INT32 data.
    void set_view_pages(const std::vector<std::byte*>& src_pages_data, size_t total_items_, DataType dtype) {
        view_pages = src_pages_data;
        view_type = dtype;
        is_view = true;
        total_items = total_items_;

        // build offsets: each entry is the starting global index of the
        // corresponding page. Read num_rows from the page header.
        view_page_offsets.clear();
        view_page_offsets.reserve(view_pages.size());
        size_t cumsum = 0;
        for (auto* raw : view_pages) {
            view_page_offsets.push_back(cumsum);
            uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw);
            cumsum += num_rows;
        }
        // (total_items_ should equal cumsum)
    }
};