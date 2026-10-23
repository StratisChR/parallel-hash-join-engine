# Parallel Hash Join Engine

![Tests](https://github.com/StratisChR/parallel-hash-join-engine/actions/workflows/tests.yml/badge.svg)

An in-memory, multi-threaded join engine in C++17 for the [SIGMOD 2025 Programming Contest](https://sigmod-contest-2025.github.io/) task. It executes join pipelines over the 113 queries of the Join Order Benchmark (JOB, IMDb data).

**On a 6-core laptop it runs all 113 queries in 1.9 s.** The first working version took 225 s.

| Version | Total time (113 queries) | Speedup |
|---|---:|---:|
| `std::unordered_map`, row-at-a-time | 225 s | 1× |
| Column store + unchained hash table + threads | 8.0 s | 28× |
| Profiling-driven rework (this repo) | **1.9 s** | **118×** |

*Ryzen 5 7530U (6 cores / 12 threads), 16 GB RAM, Linux, Release build. Every number in the last row is the median of 5 runs.*

## What it does

The engine implements a single function:

```cpp
ColumnarTable execute(const Plan& plan, void* context);
```

A `Plan` is a tree of scans and equi-joins over paged columnar tables (INT32 and VARCHAR, both nullable). `execute` returns the result of the root join in the same paged format.

## Design

- **Unchained hash table** ([Birler, Schmidt, Neumann, Leis; DaMoN '24](https://doi.org/10.1145/3662010.3663442)).
  - A directory of 64-bit entries, each holding a 48-bit pointer into one dense tuple array and a 16-bit Bloom filter.
  - Each key sets 4 of 16 Bloom bits, taken from 1,820 precomputed tags. CRC32-based hashing.
  - Most lookups for non-matching keys are rejected by the Bloom filter without touching tuple storage.
  - Parallel two-phase build: threads partition tuples by hash, then each partition is counted, prefix-summed and copied into place. Memory comes from a slab allocator.
- **Batched probe.** Keys are processed 256 at a time in three tight loops: hash all of them, check the Bloom filters, then scan the buckets of the survivors. This keeps the loops branch-light and gives the CPU independent memory accesses to overlap.
- **Columnar intermediates.** Values are 64-bit words: an INT32, NULL, or a reference to a string. Results are written in 4,096-value pages that are recycled through a pool rather than freed.
- **Scan without copying where possible.**
  - INT32 columns without NULLs are read in place (zero-copy views).
  - INT32 columns with NULLs are decoded in parallel, page by page.
  - VARCHAR columns are never copied. The engine carries a `(table, column, row)` reference and resolves the string only when the final result is written.
- **Thread pool.** A persistent fork-join pool; the calling thread participates. Probes split their input into about 8 pieces per thread for load balancing. Per-thread results are merged by moving full pages and copying only the partial tails.
- **Build side chosen at run time.** The engine builds the hash table on the input that is actually smaller, not the side the plan names.

[`docs/REPORT.md`](docs/REPORT.md) has the step-by-step optimization log with measurements, what didn't work, and the bugs found along the way.

## Repository layout

```
include/hashtable/     unchained hash table, Bloom tags, hash functions, slab allocator
include/engine/        column/value representation, page pools, thread pool
include/baselines/     open-addressing tables from the first version (Robin Hood, Hopscotch, Cuckoo)
src/execute.cpp        entry point: runs a query plan
src/scan.cpp           scans of the input tables
src/join.cpp           hash joins: build, batched probe, merge, root output
src/lazy_strings.cpp   string references, resolved only at output
tests/                 Catch2 unit and regression tests
docs/REPORT.md         design and optimization report
```

The query harness, plan and table definitions (`include/plan.h`, `table.h`, `attribute.h`, …, `tests/read_sql.cpp`, `src/build_table.cpp`, `src/statement.cpp`, `src/csv_parser.cpp`) and the JOB queries come from the contest and course materials. The engine is everything under `include/hashtable`, `include/engine` and `src/execute.cpp`, `scan.cpp`, `join.cpp`, `lazy_strings.cpp`, plus the tests and CI.

## Build and run

Requires CMake ≥ 3.16, GCC or Clang with C++17 and an x86-64 CPU with SSE4.2 (for CRC32). CMake fetches the other dependencies.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
cmake --build build -j$(nproc)
```

To run the benchmark, download the IMDb data, build the cache once, then run all queries:

```bash
./download_imdb.sh
./build/build_database imdb.db
./build/build_cache plans.json
./build/fast plans.json
```

## Tests

```bash
cmake --build build -j$(nproc) --target unit_tests regression_tests test_algorithms test_indexing hash_function_tests
for t in unit_tests regression_tests test_algorithms test_indexing hash_function_tests; do ./build/$t; done
```

- `regression_tests` covers:
  - the hash table against `std::multimap`, including duplicates and NULLs;
  - every thread-pool task running exactly once;
  - recycled pages;
  - lazy string encoding;
  - adjacent long strings;
  - multi-way joins with large intermediates;
  - self-joins;
  - consecutive queries;
  - results that don't depend on the build side.

  Each test targets a bug that existed at some point. Re-introducing those bugs on purpose (mutation testing) confirmed that the tests catch them.
- CI (GitHub Actions) builds and runs every test program on each push. A second job runs them again under AddressSanitizer and UndefinedBehaviorSanitizer (`-DSANITIZE=ON`).
