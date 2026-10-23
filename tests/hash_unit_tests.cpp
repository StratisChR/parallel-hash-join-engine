#include <catch2/catch_test_macros.hpp>

#include "baselines/int_hasher.h"
#include "baselines/robin_hood_hash_map.h"

#include <set>

TEST_CASE("IntHasher basic functionality", "[int_hasher]") {
    Contest::IntHasher hasher;

    SECTION("Consistency and uniqueness.") {
        // Ίδια είσοδος πρέπει να δίνει ίδια έξοδο.
        REQUIRE(hasher(123456) == hasher(123456));
        REQUIRE(hasher(42) == hasher(42));
        REQUIRE(hasher(0u) == 0ULL);

        // Διαφορετική είσοδος πρέπει να δίνει διαφορετική έξοδο.
        REQUIRE(hasher(42) != hasher(43));
        REQUIRE(hasher(1) != hasher(2));
        REQUIRE(hasher(128) != hasher(256));
    }

    SECTION("Distribution test.") {
        // Ελέγχουμε 1000 διαδοχικούς αριθμούς για να δούμε αν παράγουν
        // 1000 μοναδικές τιμές hash.
        std::set<size_t> unique_hashes;
        for(uint64_t i = 1; i <= 1000; ++i) { // Start from 1 to avoid 0
            unique_hashes.insert(hasher(i));
        }
        
        // Όλες οι 1000 τιμές πρέπει να είναι μοναδικές.
        CHECK(unique_hashes.size() == 1000);
    }
}


TEST_CASE("Emplace and Find in RobinHoodHashMap", "[robin_hood_hash_map]") {
    Contest::RobinHoodHashMap<int32_t, std::string, Contest::IntHasher> map;

    // Εισαγωγή στοιχείων.
    map.emplace(1, "one");
    map.emplace(2, "two");
    map.emplace(3, "three");

    // Εύρεση υπαρχόντων στοιχείων
    auto it1 = map.find(1);
    REQUIRE(it1 != map.end());
    REQUIRE(it1->second == "one");

    auto it2 = map.find(2);
    REQUIRE(it2 != map.end());
    REQUIRE(it2->second == "two");

    auto it3 = map.find(3);
    REQUIRE(it3 != map.end());
    REQUIRE(it3->second == "three");

    // Εύρεση μη υπαρχόντων στοιχείων
    auto it4 = map.find(4);
    REQUIRE(it4 == map.end());

    // Εισαγωγή διπλότυπου κλειδιού (δεν πρέπει να αλλάξει τίποτα).
    map.emplace(2,"TWO");
    auto it5 = map.find(2);
    REQUIRE(it5 != map.end());
    REQUIRE(it5->second == "two"); // Η τιμή δεν πρέπει να αλλάξει.
}

TEST_CASE("RobinHoodHashMap handles collisions", "[robin_hood_hash_map]") {
    Contest::RobinHoodHashMap<int32_t, std::string, Contest::IntHasher> map;

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

TEST_CASE("RobinHoodHashMap rehashing", "[robin_hood_hash_map]") {
    Contest::RobinHoodHashMap<int32_t, std::string, Contest::IntHasher> map; 

    // Εισαγωγή αρκετών στοιχείων για να προκαλέσει rehash.
    for (int32_t i = 0; i < 200; ++i) {
        map.emplace(i, "value" + std::to_string(i));
    }

    // Επαλήθευση ότι όλα τα στοιχεία είναι προσβάσιμα μετά το rehash.
    for (int32_t i = 0; i < 200; ++i) {
        auto it = map.find(i);
        REQUIRE(it != map.end());
        REQUIRE(it->second == "value" + std::to_string(i));
    }
}

TEST_CASE("RobinHoodHashMap with std::hash, emplace", "[robin_hood_hash_map]") {
    Contest::RobinHoodHashMap<std::string, int32_t, std::hash<std::string>> map;

    // Εισαγωγή στοιχείων.
    map.emplace("one", 1);
    map.emplace("two", 2);
    map.emplace("three", 3);

    // Εύρεση υπαρχόντων στοιχείων
    auto it1 = map.find("one");
    REQUIRE(it1 != map.end());
    REQUIRE(it1->second == 1);

    auto it2 = map.find("two");
    REQUIRE(it2 != map.end());
    REQUIRE(it2->second == 2);

    auto it3 = map.find("three");
    REQUIRE(it3 != map.end());
    REQUIRE(it3->second == 3);

    // Εύρεση μη υπαρχόντων στοιχείων
    auto it4 = map.find("four");
    REQUIRE(it4 == map.end());
}
