# Design and Optimization Report

This report covers how the engine went from 225 s to 1.9 s on the 113 JOB queries: what was built, what each optimization bought, what didn't work, and the bugs found along the way.

## Setup and methodology

- **Machine:** AMD Ryzen 5 7530U (6 cores / 12 threads), 16 GB RAM, Linux, GCC, `-DCMAKE_BUILD_TYPE=Release`.
- **Workload:** the 113 JOB queries in `plans.json`, run with `./build/fast` (input tables are loaded from a cache, so only `execute()` is timed).
- **Measurements:**
  - Every number in the rework log is the **median of 5 runs** on mains power. A laptop on battery throttles by about 20%, which is larger than most of the individual gains.
  - Correctness was checked on every step: all 113 results must match the reference.
  - Phase timers (compiled in with `#define PHASE_TIMERS` in `src/execute.cpp`) split each run into scan, build, probe, merge and root output. `perf` with DWARF call graphs (`RelWithDebInfo`) showed where the time went inside each phase.

## Part 1: from 225 s to 8 s

The first version used `std::unordered_map` and row-at-a-time tuples and took **225 s**. Three steps brought it to 8 s:

1. **Open-addressing hash tables** (Robin Hood, Hopscotch, Cuckoo, in `include/baselines/`).
   - Hopscotch was best at 211 s (−6%). Robin Hood gave 223 s, and Cuckoo was slower than the baseline at 239 s.
   - The real cause of the Cuckoo result was found later. Its second hash function is `hash ^ ((hash >> 16) + c)`, because `+` binds tighter than `^`. With `std::hash<int>` being the identity function, for keys below 65,536 the second position is a fixed function of the first. So keys that collide in one table also collide in the other, and any three of them force a rehash.
   - The general lesson was that per-key work mattered less than memory layout.
2. **Column store and the unchained hash table.**
   - Intermediate results became columns of 64-bit values, and INT32 columns without NULLs were read straight from the input pages. Time went 140 s → 55 s → 44 s.
   - On the hash table, CRC32 hashing beat Fibonacci hashing (8.5 s vs 10 s end-to-end at that stage).
3. **Parallelism.** Partitioned parallel build and parallel probe with work stealing brought it to **8 s** (28×).

## Part 2: review and bug fixes

Before optimizing further, every component was reviewed and the suspicious parts were reproduced in isolation. Four real bugs came out of it:

| Bug | Effect | Fix |
|---|---|---|
| Bloom tags built from `(i*7)&0xF`-style expressions depended only on `i mod 16`: 15 distinct tags, none with 4 bits set | False-positive rate of 7% instead of 0.06% for one key per slot, so ~100× more useless probes reached tuple storage | Take the 1,820 16-bit masks with exactly 4 bits set; pad to 2,048 |
| A long string continued while the next page started with `0xffff` (the start of a *new* long string) | Two adjacent long strings were glued together (a 10,000-char string came back as 19,000) | Continue only over `0xfffe` continuation pages |
| One partition on a 1-core machine gave `hash >> 64`, and zero cores divided by zero | Undefined behaviour and an out-of-bounds write (confirmed with UBSan) | At least 2 partitions; thread count at least 1 |
| String references stored the page index in 16 bits | Silent truncation for columns over 65,536 pages | Replaced by lazy strings with a 40-bit row index |

Each one has a regression test.

## Part 3: profiling-driven rework, 8.0 s → 1.9 s

Starting point: the bug-fixed version above, **8049 ms**. Each row is one commit. Times are medians in ms.

| # | Change | Total (ms) |
|---|---|---:|
| 0 | Baseline (bug-fixed) | 8049 |
| 1 | Persistent thread pool instead of creating threads in every phase of every join | 7370 |
| 2 | Reuse 2 MB slab chunks across joins instead of `mmap`/`munmap` for each join | 5557 |
| 3 | Pool and recycle result pages instead of `malloc`/`free` per page | 4280 |
| 4 | Remove the manual directory prefetch from the probe (see below) | 4141 |
| 5 | Parallel scan: prefix sum over page headers, then convert pages in parallel | 3930 |
| 6 | Merge per-thread results by moving full pages; copy only partial tails (merge 344 → 12 ms) | 3246 |
| 7 | Root output writes from a `string_view` into the input page, not a `std::string` per value | 3199 |
| 8 | Build the hash table on the input that is actually smaller | 2969 |
| 9 | Batched probe: hash, Bloom check and bucket scan as three separate loops over 256 keys | 2750 |
| 10 | Probe work size scales with input (about 8 pieces per thread) | 2576 |
| 11 | Lazy strings: carry `(table, column, row)`, resolve only at output | 2296 |
| 12 | Measurement fix (see below) | 1964 |
| 13 | Don't zero the directory serially; each partition zeroes its own slice in parallel | 1908 |

**Result: 8049 → 1908 ms, 4.2×.** Counting from the first version, 225 s → 1.9 s is 118×.

**Measurement fix (row 12).** The harness reports the total as query time plus the time of `build_context()`. But it adds the context time in **microseconds** to a total in **milliseconds**. Starting the thread pool inside `build_context()` took about 278 µs, so every total from row 1 to row 11 contains about 278 phantom "ms". Starting the pool lazily on first use removed it. Rows 1–11 are therefore each about 0.28 s too high. The end-to-end figure (row 0 to row 13) is not affected, because the baseline had no context to build.

### Where the time goes now

These numbers are summed over all 113 queries.

| Phase | Time | Notes |
|---|---:|---|
| Probe (intermediate joins) | ~1.08 s | 751 joins; close to the cost of the lookups themselves |
| Root join (probe + output) | ~0.31 s | 15.6 M result rows, 29.9 M strings resolved |
| Scan | ~0.26 s | INT32 columns with NULLs dominate (~0.2 s for 234 M rows); 319 M VARCHAR rows cost 14 ms thanks to lazy strings |
| Build | ~0.22 s | |

Half of the total runtime is spent in 12 queries (16b, 8c, 13d, 19d, 8d, 17e, 26c, 6f, 26a, 26b, 20c, 7a). All of them produce large intermediate results.

## What didn't work

- **Software prefetching in the probe.** Prefetching the directory entry before the lookup made the run slower (4333 ms with it, 4141 ms without). This matches §5 of the unchained-table paper: the Bloom filter already rejects most probes after a single directory load, and hardware out-of-order execution overlaps independent lookups. Batching (row 9) is what helped.
- **Contiguous-copy merge.** Replacing the per-element merge with contiguous copies made no measurable difference (4279 vs 4280 ms), because the copying itself was cheap. Avoiding the copy altogether (row 6) did help.
- **Binary search for lazy strings.** The first version of lazy strings found a row's page with `upper_bound`, which cost about 100 ns per value and wiped out the gain. A per-column block table (one entry per 1,024 rows) brought it to about 60 ns and made the change pay off.
- **Measuring on battery.** It gives numbers that are about 20% slower and swing between runs, which led to false conclusions twice.
- **Late materialization.** Before building it, I counted what the intermediate joins write: 60% of the values are join keys that any design must keep, and the rest come almost entirely from tables that contribute a single output column. Replacing them with row ids would write 99% as many values. Lazy strings had already removed the expensive part (copying strings), so the idea was dropped.
- **Greedy join ordering at run time.** Joining the two smallest current inputs first, using exact sizes, was 6% slower than the plans' order (median 2036 vs 1928 ms) and produced more intermediate rows (186 M vs 110 M). Input size is a poor guide: the rule joined large tables with tiny lookup tables that barely filter them, and postponed very selective joins because one side was large. Some queries did get much faster (13d 107 → 44 ms, 7a 56 → 12 ms), so a better choice per query could save about 11%, but only with good estimates of each join's output size, e.g. by sampling. The code is on the `join-order` branch.

## Testing

- **5 Catch2 programs**, built and run by GitHub Actions on every push:
  - `unit_tests`: end-to-end joins;
  - `regression_tests`;
  - `test_algorithms`: the baseline tables;
  - `test_indexing`;
  - `hash_function_tests`.
- **Regression tests** check the engine against simple reference implementations: `std::multimap` for the hash table, and a nested-loop join for whole plans. They are aimed at the bugs above and at things the optimizations could break:
  - pages recycled between queries;
  - string references surviving from a previous query;
  - results depending on which side is built.
- **Mutation testing.** Each fixed bug was put back by hand to check that some test fails. One test was too weak to catch its bug, so it was rewritten to force the same memory addresses to be reused between queries.
- **Sanitizers.** A second CI job runs all tests under AddressSanitizer and UndefinedBehaviorSanitizer.

## Next steps

- **Join ordering with estimated output sizes** (sampling, as the winning contest team did), building on the `join-order` branch.
- **Split `src/execute.cpp`** into scan, join and output units as part of that change.
- **Further directions.** The fastest contest entries also reorder joins using cardinality estimates and compile pipelines instead of interpreting them. Both are bigger architectural changes.
