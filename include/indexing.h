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

// Compact 64-bit value representation. We pack either a 32-bit integer
// or a pointer-like StringRef into a single uint64_t. The layout reserves
// bit 63 as NULL flag, and bit 61 as TYPE flag (1 = STRING, 0 = INT32).
// The remaining bits keep the original StringRef packing when storing
// string pointers so we can reuse StringRef helpers.
struct value_t {
    enum class Type : uint8_t { INT32, STRING, NULLVAL };

    uint64_t data = 0;

    static constexpr uint64_t NULL_MASK = (1ULL << 63);
    // We use bit 61 for the "is-string" type marker to avoid colliding
    // with StringRef's bit62 (which indicates "long string").
    static constexpr uint64_t TYPE_MASK = (1ULL << 61);

    value_t() { data = NULL_MASK; }

    static value_t make_null() { return value_t(); }

    explicit value_t(int32_t val) {
        // store the 32-bit integer in the low bits. Ensure NULL and TYPE are 0.
        data = static_cast<uint64_t>(static_cast<uint32_t>(val));
    }

    explicit value_t(const StringRef& s) {
        // store the raw StringRef bits, and always mark TYPE_MASK to indicate
        // that this value_t represents a STRING value (even if the string is null)
        data = s.raw | TYPE_MASK;
    }

    bool is_null() const { return (data & NULL_MASK) != 0; }
    // Note: strings keep the TYPE bit even when they are null. This mirrors the
    // prior behavior where a value could have Type::STRING and still be null.
    bool is_string() const { return ((data & TYPE_MASK) != 0); }
    bool is_int() const { return !is_string() && !is_null(); }

    // Compatibility accessor for older code that used a "type" field.
    // If TYPE_MASK is set we report STRING (even if the underlying string is null),
    // otherwise if NULL_MASK is set we report NULLVAL, else INT32.
    Type type() const {
        if ((data & TYPE_MASK) != 0) return Type::STRING;
        if (is_null()) return Type::NULLVAL;
        return Type::INT32;
    }

    int32_t get_int() const {
        return static_cast<int32_t>(static_cast<uint32_t>(data & 0xFFFFFFFFu));
    }

    // Return a StringRef view to the raw bits stored. This strips the
    // TYPE_MASK bit so the returned StringRef has the same layout used
    // elsewhere in the code.
    StringRef get_stringref() const {
        StringRef s;
        // clear TYPE_MASK if present, keep all other bits (including NULL/long)
        s.raw = data & ~TYPE_MASK;
        return s;
    }
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
    size_t view_rows_per_page = 0;   // rows in every page except possibly the last
    bool   view_uniform       = false; // true if the above holds, so we can divide

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
            size_t page_idx, in_page_idx;
            if (view_uniform) {
                page_idx    = idx / view_rows_per_page;
                in_page_idx = idx % view_rows_per_page;
            } else {
                auto it = std::upper_bound(view_page_offsets.begin(), view_page_offsets.end(), idx);
                page_idx    = (it == view_page_offsets.begin()) ? 0 : (size_t)(it - view_page_offsets.begin() - 1);
                in_page_idx = idx - view_page_offsets[page_idx];
            }
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
        // All pages except the last should hold the same number of rows
        // (assignment: non-null columns are densely packed). Verify instead of assuming.
        view_uniform = false;
        if (!view_pages.empty()) {
            view_rows_per_page = *reinterpret_cast<uint16_t*>(view_pages[0]);
            view_uniform = view_rows_per_page > 0;
            for (size_t i = 0; i + 1 < view_pages.size(); ++i) {
                if (*reinterpret_cast<uint16_t*>(view_pages[i]) != view_rows_per_page) {
                    view_uniform = false;
                    break;
                }
            }
        }
    }
};