# Moneta

Moneta is a C++20 accounting backend. Its expense tree organizes purchases, categories, and nested receipt items while preserving exact integer amounts. Its accounting book records cash movements, income, debt principal, fixed-interest investments, currency exchanges, and scheduled payments.

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

## Accounting book

```cpp
#include "moneta/storage/book_database.hpp"

using namespace moneta;
using namespace std::chrono;

BookDatabase db{"household.sqlite"};
const Currency usd{"USD", 2};
const Date today = year{2026}/November/25;
if (db.book().accounts().empty()) {
    db.edit("Set up checking", [&](Book& book) {
        auto checking = book.open_account("Checking", usd);
        book.opening(checking, Money{100000, usd}, today);
    });
}
const auto revision = db.edit("Buy groceries", [&](Book& book) {
    book.spend(AccountId{1}, Money{1250, usd}, today, "Groceries");
}); // Durable when edit returns.
const auto balance = db.book().account(AccountId{1}).balance;
db.undo(); // Reverse the payment and remove the receipt from active reports.
db.redo(); // Reapply the same payment.
auto earlier = db.at(revision); // Independent snapshot.
```

One SQLite file holds the current book, its revision branches and its financial audit. Each successful edit commits immediately. See the [storage guide](docs/sqlite.md) for migration, backups, history lookup and recovery.

Use the book for coordinated cash and expense writes. Each currency has an independent expense tree. A payment creates one journal entry and one linked purchase. Splits change how spending is classified while preserving the payment. The [accounting API guide](docs/accounting.md) covers debt, FX, schedules, editing receipts, persistence and importing an existing tree.

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
include/moneta/accounting/  Book, journal, schedules and exchange rates
include/moneta/core/       Money, dates, and typed identifiers
include/moneta/expenses/   Public tree, tag, and history interfaces
include/moneta/storage/    Persistence and CSV interfaces
src/                      Implementations matching those modules
tests/unit/               Unit and SQLite integration test executables
benchmarks/               Optional deterministic performance workload
docs/                    Design and operation reference
```

## Build and test

Build with a C++20 compiler, CMake 3.20 or newer, and SQLite 3.33 or newer with development headers. On Debian or Ubuntu install `libsqlite3-dev`. For a nonstandard SQLite installation, pass `-DCMAKE_PREFIX_PATH=/path/to/prefix` when configuring.

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
        src/core/*.cpp src/accounting/*.cpp src/expenses/*.cpp src/storage/*.cpp \
        "$test" -lsqlite3 -o "build/$name" || exit 1
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
