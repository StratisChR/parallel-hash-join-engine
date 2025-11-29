#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

/* 
    value_t: The new data type for either saving a 32-bit integer, or 
    pointing to a VARCHAR value in order to achieve late materialization.
    We use the full 64 bits that the exercise instruction allowed.
    We can compress it further, but the given format seemed smart
    and any further compression would limit the size of the DB we could 
    handle. To be exact, further compression would limit us to amounts
    that feel unsafe (e.g. very few tables). If needed, it can be easily 
    optimized for a given dataset.
*/
struct value_t {
    uint64_t data;

    // Exact format of the 64 bits:
    //
    // BIt 63: Is NULL check. (1 = NULL, 0 = NOT NULL)
    // Bit 62: TYPE check. (1 = VARCHAR string, 0 = 32 bit integer)
    // Bit ...: Long string check.
    // Bits 54-61 (8 bits): Table Index. We assume most datasets don't have more than 256
    //                      tables. This could be reduced in our current project, but it's
    //                      not an important enough optimization to justify losing flexibility.
    // Bits 46-53 (8 bits): Column Index. The same apllies here as for the tables. We want 
    //                      to save this index second because we work through a ColumnarTable
    //                      to access the data. This structure has a vector of the columns
    //                      of the current table, and then each column has a vector of pages.
    // Bits 12-45 (34 bits):Page Index. We assume that no column will have more than 2^34 pages.
    //                      If each page is 8KB, this gives a maximum size of 128TiB per column.
    //                      This is an absurd amount of data.
    // Bits 0-11 (12 bits): Row Index. Each page is 8KB = 2^13 bytes. Even if we only stored 
    //                      empty strings, the offset of each string would take up 2 bytes, so
    //                      we can have at most 2^12 = 4096 rows per page. But that is only if
    //                      we didn't a header or bitmap in each page. So 12 bits is plenty safe.


    static constexpr uint64_t NULL_MASK       = 1ULL << 63;
    static constexpr uint64_t TYPE_MASK       = 1ULL << 62;
    
    // Helper to identify empty/null explicitly if needed outside the struct
    static constexpr uint64_t NULL_INT        = std::numeric_limits<uint64_t>::max(); 

    // Constructors:


    value_t() : data(NULL_MASK) {} // Default to NULL 


    static value_t make_null() {
        return value_t(); 
    }

    // Constructor for Integers
    explicit value_t(int32_t val) {
        // When casting a signed int to unsigned, the sign-bit gets extended
        // as to not get lost. That would mess with our flags for NULL and TYPE.
        // Instead, we first cast to uint32_t, so it only fills the lower 32 bits.
        // Then we cast to uint64_t to store in data.
        data = static_cast<uint64_t>(static_cast<uint32_t>(val));
        // After casting to uint64_t, the upper bits are guaranteed to be 0,
        // so our NULL and TYPE bits are correctly set to 0.
    }

    // Constructor for String Pointers
    // table_idx: index from "base_table_id" in Plan's ScanNode
    // col_idx:   index from "columns" vector in Plan's ColumnarTable
    // page_idx:  index from "pages" vector in ColumnarTable's Column
    // row_idx:   row index inside the page
    value_t(uint8_t table_idx, uint8_t col_idx, uint64_t page_idx, uint16_t row_idx) {
        uint64_t table_part = static_cast<uint64_t>(table_idx) << 54;
        uint64_t col_part   = static_cast<uint64_t>(col_idx)   << 46;
        uint64_t page_part  = page_idx  << 12;
        uint64_t row_part   = static_cast<uint64_t>(row_idx); // uses bits 0-11 

        // Set TYPE_MASK to 1 to indicate String Pointer
        data = TYPE_MASK | table_part | col_part | page_part | row_part;
    }

    // functions:
    
    bool is_null() const {
        return (data & NULL_MASK) != 0;
    }

    bool is_int() const {
        // It is an Int IF: It is NOT Null AND Type bit is 0
        return !is_null() && ((data & TYPE_MASK) == 0);
    }

    bool is_string() const {
        // It is a String IF: It is NOT Null AND Type bit is 1
        return !is_null() && ((data & TYPE_MASK) != 0);
    }

    // We return the 32bit integer value.
    int32_t get_int() const {
        return static_cast<int32_t>(static_cast<uint32_t>(data));
    }

    // Unpack String Components
    void get_string_ptr(uint8_t& table, uint8_t& col, uint64_t& page, uint16_t& row) const {
        table = (data >> 54) & 0xFF;
        col   = (data >> 46) & 0xFF;
        page  = (data >> 12) & 0x3FFFFFFFFULL; // Mask for 34 bits
        row   = data & 0xFFF;           // Mask for 12 bits
    }
};