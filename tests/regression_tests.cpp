// Regression tests for the join engine.
//
// Part 1 tests individual components (hash map, Bloom tags, thread pool, pages,
// lazy string encoding). Part 2 runs whole joins through Contest::execute and
// compares the result with an answer computed independently in the test.
//
// Each test names the bug or change it protects. They need no dataset and run in
// a few seconds, so they can run on every push (GitHub Actions).

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <plan.h>
#include <table.h>

#include "hashtable/bloom.h"
#include "engine/column.h"
#include "engine/thread_pool.h"
#include "hashtable/unchained_map.h"

using namespace std::string_literals;

// ============================================================================
// Part 1: components
// ============================================================================

// Bug fixed: the old table was built with (i * 7) & 0xF etc., which depends only on
// i mod 16 -> just 15 distinct tags, none with 4 bits set (7% false positives for
// one key per slot instead of the paper's ~1/1820).
TEST_CASE("Bloom tags: 1820 distinct tags, each with exactly 4 bits set", "[bloom]") {
    precompute_bloom_tags();
    std::set<uint16_t> distinct(bloom_tags.begin(), bloom_tags.end());
    REQUIRE(distinct.size() == 1820);
    for (uint16_t tag : bloom_tags) REQUIRE(__builtin_popcount(tag) == 4);
}

// Build side as a column of value_t (nulls allowed), the way joins see it.
static std::vector<column_t> make_key_column(const std::vector<std::optional<int32_t>>& keys) {
    std::vector<column_t> table(1);
    for (auto& k : keys) table[0].push_back(k ? value_t(*k) : value_t());
    return table;
}

// The live hash map against std::multimap: every key returns exactly the right rows,
// both through lookup() and lookup_batch(), for empty, tiny, and large build sides,
// with heavy duplicates and NULL keys (which must never match).
TEST_CASE("UnchainedHashMap returns exactly the matching rows", "[hashmap]") {
    std::mt19937 rng(42);
    for (size_t n : {0ul, 1ul, 7ul, 1000ul, 70000ul, 300000ul}) {
        for (int key_range : {5, 5000, 1 << 30}) {
            std::vector<std::optional<int32_t>> keys(n);
            std::multimap<int32_t, size_t> expected;
            for (size_t i = 0; i < n; ++i) {
                if (rng() % 17 == 0) continue;                         // NULL
                keys[i] = int32_t(rng() % key_range) - key_range / 2;
                expected.insert({*keys[i], i});
            }
            auto table = make_key_column(keys);
            UnchainedHashMap<int32_t, size_t> map;
            map.build(table, 0);

            // Few distinct keys means every probe returns thousands of rows; fewer probes suffice.
            std::vector<int32_t> probes(key_range == 5 ? 40 : 2000);
            for (size_t q = 0; q < probes.size(); ++q)
                probes[q] = (q % 2 && n && keys[rng() % n]) ? *keys[rng() % n]
                                                             : int32_t(rng() % key_range) - key_range / 2;

            std::vector<std::vector<size_t>> by_batch(probes.size());
            for (size_t b = 0; b < probes.size(); b += 256) {
                size_t len = std::min<size_t>(256, probes.size() - b);
                map.lookup_batch(probes.data() + b, len,
                                 [&](size_t i, size_t row) { by_batch[b + i].push_back(row); });
            }
            for (size_t q = 0; q < probes.size(); ++q) {
                std::vector<size_t> want, single;
                auto [lo, hi] = expected.equal_range(probes[q]);
                for (auto it = lo; it != hi; ++it) want.push_back(it->second);
                map.lookup(probes[q], [&](size_t row) { single.push_back(row); });
                std::sort(want.begin(), want.end());
                std::sort(single.begin(), single.end());
                std::sort(by_batch[q].begin(), by_batch[q].end());
                REQUIRE(single == want);
                REQUIRE(by_batch[q] == want);
            }
        }
    }
}

// The pool replaced spawning threads per phase. Every task id must run exactly once
// per round, and its writes must be visible after run() returns.
TEST_CASE("ThreadPool runs every task exactly once per round", "[pool]") {
    REQUIRE(pool().size() >= 1);
    for (int round = 0; round < 2000; ++round) {
        std::vector<int> hits(pool().size(), 0);
        pool().run([&](size_t t) { hits[t]++; });
        for (int h : hits) REQUIRE(h == 1);
    }
}

// Result pages are recycled through PagePool. A reused page still holds old values,
// so this checks that columns built from recycled pages read back exactly what was
// written, also when several threads fill and drop columns at the same time.
TEST_CASE("Recycled result pages hold exactly the new values", "[pages]") {
    std::atomic<long> bad{0};
    pool().run([&](size_t t) {
        for (int round = 0; round < 20; ++round) {
            column_t col;
            size_t n = 1000 + (round * 7919 + t * 104729) % 30000;
            for (size_t i = 0; i < n; ++i) col.push_back(value_t(int32_t(i * 31 + t)));
            if (col.size() != n) bad++;
            for (size_t i = 0; i < n; ++i)
                if (col[i].get_int() != int32_t(i * 31 + t)) bad++;
        }
    });
    REQUIRE(bad == 0);
}

// Lazy strings pack (table, column, row) into a value_t. The fields must not overlap
// each other or the NULL/TYPE bits (StringRef's table field overlapped TYPE_MASK).
TEST_CASE("Lazy string values round-trip table, column and row", "[lazy-strings]") {
    const uint64_t max_row = (1ULL << 40) - 1;
    for (uint8_t table : {uint8_t(0), uint8_t(1), uint8_t(127), uint8_t(255)})
        for (uint8_t column : {uint8_t(0), uint8_t(3), uint8_t(255)})
            for (uint64_t row : {uint64_t(0), uint64_t(1), uint64_t(65536), uint64_t(123456789), max_row}) {
                value_t v = make_lazy_string(table, column, row);
                REQUIRE(v.is_string());
                REQUIRE_FALSE(v.is_null());
                REQUIRE(lazy_string_table(v) == table);
                REQUIRE(lazy_string_column(v) == column);
                REQUIRE(lazy_string_row(v) == row);
            }
}

// ============================================================================
// Part 2: whole joins through Contest::execute
// ============================================================================

using Rows = std::vector<std::vector<Data>>;

static bool is_null(const Data& d) { return std::holds_alternative<std::monostate>(d); }

// Runs a plan and returns its result rows, sorted.
static Rows run_plan(Plan& plan) {
    auto* context = Contest::build_context();
    ColumnarTable result = Contest::execute(plan, context);
    Contest::destroy_context(context);
    Rows rows = Table::from_columnar(result).table();
    std::sort(rows.begin(), rows.end());
    return rows;
}

// Reference answer for left JOIN right ON left[lk] = right[rk], output columns given
// as indexes into (left columns ++ right columns). NULL keys never match.
static Rows expected_join(const Rows& left, const Rows& right, size_t lk, size_t rk,
                          const std::vector<size_t>& out) {
    std::unordered_multimap<int32_t, size_t> index;
    for (size_t i = 0; i < right.size(); ++i)
        if (!is_null(right[i][rk])) index.emplace(std::get<int32_t>(right[i][rk]), i);
    const size_t ncols_left = left.empty() ? 0 : left[0].size();
    Rows result;
    for (auto& l : left) {
        if (is_null(l[lk])) continue;
        auto [lo, hi] = index.equal_range(std::get<int32_t>(l[lk]));
        for (auto it = lo; it != hi; ++it) {
            std::vector<Data> row;
            for (size_t c : out) row.push_back(c < ncols_left ? l[c] : right[it->second][c - ncols_left]);
            result.push_back(std::move(row));
        }
    }
    std::sort(result.begin(), result.end());
    return result;
}

static std::vector<std::tuple<size_t, DataType>> all_columns(const std::vector<DataType>& types) {
    std::vector<std::tuple<size_t, DataType>> attrs;
    for (size_t i = 0; i < types.size(); ++i) attrs.emplace_back(i, types[i]);
    return attrs;
}

static std::vector<std::tuple<size_t, DataType>> pick(const std::vector<size_t>& cols,
                                                      const std::vector<DataType>& types) {
    std::vector<std::tuple<size_t, DataType>> attrs;
    for (size_t c : cols) attrs.emplace_back(c, types[c]);
    return attrs;
}

static std::string random_string(std::mt19937& rng, size_t max_len) {
    std::string s(rng() % (max_len + 1), ' ');
    for (char& ch : s) ch = char('a' + rng() % 26);
    return s;
}

// Bug fixed: long strings (> one page) on adjacent pages were glued together,
// because the reader kept appending any 0xffff/0xfffe page, including the start
// of the next long string. Also covers a long string as the very last value.
TEST_CASE("Adjacent long strings come back separately", "[strings][long]") {
    std::string a(10000, 'A'), b(9000, 'B'), c(20000, 'C'), d(8200, 'D');
    Rows left{
        {1, a},
        {2, b},                     // starts on the page right after a's last page
        {3, "short"s},
        {4, c},
        {5, std::monostate{}},
        {6, ""s},
        {7, d},                     // last value of the column
    };
    std::vector<DataType> left_types{DataType::INT32, DataType::VARCHAR};
    Rows right{{1}, {2}, {3}, {4}, {5}, {6}, {7}, {7}};
    std::vector<DataType> right_types{DataType::INT32};

    Plan plan;
    plan.new_scan_node(0, all_columns(left_types));
    plan.new_scan_node(1, all_columns(right_types));
    plan.new_join_node(true, 0, 1, 0, 0, {{0, DataType::INT32}, {1, DataType::VARCHAR}});
    plan.inputs.push_back(Table(left, left_types).to_columnar());
    plan.inputs.push_back(Table(right, right_types).to_columnar());
    plan.root = 2;

    REQUIRE(run_plan(plan) == expected_join(left, right, 0, 0, {0, 1}));
}

// Lazy strings: values are resolved from (table, column, row) at output time, which
// needs the right page (block lookup + page row counts) and the right position on it
// (count of non-NULL rows before it). Many pages, NULLs, empty strings, a few long ones,
// strings from both join sides.
TEST_CASE("Strings from both sides across many pages, with NULLs", "[strings][lazy-strings]") {
    std::mt19937 rng(7);
    std::vector<DataType> types{DataType::INT32, DataType::VARCHAR, DataType::INT32};
    auto make = [&](size_t n, int key_range) {
        Rows rows;
        for (size_t i = 0; i < n; ++i) {
            Data key = (rng() % 20 == 0) ? Data{std::monostate{}} : Data{int32_t(rng() % key_range)};
            Data str;
            switch (rng() % 50) {
                case 0:  str = std::monostate{}; break;
                case 1:  str = ""s; break;
                case 2:  str = std::string(9000 + rng() % 4000, char('a' + rng() % 26)); break;
                default: str = random_string(rng, 30);
            }
            rows.push_back({key, str, int32_t(i)});
        }
        return rows;
    };
    Rows left = make(30000, 20000), right = make(20000, 20000);

    Plan plan;
    plan.new_scan_node(0, all_columns(types));
    plan.new_scan_node(1, all_columns(types));
    std::vector<size_t> out{0, 1, 2, 4, 5};      // left key, left str, left id, right str, right id
    plan.new_join_node(true, 0, 1, 0, 0, pick(out, {DataType::INT32, DataType::VARCHAR, DataType::INT32,
                                                    DataType::INT32, DataType::VARCHAR, DataType::INT32}));
    plan.inputs.push_back(Table(left, types).to_columnar());
    plan.inputs.push_back(Table(right, types).to_columnar());
    plan.root = 2;

    REQUIRE(run_plan(plan) == expected_join(left, right, 0, 0, out));
}

// A three-table join: the first join's result is an intermediate result (per-thread
// pages, merged by moving full pages), large enough that every thread produces several
// full pages. Strings from the first table travel through the intermediate join as
// lazy values and are resolved only at the end. Also exercises the probe on both a
// zero-copy INT32 column (no NULLs) and a materialized one (with NULLs).
TEST_CASE("Three-way join with a large intermediate result", "[join][merge][lazy-strings]") {
    std::mt19937 rng(11);
    // A(a_id, a_name)   B(b_id, a_ref)   C(c_ref, c_val)
    Rows A, B, C;
    for (int32_t i = 0; i < 20000; ++i) A.push_back({i, random_string(rng, 20)});
    for (int32_t i = 0; i < 400000; ++i)
        B.push_back({i, (rng() % 25 == 0) ? Data{std::monostate{}} : Data{int32_t(rng() % 25000)}});
    for (int32_t i = 0; i < 60000; ++i) C.push_back({int32_t(rng() % 500000), int32_t(rng())});
    std::vector<DataType> tA{DataType::INT32, DataType::VARCHAR}, tB{DataType::INT32, DataType::INT32},
                          tC{DataType::INT32, DataType::INT32};

    Plan plan;
    size_t sA = plan.new_scan_node(0, all_columns(tA));
    size_t sB = plan.new_scan_node(1, all_columns(tB));
    size_t sC = plan.new_scan_node(2, all_columns(tC));
    // J1 = A JOIN B ON a_id = a_ref  -> (a_name, b_id)
    size_t j1 = plan.new_join_node(true, sA, sB, 0, 1, {{1, DataType::VARCHAR}, {2, DataType::INT32}});
    // J2 = J1 JOIN C ON b_id = c_ref -> (a_name, b_id, c_val)
    plan.new_join_node(false, j1, sC, 1, 0, {{0, DataType::VARCHAR}, {1, DataType::INT32}, {3, DataType::INT32}});
    plan.inputs.push_back(Table(A, tA).to_columnar());
    plan.inputs.push_back(Table(B, tB).to_columnar());
    plan.inputs.push_back(Table(C, tC).to_columnar());
    plan.root = plan.nodes.size() - 1;

    Rows j1_rows = expected_join(A, B, 0, 1, {1, 2});
    Rows expected = expected_join(j1_rows, C, 1, 0, {0, 1, 3});
    Rows got = run_plan(plan);
    REQUIRE(got.size() == expected.size());
    REQUIRE(got == expected);
}

// The same base table scanned twice in one plan (a self-join): both scans share one
// string-column index entry. Strings are output from both sides.
TEST_CASE("Self-join with strings from both sides", "[strings][lazy-strings]") {
    std::mt19937 rng(3);
    Rows T;
    for (int32_t i = 0; i < 5000; ++i)
        T.push_back({int32_t(rng() % 1500), (i % 17 == 0) ? Data{std::monostate{}} : Data{random_string(rng, 12)}});
    std::vector<DataType> types{DataType::INT32, DataType::VARCHAR};

    Plan plan;
    plan.new_scan_node(0, all_columns(types));
    plan.new_scan_node(0, all_columns(types));
    plan.new_join_node(true, 0, 1, 0, 0, {{1, DataType::VARCHAR}, {3, DataType::VARCHAR}});
    plan.inputs.push_back(Table(T, types).to_columnar());
    plan.root = 2;

    REQUIRE(run_plan(plan) == expected_join(T, T, 0, 0, {1, 3}));
}

// The string-column index is per query and must be reset at the start of every
// execute(). The danger is a new column at the *same address* as one from an earlier
// query (the index is found by address): without the reset, the old page layout would
// be reused for the new pages. To force that situation, this test keeps one Plan and
// swaps new pages into its existing Column objects for every query, while the number
// of rows and the string lengths (and so the page layout) change each time.
TEST_CASE("Consecutive queries do not reuse string indexes", "[strings][lazy-strings]") {
    std::mt19937 rng(5);
    std::vector<DataType> types{DataType::INT32, DataType::VARCHAR};
    Plan plan;
    plan.new_scan_node(0, all_columns(types));
    plan.new_scan_node(1, all_columns(types));
    plan.new_join_node(true, 0, 1, 0, 0, {{1, DataType::VARCHAR}, {3, DataType::VARCHAR}});
    plan.root = 2;

    for (int query = 0; query < 30; ++query) {
        const int32_t n       = 500 + 400 * (query % 7);
        const size_t  max_len = 4 + (query * 13) % 60;
        Rows left, right;
        for (int32_t i = 0; i < n; ++i)
            left.push_back({i, "q"s + std::to_string(query) + "-" + random_string(rng, max_len)});
        for (int32_t i = 0; i < n; ++i)
            right.push_back({int32_t(rng() % n), random_string(rng, max_len)});

        ColumnarTable fresh[2] = {Table(left, types).to_columnar(), Table(right, types).to_columnar()};
        if (query == 0) {
            plan.inputs.push_back(std::move(fresh[0]));
            plan.inputs.push_back(std::move(fresh[1]));
        } else {
            // Same Column objects (same addresses), new pages. The old pages move into
            // `fresh` and are freed with it.
            for (int t = 0; t < 2; ++t) {
                std::swap(plan.inputs[t].num_rows, fresh[t].num_rows);
                for (size_t c = 0; c < types.size(); ++c)
                    std::swap(plan.inputs[t].columns[c].pages, fresh[t].columns[c].pages);
            }
        }
        REQUIRE(run_plan(plan) == expected_join(left, right, 0, 0, {1, 3}));
    }
}

// The engine builds on the actually smaller input, regardless of the plan's build_left.
// Both settings must give the same result, with a much larger left or right side.
TEST_CASE("Result does not depend on the plan's build side", "[join]") {
    std::mt19937 rng(9);
    std::vector<DataType> types{DataType::INT32, DataType::INT32};
    Rows small, big;
    for (int32_t i = 0; i < 300; ++i) small.push_back({int32_t(rng() % 1000), i});
    for (int32_t i = 0; i < 100000; ++i) big.push_back({int32_t(rng() % 1000), i});
    for (bool build_left : {true, false}) {
        for (bool small_on_left : {true, false}) {
            const Rows& L = small_on_left ? small : big;
            const Rows& R = small_on_left ? big : small;
            Plan plan;
            plan.new_scan_node(0, all_columns(types));
            plan.new_scan_node(1, all_columns(types));
            plan.new_join_node(build_left, 0, 1, 0, 0, {{1, DataType::INT32}, {3, DataType::INT32}});
            plan.inputs.push_back(Table(L, types).to_columnar());
            plan.inputs.push_back(Table(R, types).to_columnar());
            plan.root = 2;
            REQUIRE(run_plan(plan) == expected_join(L, R, 0, 0, {1, 3}));
        }
    }
}// Random bushy plans over 2-7 tables, with duplicate keys, NULL keys and strings. The
// engine may run the joins in any order it likes; the result must match the plan's.
TEST_CASE("Random multi-way plans give the plan's result in any join order", "[join][order]") {
    std::mt19937 rng(2024);
    for (int round = 0; round < 40; ++round) {
        const size_t n_tables = 2 + rng() % 6;
        std::vector<Rows> tables(n_tables);
        const std::vector<DataType> types{DataType::INT32, DataType::INT32, DataType::VARCHAR};
        for (auto& t : tables) {
            size_t rows = 20 + rng() % 400;
            int32_t domain = 20 + int32_t(rng() % 200);
            for (size_t i = 0; i < rows; ++i)
                t.push_back({(rng() % 10 == 0) ? Data{std::monostate{}} : Data{int32_t(rng() % domain)},
                             (rng() % 10 == 0) ? Data{std::monostate{}} : Data{int32_t(rng() % domain)},
                             (rng() % 7 == 0)  ? Data{std::monostate{}} : Data{random_string(rng, 8)}});
        }

        // Every node keeps all columns; parts[i] = (plan node, expected rows).
        Plan plan;
        std::vector<std::pair<size_t, Rows>> parts;
        for (size_t t = 0; t < n_tables; ++t) {
            parts.push_back({plan.new_scan_node(t, all_columns(types)), tables[t]});
            plan.inputs.push_back(Table(tables[t], types).to_columnar());
        }
        bool too_big = false;
        while (parts.size() > 1) {
            size_t i = rng() % parts.size();
            size_t j = rng() % (parts.size() - 1);
            if (j >= i) ++j;
            auto L = std::move(parts[i]), R = std::move(parts[j]);
            parts.erase(parts.begin() + std::max(i, j));
            parts.erase(parts.begin() + std::min(i, j));

            auto& l_attrs = plan.nodes[L.first].output_attrs;
            auto& r_attrs = plan.nodes[R.first].output_attrs;
            // Join keys: any INT32 column on each side.
            std::vector<size_t> l_int, r_int;
            for (size_t c = 0; c < l_attrs.size(); ++c) if (std::get<1>(l_attrs[c]) == DataType::INT32) l_int.push_back(c);
            for (size_t c = 0; c < r_attrs.size(); ++c) if (std::get<1>(r_attrs[c]) == DataType::INT32) r_int.push_back(c);
            size_t lk = l_int[rng() % l_int.size()], rk = r_int[rng() % r_int.size()];

            const bool last = parts.empty();
            std::vector<size_t> out;
            for (size_t c = 0; c < l_attrs.size() + r_attrs.size(); ++c)
                if (!last || rng() % 2 == 0) out.push_back(c);
            if (out.empty()) out.push_back(0);

            std::vector<std::tuple<size_t, DataType>> attrs;
            for (size_t c : out)
                attrs.emplace_back(c, c < l_attrs.size() ? std::get<1>(l_attrs[c])
                                                         : std::get<1>(r_attrs[c - l_attrs.size()]));
            size_t node = plan.new_join_node(rng() % 2, L.first, R.first, lk, rk, attrs);
            Rows rows = expected_join(L.second, R.second, lk, rk, out);
            if (rows.size() > 300000) { too_big = true; break; }
            parts.push_back({node, std::move(rows)});
        }
        if (too_big) continue;
        plan.root = parts[0].first;

        Rows got = run_plan(plan);
        INFO("round " << round << ", " << n_tables << " tables");
        REQUIRE(got.size() == parts[0].second.size());
        REQUIRE(got == parts[0].second);
    }
}
