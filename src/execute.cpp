#include <hardware.h>
#include <plan.h>
#include <table.h>
#include <stdexcept>
#include <variant>
#include <atomic>
#include <cassert>
#include <algorithm>
#include <cstring>
#include <functional>
#include <chrono>
#include <cstdio>

#include "hashtable/unchained_map.h"
#include "engine/thread_pool.h"
#include "engine/column.h"

namespace Contest {

// #define PHASE_TIMERS   // uncomment to print a per-phase time breakdown at the end of a run

#ifdef PHASE_TIMERS
struct PhaseTimers {
    std::atomic<uint64_t> scan{0}, build{0}, probe{0}, merge{0},
                          root_build{0}, root_probe{0}, root_finalize{0},
                          scan_int{0}, scan_str{0}, rows_int{0}, rows_str{0},
                          root_rows{0}, root_str_values{0};
    // Intermediate joins grouped by build-side rows: <1k, 1k-100k, >=100k.
    std::atomic<uint64_t> joins_by_size[3]{}, us_by_size[3]{};
};
inline PhaseTimers g_timers;

struct ScopedTimer {
    std::atomic<uint64_t>& acc;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~ScopedTimer() {
        acc += std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now() - start).count();
    }
};
#define PHASE_TIMER(name) ScopedTimer phase_timer_##name{g_timers.name}
#else
#define PHASE_TIMER(name)
#endif

using ExecuteResult = std::vector<column_t>;

// ---------------------------------------------------------------------------------
// Materialization statistics (measurement only, off by default).
// For every intermediate join it counts the values written into the result, split into
//   - key values:     columns that some join further up uses as its join key,
//   - carried values: columns no join uses, copied along only to reach the root output.
// It also estimates what a row-id design would write instead: the key columns plus one
// row id per base table that still has carried columns.
// #define MATERIALIZATION_STATS
#ifdef MATERIALIZATION_STATS
struct MatStats {
    uint64_t joins = 0, rows = 0, values = 0, key_values = 0, rowid_values = 0;
    ~MatStats() {
        if (joins == 0) return;
        auto M = [](uint64_t v) { return v / 1e6; };
        uint64_t carried = values - key_values;
        uint64_t alt     = key_values + rowid_values;
        std::fprintf(stderr,
            "intermediate joins: %llu, result rows: %.1f M\n"
            "values written now:      %8.1f M\n"
            "  join keys:             %8.1f M (%.0f%%)\n"
            "  carried to root only:  %8.1f M (%.0f%%)\n"
            "with row ids instead:    %8.1f M (keys %.1f M + row ids %.1f M) = %.0f%% of now\n",
            (unsigned long long)joins, M(rows), M(values),
            M(key_values), 100.0 * key_values / values,
            M(carried),    100.0 * carried / values,
            M(alt), M(key_values), M(rowid_values), 100.0 * alt / values);
    }
};
inline MatStats g_mat;
static std::vector<std::vector<bool>>   g_mat_key_cols;  // [node][output col]: used as a key above
static std::vector<std::vector<size_t>> g_mat_origin;    // [node][output col]: scan node it comes from

// Walks the plan once per query, before execution.
static void mat_analyze(const Plan& plan) {
    const size_t N = plan.nodes.size();
    g_mat_key_cols.assign(N, {});
    g_mat_origin.assign(N, {});

    // Bottom-up: which scan does each output column come from?
    std::function<void(size_t)> origin = [&](size_t n) {
        auto& node = plan.nodes[n];
        auto& o = g_mat_origin[n];
        o.resize(node.output_attrs.size());
        if (auto* j = std::get_if<JoinNode>(&node.data)) {
            origin(j->left);
            origin(j->right);
            size_t nl = plan.nodes[j->left].output_attrs.size();
            for (size_t c = 0; c < o.size(); ++c) {
                size_t s = std::get<0>(node.output_attrs[c]);
                o[c] = s < nl ? g_mat_origin[j->left][s] : g_mat_origin[j->right][s - nl];
            }
        } else {
            for (auto& x : o) x = n;
        }
    };

    // Top-down: which output columns of each node does some join above use as its key?
    std::function<void(size_t)> keys = [&](size_t n) {
        auto& node = plan.nodes[n];
        auto* j = std::get_if<JoinNode>(&node.data);
        if (!j) return;
        size_t nl = plan.nodes[j->left].output_attrs.size();
        auto& kl = g_mat_key_cols[j->left];
        auto& kr = g_mat_key_cols[j->right];
        kl.assign(nl, false);
        kr.assign(plan.nodes[j->right].output_attrs.size(), false);
        kl[j->left_attr]  = true;
        kr[j->right_attr] = true;
        for (size_t c = 0; c < node.output_attrs.size(); ++c) {
            if (!g_mat_key_cols[n][c]) continue;
            size_t s = std::get<0>(node.output_attrs[c]);
            if (s < nl) kl[s] = true; else kr[s - nl] = true;
        }
        keys(j->left);
        keys(j->right);
    };

    origin(plan.root);
    g_mat_key_cols[plan.root].assign(plan.nodes[plan.root].output_attrs.size(), false);
    keys(plan.root);
}

// Called after each intermediate join with its result.
static void mat_record(size_t n, const std::vector<column_t>& result) {
    const size_t cols = result.size();
    const size_t rows = cols ? result[0].size() : 0;
    size_t key_cols = 0;
    std::vector<size_t> carried_scans;
    for (size_t c = 0; c < cols; ++c) {
        if (g_mat_key_cols[n][c]) ++key_cols;
        else carried_scans.push_back(g_mat_origin[n][c]);
    }
    std::sort(carried_scans.begin(), carried_scans.end());
    size_t scans = std::unique(carried_scans.begin(), carried_scans.end()) - carried_scans.begin();

    g_mat.joins        += 1;
    g_mat.rows         += rows;
    g_mat.values       += uint64_t(rows) * cols;
    g_mat.key_values   += uint64_t(rows) * key_cols;
    g_mat.rowid_values += uint64_t(rows) * scans;
}
#define MAT_ANALYZE(plan)          mat_analyze(plan)
#define MAT_RECORD(node, result)   mat_record(node, result)
#else
#define MAT_ANALYZE(plan)
#define MAT_RECORD(node, result)
#endif
// ---------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------
// Join statistics (measurement only, off by default).
// Records every join's input and output sizes, in execution order. Prints one line per
// query, and at exit the joins of the queries with the most intermediate rows.
// #define JOIN_STATS
#ifdef JOIN_STATS
struct JoinRecord { size_t left, right, out; bool root; };
struct QueryRecord {
    size_t index;
    std::vector<JoinRecord> joins;
    uint64_t intermediate() const {
        uint64_t s = 0;
        for (auto& j : joins) if (!j.root) s += j.out;
        return s;
    }
    size_t result() const { return joins.empty() ? 0 : joins.back().out; }
};
struct JoinStats {
    std::vector<QueryRecord> queries;
    std::vector<JoinRecord>  current;
    ~JoinStats() {
        if (queries.empty()) return;
        uint64_t inter = 0, res = 0;
        for (auto& q : queries) { inter += q.intermediate(); res += q.result(); }
        std::fprintf(stderr, "\nall queries: intermediate %.1f M rows, results %.1f M rows\n",
                     inter / 1e6, res / 1e6);

        std::vector<const QueryRecord*> top;
        for (auto& q : queries) top.push_back(&q);
        std::sort(top.begin(), top.end(), [](auto* a, auto* b) { return a->intermediate() > b->intermediate(); });
        if (top.size() > 12) top.resize(12);

        std::fprintf(stderr, "\nqueries with the most intermediate rows (joins in execution order):\n");
        for (auto* q : top) {
            std::fprintf(stderr, "query #%zu: intermediate %.2f M, result %.2f M\n",
                         q->index, q->intermediate() / 1e6, q->result() / 1e6);
            for (auto& j : q->joins)
                std::fprintf(stderr, "    %10zu x %10zu -> %10zu%s\n",
                             j.left, j.right, j.out, j.root ? "   (root)" : "");
        }
    }
};
inline JoinStats g_join_stats;

static void join_record(size_t left, size_t right, size_t out, bool root) {
    g_join_stats.current.push_back({left, right, out, root});
}
static void join_query_done() {
    QueryRecord q{g_join_stats.queries.size() + 1, std::move(g_join_stats.current)};
    g_join_stats.current.clear();
    std::fprintf(stderr, "query #%zu: %zu joins, intermediate %.2f M rows, result %.2f M rows\n",
                 q.index, q.joins.size(), q.intermediate() / 1e6, q.result() / 1e6);
    g_join_stats.queries.push_back(std::move(q));
}
#define JOIN_RECORD(l, r, out, root) join_record(l, r, out, root)
#define JOIN_QUERY_DONE()            join_query_done()
#else
#define JOIN_RECORD(l, r, out, root)
#define JOIN_QUERY_DONE()
#endif
// ---------------------------------------------------------------------------------

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
static std::vector<StringColumnIndex> g_string_columns(256 * 256);   // indexed by table * 256 + column
static std::vector<size_t>            g_string_columns_used;         // slots filled in this query

static inline StringColumnIndex& string_column_index(uint8_t table, uint8_t column) {
    return g_string_columns[size_t(table) * 256 + column];
}

// Called at the start of every query. Input tables differ between queries, and a new
// column can live at the same address as an old one, so old entries must not survive.
static void reset_string_columns() {
    for (size_t slot : g_string_columns_used) g_string_columns[slot] = StringColumnIndex{};
    g_string_columns_used.clear();
}

// Scan of a VARCHAR column: reads only the page headers. No row is converted.
static void scan_lazy_string_column(const Column& column, size_t table_id, size_t in_col_idx, column_t& target_col) {
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

// Number of set bits among the first `count` bits of a bitmap.
static inline size_t count_set_bits(const uint8_t* bitmap, size_t count) {
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
static bool resolve_string(value_t v, std::string& scratch, std::string_view& out) {
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

ExecuteResult execute_impl(const Plan& plan, size_t node_idx);

// Wrapper for writing to columnar tables during Root Join materialization
struct ColumnWriter {
    std::unique_ptr<Column> col;
    std::unique_ptr<ColumnInserter<int32_t>> int_inserter;
    std::unique_ptr<ColumnInserter<std::string>> str_inserter;
    DataType type;
    std::string scratch;   // reused buffer for long strings (one per writer, so one per thread)

    void (ColumnWriter::*add_val_impl)(const value_t&, const Plan&) = nullptr;
    void (ColumnWriter::*finalize_impl)() = nullptr;

    ColumnWriter() = default;
    ColumnWriter(ColumnWriter&&) = default;
    ColumnWriter& operator=(ColumnWriter&&) = default;

    void init(DataType t) {
        type = t;
        col = std::make_unique<Column>(t);
        if (t == DataType::INT32) {
            int_inserter = std::make_unique<ColumnInserter<int32_t>>(*col);
            add_val_impl = &ColumnWriter::add_val_int;
            finalize_impl = &ColumnWriter::finalize_int;
        } else {
            str_inserter = std::make_unique<ColumnInserter<std::string>>(*col);
            add_val_impl = &ColumnWriter::add_val_str;
            finalize_impl = &ColumnWriter::finalize_str;
        }
    }

    void add_val(const value_t& v, const Plan& plan) {
        (this->*add_val_impl)(v, plan);
    }

    void finalize() {
        if (col) {
            (this->*finalize_impl)();
        }
    }

private:
    void add_val_int(const value_t& v, const Plan& plan) {
        if (v.is_null()) int_inserter->insert_null();
        else int_inserter->insert(v.get_int());
    }

    void add_val_str(const value_t& v, const Plan& plan) {
        std::string_view sv;
        if (v.is_null() || !resolve_string(v, scratch, sv)) str_inserter->insert_null();
        else str_inserter->insert(sv);
    }

    void finalize_int() { int_inserter->finalize(); }
    void finalize_str() { str_inserter->finalize(); }
};

// Combines the per-thread results of a join into one result. Row order inside an
// intermediate result does not matter (a join's output is the same for any input
// order), so instead of copying every row, the full pages of every thread are moved
// over (just pointers), and only each thread's partially filled last page is copied.
// All columns of one thread have the same length, so every output column gets the
// same row arrangement.
static void merge_thread_results(std::vector<column_t>& results,
                                 std::vector<ExecuteResult>& thread_results,
                                 size_t num_output_cols) {
    const size_t num_threads = thread_results.size();
    for (size_t c = 0; c < num_output_cols; ++c) {
        column_t& dst = results[c];
        size_t total = 0, tail_total = 0;
        for (auto& tr : thread_results) {
            if (c >= tr.size()) continue;
            total      += tr[c].size();
            tail_total += tr[c].size() % PAGE_CAPACITY;
        }
        dst.total_items = total;
        dst.pages.reserve((total + PAGE_CAPACITY - 1) / PAGE_CAPACITY);

        // 1. Move every full page (no copying).
        for (size_t t = 0; t < num_threads; ++t) {
            if (c >= thread_results[t].size()) continue;
            column_t& src = thread_results[t][c];
            assert(!src.is_view);
            size_t full = src.size() / PAGE_CAPACITY;
            for (size_t p = 0; p < full; ++p) dst.pages.push_back(std::move(src.pages[p]));
        }

        // 2. Copy the partial last pages, packed one after another.
        size_t tail_pages = (tail_total + PAGE_CAPACITY - 1) / PAGE_CAPACITY;
        size_t first_tail_page = dst.pages.size();
        for (size_t p = 0; p < tail_pages; ++p) dst.pages.push_back(acquire_page());
        size_t pos = 0;   // position within the tail area
        for (size_t t = 0; t < num_threads; ++t) {
            if (c >= thread_results[t].size()) continue;
            column_t& src = thread_results[t][c];
            size_t tail = src.size() % PAGE_CAPACITY;
            if (tail == 0) continue;
            const value_t* from = src.pages[src.size() / PAGE_CAPACITY]->data;
            size_t done = 0;
            while (done < tail) {
                size_t d_page = first_tail_page + pos / PAGE_CAPACITY, d_off = pos % PAGE_CAPACITY;
                size_t run = std::min(tail - done, PAGE_CAPACITY - d_off);
                std::copy_n(from + done, run, &dst.pages[d_page]->data[d_off]);
                done += run; pos += run;
            }
        }
    }
}

// Logic shared between standard Join and Root Join
struct JoinLogic{

    template <typename T, typename ResultContainer, typename MaterializeFn>
    static std::vector<ResultContainer> run_probe(
        UnchainedHashMap<T, size_t>& hash_table,
        ExecuteResult& probe_table,
        size_t probe_col_idx,
        bool build_left,
        size_t num_output_cols,
        MaterializeFn&& materialize_fn
    )
    {
        size_t num_threads = pool().size();
        std::vector<ResultContainer> thread_results(num_threads);
        for (auto& r : thread_results) r.resize(num_output_cols);

        auto& probe_col = probe_table[probe_col_idx];
        size_t total_rows = probe_col.total_items;

        std::atomic<size_t> next_batch_idx{0};
        // Rows each thread takes at a time: big enough that taking work is cheap, small
        // enough that every thread gets work (65536 left most threads idle on probe sides
        // below ~786k rows). About 8 pieces per thread, between 2048 and 65536 rows.
        const size_t BATCH_SIZE = std::clamp<size_t>(total_rows / (num_threads * 8), 2048, 65536);
        constexpr size_t PB = UnchainedHashMap<T, size_t>::PROBE_BATCH;

        pool().run([&](size_t t) {
            auto& local_res = thread_results[t];

            // Reports one match: probe row `probe_row` matched build row `build_row`.
            auto emit = [&](size_t probe_row, size_t build_row) {
                materialize_fn(local_res, build_left ? build_row : probe_row,
                                          build_left ? probe_row : build_row);
            };

            int32_t  keys[PB];      // used by the materialized path only
            uint32_t rows[PB];

            while (true) {
                size_t start = next_batch_idx.fetch_add(BATCH_SIZE, std::memory_order_relaxed);
                if (start >= total_rows) break;
                size_t end = std::min(start + BATCH_SIZE, total_rows);

                if (probe_col.is_view) {
                    // Zero-copy input: keys are already a contiguous int32 array, no nulls.
                    auto it = std::upper_bound(probe_col.view_page_offsets.begin(),
                                               probe_col.view_page_offsets.end(), start);
                    size_t page_idx = (it == probe_col.view_page_offsets.begin())
                                      ? 0 : std::distance(probe_col.view_page_offsets.begin(), it) - 1;
                    while (page_idx < probe_col.view_pages.size()) {
                        size_t page_start = probe_col.view_page_offsets[page_idx];
                        if (page_start >= end) break;
                        std::byte* raw_page = probe_col.view_pages[page_idx];
                        uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw_page);
                        size_t work_start = std::max(start, page_start);
                        size_t work_end   = std::min(end, page_start + num_rows);
                        const int32_t* data = reinterpret_cast<const int32_t*>(raw_page + 4)
                                              + (work_start - page_start);
                        for (size_t b = work_start; b < work_end; b += PB) {
                            size_t n = std::min(PB, work_end - b);
                            hash_table.lookup_batch(data + (b - work_start), n,
                                [&](size_t i, size_t build_row) { emit(b + i, build_row); });
                        }
                        page_idx++;
                    }
                } else {
                    // Materialized input: collect non-null keys (and their row numbers) into a batch.
                    size_t n = 0;
                    auto flush = [&] {
                        hash_table.lookup_batch(keys, n,
                            [&](size_t i, size_t build_row) { emit(rows[i], build_row); });
                        n = 0;
                    };
                    for (size_t cur = start; cur < end;) {
                        size_t page_idx = cur / PAGE_CAPACITY, offset = cur % PAGE_CAPACITY;
                        if (page_idx >= probe_col.pages.size()) break;
                        const value_t* cells = probe_col.pages[page_idx]->data + offset;
                        size_t count = std::min(end - cur, PAGE_CAPACITY - offset);
                        for (size_t i = 0; i < count; ++i) {
                            if (cells[i].is_null()) continue;
                            keys[n] = cells[i].get_int();
                            rows[n] = static_cast<uint32_t>(cur + i);
                            if (++n == PB) flush();
                        }
                        cur += count;
                    }
                    if (n) flush();
                }
            }
        });
        return thread_results;
    }
};

struct JoinAlgorithm {
    bool                                              build_left;
    ExecuteResult&                                    left;
    ExecuteResult&                                    right;
    ExecuteResult&                                    results;
    size_t                                            left_col, right_col;
    const std::vector<std::tuple<size_t, DataType>>&  output_attrs;
    const Plan* plan_ptr;

    template <class T>
    auto run() {
#ifdef PHASE_TIMERS
        auto join_start = std::chrono::steady_clock::now();
#endif
        UnchainedHashMap<T, size_t> hash_table;

        // Build Phase
        ExecuteResult& build_table = build_left ? left : right;
        size_t build_idx = build_left ? left_col : right_col;
        { PHASE_TIMER(build); hash_table.build(build_table, build_idx); }

        // Probe Phase
        ExecuteResult& probe_table = build_left ? right : left;
        size_t probe_idx = build_left ? right_col : left_col;
        size_t left_table_cols = left.size();

        // Resolve once per join where each output column comes from.
        struct OutSource { const column_t* col; bool from_left; };
        std::vector<OutSource> sources;
        sources.reserve(output_attrs.size());
        for (auto& [src_col, _] : output_attrs)
            sources.push_back(src_col < left_table_cols ? OutSource{&left[src_col], true}
                                                        : OutSource{&right[src_col - left_table_cols], false});
        const size_t num_cols = sources.size();

        std::vector<ExecuteResult> thread_results;
        {
            PHASE_TIMER(probe);
            thread_results = JoinLogic::run_probe<T, ExecuteResult>(
                hash_table, probe_table, probe_idx, build_left, output_attrs.size(),
                [&](ExecuteResult& local, size_t l_idx, size_t r_idx) {
                    if (num_cols == 0) return;
                    // All output columns of a thread have the same length; only column 0
                    // keeps the count during the probe (fixed up for the others below).
                    size_t n   = local[0].total_items;
                    size_t off = n % PAGE_CAPACITY;
                    if (off == 0)
                        for (size_t c = 0; c < num_cols; ++c) local[c].pages.push_back(acquire_page());
                    for (size_t c = 0; c < num_cols; ++c) {
                        const OutSource& s = sources[c];
                        local[c].pages.back()->data[off] = (*s.col)[s.from_left ? l_idx : r_idx];
                    }
                    local[0].total_items = n + 1;
                }
            );
        }

        // Give every output column the row count that column 0 tracked.
        for (auto& local : thread_results)
            for (size_t c = 1; c < num_cols; ++c) local[c].total_items = local[0].total_items;

        // Merge Phase
        {
            PHASE_TIMER(merge);
            results.resize(output_attrs.size());
            merge_thread_results(results, thread_results, output_attrs.size());
        }

#ifdef PHASE_TIMERS
        size_t b = build_table[build_idx].size();
        int k = b < 1000 ? 0 : (b < 100000 ? 1 : 2);
        g_timers.joins_by_size[k]++;
        g_timers.us_by_size[k] += std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - join_start).count();
#endif
    }
};

// Joins two materialized inputs on left[left_attr] = right[right_attr]. output_attrs index
// into the columns of left followed by the columns of right.
static ExecuteResult join_inputs(const Plan& plan, ExecuteResult& left, ExecuteResult& right,
                                 size_t left_attr, size_t right_attr, DataType key_type,
                                 const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    // Build on the smaller input. A join's result is the same either way.
    bool build_left = left[left_attr].size() <= right[right_attr].size();

    ExecuteResult results(output_attrs.size());
    JoinAlgorithm join_algorithm{
        .build_left    = build_left,
        .left          = left,
        .right         = right,
        .results       = results,
        .left_col      = left_attr,
        .right_col     = right_attr,
        .output_attrs  = output_attrs,
        .plan_ptr      = &plan
    };
    switch (key_type) {
        case DataType::INT32: join_algorithm.run<int32_t>(); break;
        default: throw std::runtime_error("unsupported join key type");
    }

    JOIN_RECORD(left[left_attr].size(), right[right_attr].size(),
                results.empty() ? 0 : results[0].size(), false);
    return results;
}

// The last join of a query: writes the result straight into the output format.
static ColumnarTable root_join_inputs(const Plan& plan, ExecuteResult& left, ExecuteResult& right,
                                      size_t left_attr, size_t right_attr, DataType key_type,
                                      const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    bool build_left = left[left_attr].size() <= right[right_attr].size();

    using RootResultType = std::vector<ColumnWriter>;

    auto run_root_join = [&](auto type_tag) -> ColumnarTable {
        using T = typename decltype(type_tag)::type;

        UnchainedHashMap<T, size_t> hash_table;

        ExecuteResult& build_table = build_left ? left : right;
        size_t build_col_idx = build_left ? left_attr : right_attr;

        { PHASE_TIMER(root_build); hash_table.build(build_table, build_col_idx); }

        ExecuteResult& probe_table = build_left ? right : left;
        size_t probe_idx = build_left ? right_attr : left_attr;
        size_t left_table_cols = left.size();

        std::vector<RootResultType> thread_results;
        {
            PHASE_TIMER(root_probe);
            thread_results = JoinLogic::run_probe<T, RootResultType>(
                hash_table, probe_table, probe_idx, build_left, output_attrs.size(),
                [&](RootResultType& writers, size_t l_idx, size_t r_idx) {
                    if (writers.empty()) writers.resize(output_attrs.size());
                    if (!writers[0].col) {
                        for (size_t i = 0; i < output_attrs.size(); ++i) {
                            writers[i].init(std::get<1>(output_attrs[i]));
                        }
                    }
                    for (size_t out_i = 0; out_i < output_attrs.size(); ++out_i) {
                        auto [src_col, dtype] = output_attrs[out_i];
                        value_t val;
                        if (src_col < left_table_cols) val = left[src_col][l_idx];
                        else val = right[src_col - left_table_cols][r_idx];
                        writers[out_i].add_val(val, plan);
                    }
                }
            );
        }

        PHASE_TIMER(root_finalize);
        ColumnarTable result_table;

        result_table.columns.reserve(output_attrs.size());

        for (size_t col_idx = 0; col_idx < output_attrs.size(); ++col_idx) {
            DataType type = std::get<1>(output_attrs[col_idx]);
            result_table.columns.emplace_back(type);
            Column& global_col = result_table.columns.back();

            for (auto& t_vec : thread_results) {
                if (col_idx >= t_vec.size()) continue;
                auto& writer = t_vec[col_idx];

                if (writer.col) {
                    writer.finalize();
                    global_col.pages.insert(global_col.pages.end(),
                                            writer.col->pages.begin(),
                                            writer.col->pages.end());
                    writer.col->pages.clear();
                }
            }
        }

        if (!result_table.columns.empty()) {
            size_t rows = 0;
            for (auto* page : result_table.columns[0].pages) {
                rows += *reinterpret_cast<uint16_t*>(page->data);
            }
            result_table.num_rows = rows;
        } else {
            result_table.num_rows = 0;
        }
        JOIN_RECORD(left[left_attr].size(), right[right_attr].size(),
                    result_table.num_rows, true);

#ifdef PHASE_TIMERS
        size_t str_cols = 0;
        for (auto& [_, type] : output_attrs) if (type == DataType::VARCHAR) ++str_cols;
        g_timers.root_rows       += result_table.num_rows;
        g_timers.root_str_values += result_table.num_rows * str_cols;
#endif

        return result_table;
    };

    struct IntTag { using type = int32_t; };
    if (key_type == DataType::INT32) {
        return run_root_join(IntTag{});
    } else {
        throw std::runtime_error("Unsupported join key type in root join");
    }
}

// Joins in the order the plan gives.
ExecuteResult execute_hash_join(const Plan& plan,
                                const JoinNode& join,
                                const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto left  = execute_impl(plan, join.left);
    auto right = execute_impl(plan, join.right);
    DataType key_type = std::get<1>(plan.nodes[join.left].output_attrs[join.left_attr]);
    return join_inputs(plan, left, right, join.left_attr, join.right_attr, key_type, output_attrs);
}

ColumnarTable execute_hash_join_root(const Plan& plan,
                                     const JoinNode& join,
                                     const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    auto left  = execute_impl(plan, join.left);
    auto right = execute_impl(plan, join.right);
    DataType key_type = std::get<1>(plan.nodes[join.left].output_attrs[join.left_attr]);
    return root_join_inputs(plan, left, right, join.left_attr, join.right_attr, key_type, output_attrs);
}

// Converts one input column (any column the zero-copy view can't handle) into value_t pages.
// Pass 1 reads only page headers to find where each input page's rows go in the output;
// pass 2 converts the pages in parallel, each into its own output positions.
static void scan_copy_column(const Column& column, size_t table_id, size_t in_col_idx, column_t& target_col) {
    const size_t n_src = column.pages.size();

    // Pass 1 (serial, headers only): first output row of every input page.
    std::vector<size_t> first_row(n_src + 1, 0);
    for (size_t p = 0; p < n_src; ++p) {
        uint16_t n = *reinterpret_cast<const uint16_t*>(column.pages[p]->data);
        size_t rows = (n == 0xFFFF) ? 1 : (n == 0xFFFE) ? 0 : n;   // long-string start / continuation / normal
        first_row[p + 1] = first_row[p] + rows;
    }
    const size_t total_rows = first_row[n_src];

    target_col.total_items = total_rows;
    size_t num_pages = (total_rows + PAGE_CAPACITY - 1) / PAGE_CAPACITY;
    target_col.pages.reserve(num_pages);
    for (size_t i = 0; i < num_pages; ++i) target_col.pages.push_back(acquire_page());

    const auto tid = static_cast<uint8_t>(table_id);
    const auto cid = static_cast<uint8_t>(in_col_idx);

    // Pass 2: convert input page p into output rows [first_row[p], first_row[p+1]).
    auto convert_page = [&](size_t p) {
        assert(column.type == DataType::INT32);
        const std::byte* page = column.pages[p]->data;
        const uint16_t header = *reinterpret_cast<const uint16_t*>(page);
        if (header == 0xFFFE) return;                               // long-string continuation: no row

        size_t dst = first_row[p];
        auto put = [&](value_t v) {
            target_col.pages[dst / PAGE_CAPACITY]->data[dst % PAGE_CAPACITY] = v;
            ++dst;
        };

        const uint16_t rows   = header;
        const auto*    bitmap = reinterpret_cast<const uint8_t*>(page + PAGE_SIZE - (rows + 7) / 8);
        if (column.type == DataType::INT32) {
            const auto* int_data = reinterpret_cast<const int32_t*>(page + 4);
            uint16_t data_idx = 0;
            for (uint16_t i = 0; i < rows; ++i)
                put((bitmap[i / 8] & (1u << (i % 8))) ? value_t(int_data[data_idx++]) : value_t());
        }
    };

    // Small columns: not worth waking the pool.
    constexpr size_t PARALLEL_MIN_PAGES = 16;
    if (n_src < PARALLEL_MIN_PAGES) {
        for (size_t p = 0; p < n_src; ++p) convert_page(p);
    } else {
        std::atomic<size_t> next{0};
        pool().run([&](size_t) {
            for (size_t p; (p = next.fetch_add(1, std::memory_order_relaxed)) < n_src;) convert_page(p);
        });
    }
}

ExecuteResult execute_scan(const Plan& plan,
    const ScanNode& scan,
    const std::vector<std::tuple<size_t, DataType>>& output_attrs) {
    PHASE_TIMER(scan);

    auto table_id = scan.base_table_id;
    auto& input   = plan.inputs[table_id];

    ExecuteResult results;
    results.resize(output_attrs.size());

    for (size_t out_col = 0; out_col < output_attrs.size(); ++out_col) {
        auto in_col_idx = std::get<0>(output_attrs[out_col]);
        auto& column    = input.columns[in_col_idx];
        auto& target_col = results[out_col];

        // =========================================================
        // ZERO-COPY VIEW OPTIMIZATION
        // =========================================================
        // If the column has no nulls and is INT32, we can reuse the
        // raw page buffers directly without copying data.
        if (column.type == DataType::INT32) {
            bool all_no_nulls = true;
            size_t total_items = 0;
            for (auto* p : column.pages) {
                auto* raw = p->data;
                uint16_t num_rows = *reinterpret_cast<uint16_t*>(raw);
                uint16_t num_non_null = *reinterpret_cast<uint16_t*>(raw + 2);
                total_items += num_rows;
                if (num_non_null != num_rows) { all_no_nulls = false; break; }
            }
            if (all_no_nulls) {
                std::vector<std::byte*> page_data_ptrs;
                page_data_ptrs.reserve(column.pages.size());
                for (auto* p : column.pages) page_data_ptrs.push_back(p->data);
                target_col.set_view_pages(page_data_ptrs, total_items, DataType::INT32);
                continue;
            }
        }

        // Standard Copy Path
        if (column.type == DataType::INT32) {
            PHASE_TIMER(scan_int);
            scan_copy_column(column, table_id, in_col_idx, target_col);
        } else {
            PHASE_TIMER(scan_str);
            scan_lazy_string_column(column, table_id, in_col_idx, target_col);
        }
#ifdef PHASE_TIMERS
        (column.type == DataType::INT32 ? g_timers.rows_int : g_timers.rows_str) += target_col.size();
#endif
    }
    return results;
}

ExecuteResult execute_impl(const Plan& plan, size_t node_idx) {
    auto& node = plan.nodes[node_idx];
    return std::visit(
        [&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, JoinNode>) {
                auto result = execute_hash_join(plan, value, node.output_attrs);
                MAT_RECORD(node_idx, result);
                return result;
            } else {
                return execute_scan(plan, value, node.output_attrs);
            }
        },
        node.data);
}

// ---------------------------------------------------------------------------------
// Join ordering at run time.
//
// Every join in a plan compares one column of its left side with one column of its right
// side, and a plan over n scans has n-1 joins. So the join conditions form a tree over the
// scans, and joining any two groups of scans that one condition connects gives the same
// result as the plan, in any order. We ignore the plan's tree, run every scan, and then
// repeatedly execute the condition whose two inputs are currently smallest. After each
// join its real size is known, so later choices use exact sizes, not estimates.
// Comment out USE_JOIN_ORDER to run joins in the plan's order instead.
// #define USE_JOIN_ORDER

// A column of a scan node: the scan's node index and the column's position in its output.
struct BaseCol {
    size_t scan, col;
    bool operator==(const BaseCol& o) const { return scan == o.scan && col == o.col; }
};

struct JoinEdge { BaseCol left, right; };

// The result of joining a group of scans so far.
struct Component {
    ExecuteResult        cols;
    std::vector<BaseCol> refs;   // refs[i]: which base column cols[i] holds
    size_t               rows = 0;
};

// Which base column each output column of `node` comes from.
static void base_cols(const Plan& plan, size_t node, std::vector<std::vector<BaseCol>>& out) {
    auto& n = plan.nodes[node];
    auto& o = out[node];
    o.resize(n.output_attrs.size());
    if (auto* j = std::get_if<JoinNode>(&n.data)) {
        base_cols(plan, j->left, out);
        base_cols(plan, j->right, out);
        size_t nl = plan.nodes[j->left].output_attrs.size();
        for (size_t c = 0; c < o.size(); ++c) {
            size_t s = std::get<0>(n.output_attrs[c]);
            o[c] = s < nl ? out[j->left][s] : out[j->right][s - nl];
        }
    } else {
        for (size_t c = 0; c < o.size(); ++c) o[c] = BaseCol{node, c};
    }
}

static void collect(const Plan& plan, size_t node, const std::vector<std::vector<BaseCol>>& bc,
                    std::vector<size_t>& scans, std::vector<JoinEdge>& edges) {
    auto& n = plan.nodes[node];
    if (auto* j = std::get_if<JoinNode>(&n.data)) {
        edges.push_back({bc[j->left][j->left_attr], bc[j->right][j->right_attr]});
        collect(plan, j->left, bc, scans, edges);
        collect(plan, j->right, bc, scans, edges);
    } else {
        scans.push_back(node);
    }
}

static DataType base_type(const Plan& plan, BaseCol c) {
    return std::get<1>(plan.nodes[c.scan].output_attrs[c.col]);
}

static ColumnarTable execute_reordered(const Plan& plan) {
    const auto& root = plan.nodes[plan.root];

    std::vector<std::vector<BaseCol>> bc(plan.nodes.size());
    base_cols(plan, plan.root, bc);

    std::vector<size_t>   scans;
    std::vector<JoinEdge> edges;
    collect(plan, plan.root, bc, scans, edges);
    const std::vector<BaseCol>& final_cols = bc[plan.root];

    // Run every scan; each one starts as its own component.
    std::vector<Component> comps;
    std::vector<size_t>    comp_of(plan.nodes.size(), SIZE_MAX);   // scan node -> component
    for (size_t s : scans) {
        Component c;
        c.cols = execute_scan(plan, std::get<ScanNode>(plan.nodes[s].data), plan.nodes[s].output_attrs);
        for (size_t i = 0; i < c.cols.size(); ++i) c.refs.push_back(BaseCol{s, i});
        c.rows = c.cols.empty() ? 0 : c.cols[0].size();
        comp_of[s] = comps.size();
        comps.push_back(std::move(c));
    }

    std::vector<bool> done(edges.size(), false);

    // A base column is still needed if the final result shows it, or a join that has
    // not run yet uses it as its key.
    auto needed = [&](BaseCol c) {
        for (auto& f : final_cols) if (f == c) return true;
        for (size_t e = 0; e < edges.size(); ++e)
            if (!done[e] && (edges[e].left == c || edges[e].right == c)) return true;
        return false;
    };
    auto find = [](const Component& comp, BaseCol c) {
        for (size_t i = 0; i < comp.refs.size(); ++i) if (comp.refs[i] == c) return i;
        throw std::runtime_error("join ordering: column not found");
    };

    for (size_t step = 0; step < edges.size(); ++step) {
        // The join whose two inputs are smallest together.
        size_t best = SIZE_MAX, best_cost = SIZE_MAX;
        for (size_t e = 0; e < edges.size(); ++e) {
            if (done[e]) continue;
            size_t a = comp_of[edges[e].left.scan], b = comp_of[edges[e].right.scan];
            if (a == b) throw std::runtime_error("join ordering: join graph has a cycle");
            size_t cost = comps[a].rows + comps[b].rows;
            if (cost < best_cost) { best_cost = cost; best = e; }
        }
        done[best] = true;
        const JoinEdge& edge = edges[best];
        size_t a = comp_of[edge.left.scan], b = comp_of[edge.right.scan];
        Component& L = comps[a];
        Component& R = comps[b];
        size_t l_attr = find(L, edge.left), r_attr = find(R, edge.right);
        DataType key_type = base_type(plan, edge.left);

        if (step + 1 == edges.size()) {
            // Last join: produce the final columns in the plan's order.
            std::vector<std::tuple<size_t, DataType>> out;
            for (auto& f : final_cols) {
                size_t i = (comp_of[f.scan] == a) ? find(L, f) : L.cols.size() + find(R, f);
                out.emplace_back(i, base_type(plan, f));
            }
            return root_join_inputs(plan, L.cols, R.cols, l_attr, r_attr, key_type, out);
        }

        // Intermediate join: keep only the columns something later still needs.
        std::vector<std::tuple<size_t, DataType>> out;
        Component J;
        for (size_t i = 0; i < L.refs.size(); ++i)
            if (needed(L.refs[i])) { out.emplace_back(i, base_type(plan, L.refs[i])); J.refs.push_back(L.refs[i]); }
        for (size_t i = 0; i < R.refs.size(); ++i)
            if (needed(R.refs[i])) { out.emplace_back(L.cols.size() + i, base_type(plan, R.refs[i])); J.refs.push_back(R.refs[i]); }
        J.cols = join_inputs(plan, L.cols, R.cols, l_attr, r_attr, key_type, out);
        J.rows = J.cols.empty() ? 0 : J.cols[0].size();

        // The joined result replaces component a; component b is emptied.
        for (size_t s : scans) if (comp_of[s] == b) comp_of[s] = a;
        comps[a] = std::move(J);
        comps[b] = Component{};
    }
    // A plan with no joins never reaches here: the root is always a join.
    throw std::runtime_error("join ordering: no joins");
}
// ---------------------------------------------------------------------------------

ColumnarTable execute(const Plan& plan, [[maybe_unused]] void* context) {
    reset_string_columns();
    MAT_ANALYZE(plan);
#ifdef USE_JOIN_ORDER
    auto result = execute_reordered(plan);
#else
    auto& root_node = plan.nodes[plan.root];
    auto& j = std::get<JoinNode>(root_node.data);
    auto result = execute_hash_join_root(plan, j, root_node.output_attrs);
#endif
    JOIN_QUERY_DONE();
    return result;
}

void* build_context() {
    return nullptr;
}

void destroy_context([[maybe_unused]] void* context) {
#ifdef PHASE_TIMERS
    auto ms = [](auto& a) { return a.load() / 1000.0; };
    fprintf(stderr,
        "scan %.0f | build %.0f | probe %.0f | merge %.0f | root build %.0f | "
        "root probe+output %.0f | root finalize %.0f  (ms)\n",
        ms(g_timers.scan), ms(g_timers.build), ms(g_timers.probe), ms(g_timers.merge),
        ms(g_timers.root_build), ms(g_timers.root_probe), ms(g_timers.root_finalize));
    const char* label[3] = {"<1k", "1k-100k", ">=100k"};
    for (int k = 0; k < 3; ++k)
        fprintf(stderr, "joins with build side %-7s: %4lu joins, %6.0f ms\n", label[k],
                (unsigned long)g_timers.joins_by_size[k].load(), ms(g_timers.us_by_size[k]));
    fprintf(stderr, "scan: INT32 with nulls %.0f ms (%.1f M rows) | VARCHAR (lazy) %.0f ms (%.1f M rows)\n",
        ms(g_timers.scan_int), g_timers.rows_int.load() / 1e6,
        ms(g_timers.scan_str), g_timers.rows_str.load() / 1e6);
    fprintf(stderr, "root output: %.1f M rows, %.1f M string values\n",
            g_timers.root_rows.load() / 1e6, g_timers.root_str_values.load() / 1e6);
#endif
}

} // namespace Contest