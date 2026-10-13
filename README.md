# Moneta

Moneta is a C++20 accounting backend. Its expense tree organizes purchases, categories, and nested receipt items while preserving exact integer amounts. Accounts, the ledger, income, debts, investments, and a frontend remain separate planned modules.

```text
Expenses                         198 INR
└── Food                         198 INR
    └── Dinner                   198 INR
        ├── Pizza                150 INR
        └── Taco                  48 INR
```

An unsplit purchase carries its amount. Splitting it replaces that amount with its components for aggregation; parent and component amounts are never added together. Every node caches its all-time subtree sum. Moving or refining a purchase preserves its total; correcting or deleting an item changes the purchase's recorded amount.

## Implemented backend

- Ordered categories, purchases, nested items, grouping, splitting, merging, moves, corrections, deletion, and atomic batch imports.
- Stable typed IDs and sorted numeric tag IDs on every kind of node. Explicit tags inherit downward without copying labels into descendants.
- Any/all tag filters combined with inclusive dates, account, and location; transaction lists, leaf lists, and immutable filtered tree views.
- Explicit-tag postings, lazy leaf intervals/prefix sums, a lazy global date index, and bounded filtered-view caching.
- Validated snapshots, versioned stream/file persistence, leaf-based CSV export, and optional bounded undo/redo history.

[Expense tree design](docs/expense-tree.md) explains the model and API. [Operations and complexity](docs/operations.md) records the actual costs and index invalidation rules. All-time total reads are O(1); combined queries and complete filtered views can still require linear work. Performance depends on data, hardware, compiler settings, and whether indexes/caches are warm.

## Tags and filters

```cpp
#include "moneta/expenses/expense_tree.hpp"

using namespace moneta;
using namespace std::chrono;

const Currency inr{"INR", 2};
ExpenseTree tree{inr};
const auto subscriptions = tree.create_tag("Subscription");
const auto entertainment = tree.add_group(tree.root_id(), "Entertainment");
tree.add_expense(entertainment, "Streaming", Expense{
    Money{19900, inr}, year{2026} / October / 10, AccountId{1},
    std::nullopt, std::nullopt, "Streaming service", ""}, {subscriptions});

QueryFilter filter;
filter.tags = {subscriptions};
filter.dates = DateRange{year{2026} / October / 1, year{2026} / October / 31};
const auto spent = tree.filtered_total(tree.root_id(), filter); // 19900 paise.
const auto view = tree.filter_tree(tree.root_id(), filter);
```

Tags belong to nodes, not to `Expense` metadata. A category tag includes its descendant purchases; an item tag can select only part of a receipt. Distinct tag reports can overlap, while a single report always counts each monetary leaf once.

## Layout

```text
include/moneta/core/       Money, dates, and typed identifiers
include/moneta/expenses/   Public tree, tag, and history interfaces
include/moneta/storage/    Persistence and CSV interfaces
src/                      Implementations matching those modules
tests/unit/              Dependency-free test executables
benchmarks/               Optional deterministic performance workload
docs/                    Design and operation reference
```

Headers describe interfaces other code can include; source files implement them. Tree metadata references account, ledger, and location stores by typed IDs. The application must validate those external records and coordinate financial changes with the ledger; these tree operations do not calculate account balances.

## Build and test

Install CMake separately from the compiler. Use a C++20 compiler and CMake 3.20 or newer:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

On macOS, Apple Clang from Xcode Command Line Tools or a compatible Homebrew GCC can be selected through the CMake compiler kit. Clear the CMake cache after changing compilers. VS Code settings are local configuration and are not part of the library.

Without CMake, compile and run each test directly from the repository root:

```sh
mkdir -p build
for test in tests/unit/*_tests.cpp; do
    name="${test##*/}"
    name="${name%.cpp}"
    g++ -std=c++20 -O2 -Wall -Wextra -Wpedantic -Iinclude \
        src/core/*.cpp src/expenses/*.cpp src/storage/*.cpp \
        "$test" -o "build/$name" || exit 1
    "./build/$name" || exit 1
done
```

## Optional benchmark

```sh
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DMONETA_BUILD_BENCHMARKS=ON
cmake --build build-bench --target expense_tree_benchmark
./build-bench/expense_tree_benchmark --count 100000
```

For an explicit larger run:

```sh
./build-bench/expense_tree_benchmark --count 1000000
```

The benchmark defaults to 100,000 purchases and accepts at most 1,000,000. It measures construction, cold/warm totals, combined filtering, selective and complete views, cache reuse, inherited category tags, and edits/moves followed by index rebuilding. It checks results against independent arithmetic. [An example cloud run](docs/benchmarks.md) records the workload, timings, and memory use. These workload-specific measurements do not establish a 500 ms guarantee, disk performance, or network latency.

## Planned accounting modules

The ledger will record cash movements and support account balances. Income, transfers, loan principal, investments, and exchange-rate conversions need their own accounting rules. Future work includes account/ledger integration, income and obligations, investment tracking, location summaries, recurring schedules, and application interfaces. A tag named `Recurring` is a label; it does not schedule payments.
