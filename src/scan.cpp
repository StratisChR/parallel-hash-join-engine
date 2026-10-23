// Scans: turn the columns of an input table into the engine's column format. INT32
// columns without NULLs are used in place, INT32 columns with NULLs are converted in
// parallel, and VARCHAR columns become lazy string references.
#include <atomic>
#include <cassert>
#include <vector>

#include <plan.h>
#include "engine/column.h"
#include "engine/execute_internal.h"
#include "engine/instrumentation.h"
#include "engine/lazy_strings.h"
#include "engine/thread_pool.h"

namespace Contest {

// Converts one input column (any column the zero-copy view can't handle) into value_t pages.
// Pass 1 reads only page headers to find where each input page's rows go in the output;
// pass 2 converts the pages in parallel, each into its own output positions.
static void scan_copy_column(const Column& column, column_t& target_col) {
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
            scan_copy_column(column, target_col);
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

} // namespace Contest
