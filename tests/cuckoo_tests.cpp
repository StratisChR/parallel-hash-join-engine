#include <catch2/catch_test_macros.hpp>

#include <int_hasher.h>
#include <cuckoo_hash_map.h>

#include <set>

TEST_CASE("CuckooHashMap emplace and find", "[CuckooHashMap]") {
    using namespace Contest;

    CuckooHashMap<int, std::string, IntHasher> map;

    // Εισαγωγή στοιχείων
    map.emplace(1, "one");
    map.emplace(2, "two");
    map.emplace(3, "three");

    // Υπάρχοντα κλειδιά
    auto it1 = map.find(1);
    REQUIRE(it1 != map.end());
    REQUIRE(it1->second == "one");

    auto it2 = map.find(2);
    REQUIRE(it2 != map.end());
    REQUIRE(it2->second == "two");

    auto it3 = map.find(3);
    REQUIRE(it3 != map.end());
    REQUIRE(it3->second == "three");

    // Μη-υπάρχον κλειδί
    auto it4 = map.find(4);
    REQUIRE(it4 == map.end());

    // Εισαγωγή διπλότυπου κλειδιού (δεν πρέπει να αλλάξει τίποτα).
    map.emplace(2,"TWO");
    auto it5 = map.find(2);
    REQUIRE(it5 != map.end());
    REQUIRE(it5->second == "two"); // Η τιμή δεν πρέπει να αλλάξει.
}

TEST_CASE("CuckooHashMap handles collisions", "[cuckoo_hash_map]") {
    Contest::CuckooHashMap<int32_t, std::string, Contest::IntHasher> map;

    // Κλειδιά που συγκρούονται.
    int32_t key1 = 1;
    int32_t key2 = 12; 

    map.emplace(key1, "value1");
    map.emplace(key2, "value2");

    // Εύρεση και των δύο στοιχείων.
    auto it1 = map.find(key1);
    REQUIRE(it1 != map.end());
    REQUIRE(it1->second == "value1");

    auto it2 = map.find(key2);
    REQUIRE(it2 != map.end());
    REQUIRE(it2->second == "value2");
}

TEST_CASE("CuckooHashMap rehashing works correctly", "[cuckoo_hash_map]") {
    Contest::CuckooHashMap<int, std::string, Contest::IntHasher> map;

    const int num_elements = 200; // Αριθμός στοιχείων για εισαγωγή

    // Εισαγωγή πολλών στοιχείων για να αναγκάσουμε rehashing
    for (int i = 0; i < num_elements; ++i) {
        map.emplace(i, "value" + std::to_string(i));
    }

    // Έλεγχος ότι όλα τα στοιχεία είναι προσβάσιμα μετά το rehashing
    for (int i = 0; i < num_elements; ++i) {
        auto it = map.find(i);
        REQUIRE(it != map.end());
        REQUIRE(it->second == "value" + std::to_string(i));
    }
}