// Lazy strings: a scan of a VARCHAR column copies nothing. Each value is a reference
// (table, column, row) into the input pages, resolved to characters only when the final
// result is written. resolve_string is called once per output string, so it lives in
// this header to stay inlinable.
#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <plan.h>
#include "engine/column.h"

namespace Contest {

// For every string column scanned in the current query: the first row number of each
// input page (a prefix sum of the page headers). Filled by the scans, which run on one
// thread before any output is produced; read (never written) during output.
struct StringColumnIndex {
    const Column*       column = nullptr;
    std::vector<size_t> first_row;   // first_row[p] = number of rows before page p; last entry = total
    // Page holding row (b << BLOCK_SHIFT), for every block b of 1024 rows. Lets resolve_string
    // find a row's page with a table lookup plus a few steps instead of a binary search.
    static constexpr unsigned BLOCK_SHIFT = 10;
    std::vector<uint32_t> block_page;
};
inline std::vector<StringColumnIndex> g_string_columns(256 * 256);   // indexed by table * 256 + column
inline std::vector<size_t>            g_string_columns_used;         // slots filled in this query

inline StringColumnIndex& string_column_index(uint8_t table, uint8_t column) {
    return g_string_columns[size_t(table) * 256 + column];
}

// Called at the start of every query. Input tables differ between queries, and a new
// column can live at the same address as an old one, so old entries must not survive.
void reset_string_columns();

// Scan of a VARCHAR column: reads only the page headers. No row is converted.
void scan_lazy_string_column(const Column& column, size_t table_id, size_t in_col_idx, column_t& target_col);

// Number of set bits among the first `count` bits of a bitmap.
inline size_t count_set_bits(const uint8_t* bitmap, size_t count) {
    size_t total = 0, i = 0;
    for (; i + 64 <= count; i += 64) {
        uint64_t w;
        std::memcpy(&w, bitmap + i / 8, 8);
        total += __builtin_popcountll(w);
    }
    for (; i + 8 <= count; i += 8) total += __builtin_popcount(bitmap[i / 8]);
    if (i < count) total += __builtin_popcount(bitmap[i / 8] & ((1u << (count - i)) - 1));
    return total;
}

// Turns a lazy string value into its characters. Returns false if the string is NULL.
// Short strings: `out` points straight into the input page. Long strings are assembled
// in `scratch` and `out` points there (valid until the next call with the same scratch).
inline bool resolve_string(value_t v, std::string& scratch, std::string_view& out) {
    const StringColumnIndex& index = string_column_index(lazy_string_table(v), lazy_string_column(v));
    assert(index.column != nullptr);
    const Column& column = *index.column;
    const size_t  row    = lazy_string_row(v);

    // The page holding `row`: start at the page of its 1024-row block and step forward
    // until the page's row range contains it. (Continuation pages of long strings hold
    // no rows, so the loop steps over them.)
    size_t p = index.block_page[row >> StringColumnIndex::BLOCK_SHIFT];
    while (index.first_row[p + 1] <= row) ++p;

    const std::byte* page   = column.pages[p]->data;
    const uint16_t   header = *reinterpret_cast<const uint16_t*>(page);

    if (header == 0xFFFF) {                       // long string: this page and its continuations
        scratch.clear();
        size_t idx = p;
        do {
            auto* pg = column.pages[idx]->data;
            scratch.append(reinterpret_cast<const char*>(pg + 4),
                           *reinterpret_cast<const uint16_t*>(pg + 2));
            ++idx;
        } while (idx < column.pages.size() &&
                 *reinterpret_cast<const uint16_t*>(column.pages[idx]->data) == 0xFFFE);
        out = scratch;
        return true;
    }

    const size_t   r      = row - index.first_row[p];             // row within the page
    const uint16_t rows   = header;
    const auto*    bitmap = reinterpret_cast<const uint8_t*>(page + PAGE_SIZE - (rows + 7) / 8);
    if (!(bitmap[r / 8] & (1u << (r % 8)))) return false;        // NULL

    const size_t   k            = count_set_bits(bitmap, r);     // non-null strings before this one
    const uint16_t num_non_null = *reinterpret_cast<const uint16_t*>(page + 2);
    const auto*    offsets      = reinterpret_cast<const uint16_t*>(page + 4);
    const char*    chars        = reinterpret_cast<const char*>(page + 4 + 2 * size_t(num_non_null));
    const uint16_t begin        = k ? offsets[k - 1] : 0;
    out = std::string_view(chars + begin, offsets[k] - begin);
    return true;
}

} // namespace Contest
