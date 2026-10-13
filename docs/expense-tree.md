# Expense tree design

Currency metadata uses a fixed three-byte code and one decimal-scale byte.
`Money` combines that small value with signed 64-bit minor units; currency codes
do not allocate a string per transaction. `Currency::code()` returns a borrowed
`std::string_view` valid while the currency object lives.

## Ownership and amounts

Moneta uses a general rooted tree: categories, purchases, and receipt items each have one parent and ordered children. The node hash map provides stable identity; the parent/child links provide ownership and classification. A tree is appropriate for this hierarchy, while overlapping classifications belong in tags.

```text
Expenses
└── Groceries
    └── Walmart purchase (transaction)
        ├── Meat (composite item, tag: Recurring)
        │   ├── Chicken (item)
        │   └── Beef (item)
        └── Other (item)
```

Root/group nodes contain categories or transactions. A transaction contains items; an item may itself contain items. All nested items belong to the same transaction. An unsplit transaction or item is a monetary leaf. Every parent stores the sum of its immediate children's amounts, giving O(1) all-time totals. Empty categories total zero.

Amounts are nonnegative signed 64-bit minor units in one currency/scale: 198 INR is 19,800 paise. Currency mismatches and overflow throw. `Money` itself permits signed values for future ledger use, but the expense tree currently rejects negative expenses.

A transaction's separate `Expense.amount` is its canonical purchase amount and always equals the node total. Splitting/grouping/moving items preserves that amount. Editing an item leaf or deleting an item subtree corrects the canonical amount and ancestor totals. To change a split transaction's amount directly, merge it first or edit its monetary items.

## Storage

| Record | Stored fields |
| --- | --- |
| `ExpenseNode` | Stable ID, kind, name, parent ID, ordered child IDs, sorted explicit `TagId` vector, cached amount, owning transaction ID. |
| `Expense` | Purchase amount/date, account ID, optional ledger/location IDs, merchant, transaction note. Stored only for transactions. |
| Node notes | Nonempty category/item notes in a separate map; transaction notes remain in `Expense`. |
| `TagDefinition` | Stable ID, display name, normalized lookup key. |
| Tree | Currency, ID counters, node/purchase/note maps, tag registry/postings, revisions, lazy indexes, bounded view cache. |

Categories and items reserve no space for complete purchase metadata. Items obtain date, account, location, merchant, and canonical purchase context through `transaction_id`. Their own names, amounts, notes, and explicit tags remain independent.

`NodeId` identifies a particular node even after a rename or move. It is neither an array position nor a DFS position. IDs are nonzero, local to a tree, and never reused after deletion. Restoring into an existing tree preserves allocation high-water marks, including undo followed by a different edit branch. Account, ledger, location, and tag IDs use distinct C++ types. External record existence is the application's responsibility.

Child vectors preserve display order and provide direct pagination. Removing a child shifts siblings, costing O(d) for d siblings. This is a deliberate tradeoff for simple ordered storage. Arbitrarily deep user trees do not have logarithmic height; mutations updating ancestors cost O(h).

## Explicit and inherited tags

Every node kind can carry explicit tags in a sorted unique `vector<TagId>`. Small numeric vectors support binary-search membership without repeated string normalization or a hash allocation for every node. A registry resolves names once and allows renaming labels without rewriting node references.

Registry lookup trims outer ASCII whitespace, removes one leading `#`, trims again, and lowercases ASCII letters. Display names preserve case. `Subscription` and `#SUBSCRIPTION` share an ID; Unicode bytes remain unchanged. Blank creation/rename is rejected, and renaming onto another tag's normalized name is rejected.

A node effectively has a tag when it or any ancestor explicitly carries it. Tags inherit positively: removing a child's explicit tag does not cancel the same tag inherited from a parent. Repeated tags at different depths never multiply amounts. For `TagMatch::all`, the requested tags may come from different ancestors along a leaf's path.

Only matching monetary leaves contribute to a filtered total. Untagged ancestors remain as connecting rows when they contain matches. An item-level tag selects that item's portion of a purchase, and the transaction list returns its owning purchase once. Queries scoped to a subtree still respect tags on ancestors above that subtree. Zero-valued matching leaves remain visible.

## Refining and editing purchases

`split` applies to a transaction or nested item, requiring at least two new components whose amounts sum exactly to its current total. Splitting an existing composite replaces its descendants with fresh IDs. The original node's metadata/tags remain; removed descendant labels and notes disappear.

`merge` removes descendants and turns that transaction/item back into a monetary leaf of the same amount. Descendant labels/notes are intentionally removed. `group_items` groups selected sibling items in their existing order without changing their IDs. `ungroup_items` promotes its children and copies the removed group's explicit tags onto them to preserve tag membership.

Items may move only within their owning purchase, into a composite item or a zero-valued leaf. Categories and transactions move under category nodes. Moves reject cycles and root relocation. The implementation finds the common ancestor path and changes only ancestors unique to the source/destination paths; common ancestors retain their total, avoiding temporary overflow on a total-preserving move.

Deletion returns the removed amount and removes descendant records and explicit tag postings. Batch import uses an explicit authoritative-tree copy, applies all records to that candidate, then replaces the original only on success. This keeps the API simple and atomic but makes small batches on large trees relatively expensive.

## Queries and lazy indexes

The tree maintains an inverted index from each tag to nodes explicitly carrying it. It does not store inherited tags on every descendant.

A query lazily builds a DFS index containing preorder rows, subtree leaf intervals, compact leaf metadata, and amount prefix sums. A tagged node covers one contiguous leaf interval. Query preparation merges coverage intervals for any-tag matching or intersects coverage for all-tag matching. Prefix subtraction totals those disjoint intervals without visiting their leaves or double counting overlap.

A separate global date index sorts monetary leaves by transaction date and adds amount prefixes. Once built, whole-tree date-only totals use two binary searches. Tag/date/account/location combinations use the prepared tag ranges and inspect numeric leaf metadata. For totals with a date range, the implementation chooses between scanning covered leaves and inspecting the cheaper date candidate set with range-membership tests. It does not maintain an independent logarithmic index for every category/tag/account/location combination.

The first structural index costs O(n); the first date index costs O(L log L). Names, notes, merchant/ledger metadata, and tag edits reuse these indexes. Changes to amounts, dates, account/location, or topology advance `data_revision`; the next relevant query rebuilds its lazy indexes. All changes advance the view revision. Logical invalidation is constant work, but reclamation of old arrays/views can be deferred to replacement or eviction.

See [operations and complexity](operations.md) for complete bounds, including tag-root sorting and candidate scanning.

## Filtered views and cache

`filter_tree` returns `shared_ptr<const FilteredTree>`, a flat preorder array of `(NodeId, parent row index, filtered amount)`. The source tree is unchanged. Rows reference source IDs instead of copying purchase metadata and strings.

On a cache miss, the implementation identifies matching leaves, rolls amounts up the selected subtree, and emits matching leaves plus their ancestors. Constructing a complete view can require O(n) work even when relatively few rows survive. Cache entries are keyed by source revision, subtree, and normalized filter. A hit returns the same immutable shared object after a small bounded cache lookup.

Cache budgets limit both entry count and total cached rows; oversized views are returned without caching. The defaults are four entries and 100,000 rows. Callers holding shared views keep their memory alive after eviction. Returned rows remain immutable after edits, but metadata referenced by their IDs may have changed or been deleted: check the revision before resolving them against the current tree.

## Example

```cpp
#include "moneta/expenses/expense_tree.hpp"
#include "moneta/expenses/expense_history.hpp"
#include "moneta/storage/expense_store.hpp"

using namespace moneta;
using namespace std::chrono;

const Currency inr{"INR", 2};
ExpenseTree tree{inr};
const auto recurring = tree.create_tag("Recurring");
const auto groceries = tree.add_group(tree.root_id(), "Groceries");
const auto purchase = tree.add_expense(groceries, "Walmart", Expense{
    Money{19800, inr}, year{2026} / October / 10, AccountId{1},
    std::nullopt, LocationId{3}, "Walmart", "Receipt"});
const auto parts = tree.split(purchase, {
    {"Meat", Money{15000, inr}, "", {recurring}},
    {"Other", Money{4800, inr}, "", {}}});
const auto meat = tree.split(parts[0], {
    {"Chicken", Money{10000, inr}, "", {}},
    {"Beef", Money{5000, inr}, "", {}}});

QueryFilter filter;
filter.tags = {recurring};
filter.dates = DateRange{year{2026} / October / 1, year{2026} / October / 31};
const auto spent = tree.filtered_total(groceries, filter); // 15000 paise.
const auto view = tree.filter_tree(groceries, filter);

ExpenseHistory history{5};
history.checkpoint(tree); // Opt in before the edit.
tree.update_item(meat[0], Money{11000, inr}); // Purchase becomes 20800 paise.
history.undo(tree); // Restore 19800, preserving future ID allocation safety.

save_tree_file("expenses.moneta", tree);
auto restored = load_tree_file("expenses.moneta");
```

## Persistence and history

`save_tree`/`load_tree` provide a versioned text format preserving IDs, currency, ordering, tags, notes, and canonical metadata. Loading bounds declared arrays/text, validates graph ownership and totals, and rejects malformed/truncated/trailing data. Query caches and revisions are not persisted.

File saving writes/closes a temporary file on the destination filesystem and then renames it atomically. Failed writes leave an existing destination untouched; crash durability through `fsync` is not provided. CSV export writes monetary leaves, including explicit/effective tags and purchase metadata, with RFC 4180 quoting. It is a report export; CSV import is not implemented.

`ExpenseHistory` is optional bounded snapshot history. Checkpoint before an edit; ordinary tree operations do not automatically copy history. Undo/redo validates and restores full snapshots. Each entry costs approximately the authoritative tree's storage, and checkpoint sorting/copying is expensive for large trees. Capacity zero disables checkpointing.

The container is single-threaded and does not update account balances. Calendar dates are supplied by callers; timestamp/timezone conversion, refunds, recurring schedules, multi-currency conversion, and cross-category allocation records need their own models. Disk/network latency and a particular interactive response time are not implied by asymptotic bounds.
