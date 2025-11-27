#include <catch2/catch_test_macros.hpp>

#include <value_t.h>

// We have to test the following cases:
//
// Construct a value_t vector containg:
//      - NULL values
//      - Integers
//      - VARCHAR strings
// Does it construct it as expected?
//
// Can we get the correct values back?

// Constructing the value_t vector:
TEST_CASE("Constructing and accessing value_t vectors with only integers", "[value_t]") {
    std::vector<value_t> vec;

    value_t val1, val2, val3, val4, val5;

    val1 = value_t(42);
    val2 = value_t(-7);
    val3 = value_t::make_null();
    val4 = value_t(0);
    val5 = value_t(123456789);

    vec.emplace_back(val1);
    vec.emplace_back(val2);
    vec.emplace_back(val3);
    vec.emplace_back(val4);
    vec.emplace_back(val5);

    REQUIRE(vec.size() == 5);

    // Check values:
    REQUIRE((!((vec[0].data & value_t::NULL_MASK) != 0))); // Not NULL
    REQUIRE((!(vec[0].data & value_t::TYPE_MASK))); // Integer type
    REQUIRE(static_cast<int32_t>(vec[0].data) == 42);

    REQUIRE((!((vec[1].data & value_t::NULL_MASK) != 0))); // Not NULL
    REQUIRE((!(vec[1].data & value_t::TYPE_MASK))); // Integer type
    REQUIRE(static_cast<int32_t>(vec[1].data) == -7);

    REQUIRE(((vec[2].data & value_t::NULL_MASK) != 0)); // NULL

    REQUIRE((!((vec[3].data & value_t::NULL_MASK) != 0))); // Not NULL
    REQUIRE((!(vec[3].data & value_t::TYPE_MASK))); // Integer type
    REQUIRE(static_cast<int32_t>(vec[3].data) == 0);

    REQUIRE((!((vec[4].data & value_t::NULL_MASK) != 0))); // Not NULL
    REQUIRE((!(vec[4].data & value_t::TYPE_MASK))); // Integer type
    REQUIRE(static_cast<int32_t>(vec[4].data) == 123456789);

    int counter = 0;
    // check get_int() and is_int()
    for (const auto& element : vec)
    {
        if ( element.is_int() )
        {
            int32_t int_value = element.get_int();
            // We just verify that get_int() returns the same as static_cast
            REQUIRE(int_value == static_cast<int32_t>(element.data));
            counter++;
        }
    }
    REQUIRE(counter == 4);
}

// If we put table, column, page, and row indices into a value_t,
// can we get them back correctly?
TEST_CASE("Constructing and accessing value_t vectors with string pointers", "[value_t]") {
    std::vector<value_t> vec;
    value_t val1, val2, val3;

    val1 = value_t(3, 5, 12345, 678);
    val2 = value_t(0, 0, 0, 0);
    val3 = value_t(255, 255, 17179869183, 4095);

    vec.emplace_back(val1);
    vec.emplace_back(val2);
    vec.emplace_back(val3); 

    REQUIRE(vec.size() == 3);

    // Check val1
    REQUIRE((!((vec[0].data & value_t::NULL_MASK) != 0))); // Not NULL
    REQUIRE(((vec[0].data & value_t::TYPE_MASK) != 0)); // String type
    REQUIRE(((vec[0].data >> 54) & 0xFF) == 3); // Table index
    REQUIRE(((vec[0].data >> 46) & 0xFF) == 5); // Column index
    REQUIRE(((vec[0].data >> 12) & 0x3FFFFFFFFULL) == 12345); // Page index
    REQUIRE((vec[0].data & 0xFFF) == 678); // Row index

    // Check val1 with get_string_ptr
    uint8_t table, col;
    uint64_t page;
    uint16_t row;
    vec[0].get_string_ptr(table, col, page, row);

    REQUIRE(table == 3);
    REQUIRE(col == 5);
    REQUIRE(page == 12345);
    REQUIRE(row == 678);

    // Check val2
    REQUIRE((!((vec[1].data & value_t::NULL_MASK) != 0))); // Not NULL
    REQUIRE(((vec[1].data & value_t::TYPE_MASK) != 0)); // String type
    REQUIRE(((vec[1].data >> 54) & 0xFF) == 0); // Table index
    REQUIRE(((vec[1].data >> 46) & 0xFF) == 0); // Column index
    REQUIRE(((vec[1].data >> 12) & 0x3FFFFFFFFULL) == 0); // Page index
    REQUIRE((vec[1].data & 0xFFF) == 0); // Row index   

    // Check val2 with get_string_ptr
    vec[1].get_string_ptr(table, col, page, row);

    REQUIRE(table == 0);
    REQUIRE(col == 0);
    REQUIRE(page == 0);
    REQUIRE(row == 0);

    // Check val3
    REQUIRE((!((vec[2].data & value_t::NULL_MASK) != 0))); // Not NULL
    REQUIRE(((vec[2].data & value_t::TYPE_MASK) != 0)); // String type
    REQUIRE(((vec[2].data >> 54) & 0xFF) == 255); // Table index
    REQUIRE(((vec[2].data >> 46) & 0xFF) == 255); // Column index
    REQUIRE(((vec[2].data >> 12) & 0x3FFFFFFFFULL) == 17179869183); // Page index
    REQUIRE((vec[2].data & 0xFFF) == 4095); // Row index

    // Check val3 with get_string_ptr
    vec[2].get_string_ptr(table, col, page, row);

    REQUIRE(table == 255);
    REQUIRE(col == 255);
    REQUIRE(page == 17179869183);
    REQUIRE(row == 4095);

    // Check NULL and is_string()
    int counter = 0;
    for (const auto& element : vec)
    {
        if ( element.is_string() )
        {
            uint8_t  table_idx, col_idx;
            uint64_t page_idx;
            uint16_t row_idx;
            element.get_string_ptr(table_idx, col_idx, page_idx, row_idx);
            // We just verify that we can retrieve the values without error
            counter++;
        }
    }
    REQUIRE(counter == 3);
}    

