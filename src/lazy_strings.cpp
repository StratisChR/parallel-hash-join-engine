#include "engine/lazy_strings.h"

namespace Contest {

// Called at the start of every query. Input tables differ between queries, and a new
// column can live at the same address as an old one, so old entries must not survive.
void reset_string_columns() {
    for (size_t slot : g_string_columns_used) g_string_columns[slot] = StringColumnIndex{};
    g_string_columns_used.clear();
}

// Scan of a VARCHAR column: reads only the page headers. No row is converted.
void scan_lazy_string_column(const Column& column, size_t table_id, size_t in_col_idx, column_t& target_col) {
    assert(table_id < 256 && in_col_idx < 256);
    StringColumnIndex& index = string_column_index(uint8_t(table_id), uint8_t(in_col_idx));
    if (index.column != &column) {               // not indexed yet in this query
        index.column = &column;
        g_string_columns_used.push_back(table_id * 256 + in_col_idx);

        index.first_row.assign(column.pages.size() + 1, 0);
        for (size_t p = 0; p < column.pages.size(); ++p) {
            uint16_t n = *reinterpret_cast<const uint16_t*>(column.pages[p]->data);
            size_t rows = (n == 0xFFFF) ? 1 : (n == 0xFFFE) ? 0 : n;   // long start / continuation / normal
            index.first_row[p + 1] = index.first_row[p] + rows;
        }

        // block_page[b] = the page whose row range contains row b << BLOCK_SHIFT.
        const size_t total  = index.first_row.back();
        const size_t blocks = (total >> StringColumnIndex::BLOCK_SHIFT) + 1;
        index.block_page.resize(blocks);
        size_t p = 0;
        for (size_t b = 0; b < blocks; ++b) {
            size_t row = b << StringColumnIndex::BLOCK_SHIFT;
            while (p + 1 < column.pages.size() && index.first_row[p + 1] <= row) ++p;
            index.block_page[b] = uint32_t(p);
        }
    }
    target_col.set_lazy_string(uint8_t(table_id), uint8_t(in_col_idx), index.first_row.back());
}

} // namespace Contest
