#define CATCH_CONFIG_MAIN
#include <catch2/catch_test_macros.hpp>
#include "engine/column.h" 

// ------------------------------------------------------------
// Helper to unpack a constructed StringRef using bit masks
// ------------------------------------------------------------
static uint64_t mask(uint64_t value, int shift, uint64_t bits) {
    return (value >> shift) & bits;
}

// ------------------------------------------------------------
// Tests for StringRef
// ------------------------------------------------------------
TEST_CASE("StringRef: null construction and detection") {
    StringRef s = StringRef::null();

    REQUIRE(s.is_null());
    REQUIRE(s.is_long() == false);

    // All other fields should be zero
    REQUIRE(s.table_id() == 0);
    REQUIRE(s.column_id() == 0);
    REQUIRE(s.page_id() == 0);
    REQUIRE(s.offset() == 0);
}

TEST_CASE("StringRef: short-string construction and field extraction") {
    uint8_t table = 3;
    uint8_t column = 7;
    uint16_t page = 512;
    uint32_t offset = 0x12345; // lower 30 bits allowed

    StringRef s(table, column, page, offset, false);

    REQUIRE(!s.is_null());
    REQUIRE(!s.is_long());

    REQUIRE(s.table_id() == table);
    REQUIRE(s.column_id() == column);
    REQUIRE(s.page_id() == page);
    REQUIRE(s.offset() == (offset & 0x3FFFFFFF));
}

TEST_CASE("StringRef: long-string construction and field extraction") {
    uint8_t table = 1;
    uint8_t column = 2;
    uint16_t page = 0xBEEF;
    uint32_t offset = 0; // ignored for long strings

    StringRef s(table, column, page, offset, true);

    REQUIRE(!s.is_null());
    REQUIRE(s.is_long());

    REQUIRE(s.table_id() == table);
    REQUIRE(s.column_id() == column);
    REQUIRE(s.page_id() == page);
    REQUIRE(s.offset() == 0);
}

TEST_CASE("StringRef: bitpacking correctness") {
    uint8_t table = 42;
    uint8_t column = 123;
    uint16_t page = 655; 
    uint32_t offset = (1u << 29) - 1; // largest valid short-string offset

    StringRef s(table, column, page, offset, false);

    REQUIRE(!s.is_long());
    REQUIRE(mask(s.raw, 54, 0xFF) == table);
    REQUIRE(mask(s.raw, 46, 0xFF) == column);
    REQUIRE(mask(s.raw, 30, 0xFFFF) == page);
    REQUIRE(mask(s.raw, 0, 0x3FFFFFFF) == offset);
}

// ------------------------------------------------------------
// Tests for value_t
// ------------------------------------------------------------
TEST_CASE("value_t: default constructor creates NULL") {
    value_t v;

    REQUIRE(v.is_null());
}

TEST_CASE("value_t: INT32 constructor") {
    value_t v(123);

    REQUIRE(v.is_int());
    REQUIRE(!v.is_null());
    REQUIRE(v.get_int() == 123);
}

TEST_CASE("value_t: STRING constructor with non-null StringRef") {
    StringRef s(1, 2, 3, 99, false);
    value_t v(s);

    REQUIRE(v.is_string());
    REQUIRE(!v.is_null());
    REQUIRE(v.get_stringref().table_id() == 1);
    REQUIRE(v.get_stringref().column_id() == 2);
    REQUIRE(v.get_stringref().page_id() == 3);
    REQUIRE(v.get_stringref().offset() == 99);
}

TEST_CASE("value_t: STRING constructor with null StringRef") {
    StringRef s = StringRef::null();
    value_t v(s);

   

    REQUIRE(v.is_string());
    REQUIRE(v.is_null());
}

TEST_CASE("value_t: union correctness across type switches") {
    value_t v;

    // assign int
    v = value_t(777);
    REQUIRE(v.is_int());
    REQUIRE(v.get_int() == 777);
    REQUIRE(!v.is_null());

    // assign stringref
    StringRef s(9, 8, 7, 1234, false);
    v = value_t(s);
    REQUIRE(v.is_string());
    REQUIRE(v.get_stringref().table_id() == 9);
    REQUIRE(v.get_stringref().column_id() == 8);
    REQUIRE(v.get_stringref().page_id() == 7);
    REQUIRE(v.get_stringref().offset() == 1234);
}

TEST_CASE("value_t: null detection rules") {
    // explicit null
    value_t v_null;
    REQUIRE(v_null.is_null());

    // non-null INT32
    value_t v_int(5);
    REQUIRE(!v_int.is_null());

    // non-null string
    value_t v_str(StringRef(1,2,3,4,false));
    REQUIRE(!v_str.is_null());

    // null string
    value_t v_str_null(StringRef::null());
    REQUIRE(v_str_null.is_null());
}
