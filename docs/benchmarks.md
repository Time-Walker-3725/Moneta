# Example benchmark run

Measured on 2026-10-13 in the cloud workspace, using GNU C++ 14.2.0, C++20,
CMake Release optimization, and an AMD EPYC 9V45 host. The benchmark is
single-threaded. These are single-run wall times, not percentiles or latency
guarantees for another machine.

The workload has 64 shallow categories, unsplit purchases, four tags mixed
within every category, integer amounts, eight account IDs, optional location
IDs, and dates distributed across 365 days. Names are short and notes empty.
The subscription/date filter matches 6,234 of 100,000 purchases and 62,335 of
1,000,000 purchases. Nested receipts and deep hierarchies are covered by unit
tests rather than this timing workload.

| Operation | 100,000 purchases | 1,000,000 purchases |
| --- | ---: | ---: |
| Construct transactions and indexes for explicit tag roots | 37.66 ms | 562.67 ms |
| First tag total, including structural leaf-index construction | 18.03 ms | 234.90 ms |
| Warm tag total | 2.89 ms | 42.85 ms |
| First date total, structural index already warm | 3.63 ms | 44.47 ms |
| Warm whole-tree date total | <0.01 ms | <0.01 ms |
| Warm tag + date total | 3.26 ms | 48.60 ms |
| Selective filtered-view miss, indexes warm | 3.01 ms | 56.99 ms |
| Complete filtered-view miss, indexes warm | 1.10 ms | 15.72 ms |
| Filtered-view cache retrieval | <0.01 ms | <0.01 ms |
| First combined total after amount edit and move, rebuilding indexes | 23.68 ms | 320.13 ms |
| Inherited category-tag total, leaf index reused | <0.01 ms | <0.01 ms |
| Peak process memory across the entire run | 72.25 MiB | 699.85 MiB |

A selective view can take longer than a complete view: the selective case must
also sort/merge explicit tag roots and evaluate predicates; the complete case
has no tag/date selection to prepare. Cache retrieval returns an immutable
shared view without copying, iterating, rendering, or exporting its rows.

The benchmark explicitly increases the cache row budget so a complete view can
be retained. The library default is four entries and 100,000 total rows;
oversize views are returned without caching. External retained shared pointers
can keep evicted views alive. Peak memory includes source records, indexes,
temporary rebuilds, matching IDs, and retained views; it is not source-tree-only
storage.

Cold/warm order matters. The first tag query builds the structural index; the
first date query builds the date index afterward. The first query after an
amount edit/move rebuilds both. Metadata/tag-only changes preserve leaf indexes.
Every reported run checks sums, matched leaf counts, view rows, cache identity,
and invalidation against independently generated expected amounts.

To measure locally:

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DMONETA_BUILD_BENCHMARKS=ON
cmake --build build-bench --target expense_tree_benchmark
./build-bench/expense_tree_benchmark --count 100000
./build-bench/expense_tree_benchmark --count 1000000
```

Use representative receipt depth, tag distributions, string lengths, and edits
when assessing an application workload. Construction, file persistence,
snapshot history, rendering, and network activity are separate costs.
