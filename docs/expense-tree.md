# Expense tree design

## Is a tree the right choice?

Yes, for a hierarchy where each category, transaction, or item has one parent.
This is a general rooted tree: each node can have any number of children. A
binary search tree would not model the expense hierarchy naturally.

```text
Expenses (root)
└── Food (group)
    └── Dominos (group)
        └── Dinner (transaction, 19800 paise)
            ├── Pizza (item, 15000 paise)
            └── Taco (item, 4800 paise)
```

Categories and merchant folders both use group nodes; nesting is optional. A
transaction can start directly under Food and move under Dominos later. The
merchant is also stored on the transaction, so merchant identity survives
reclassification. A group name is a display label, not a unique merchant ID.

A tree alone does not support every accounting query efficiently. Use a ledger
for account movements, a date index for chronological reports, and location
records/indexes for geography. Persisting transactions and parent relationships
in a database is compatible with this in-memory tree. A node-ID hash map gives
expected O(1) lookup; the tree defines ownership and classification.

One transaction can only belong to one category path in this version. Tags allow
overlapping classifications without copying the transaction. If a receipt must
allocate spending across different category branches, add explicit allocation
records whose amounts sum to the transaction total. Moving one existing split
item to another category is deliberately rejected because it would violate
the current transaction/item ownership model. Arbitrary overlapping category
membership would need relations or a graph and an explicit counting rule.

## Fields

| Owner | Field | Purpose |
| --- | --- | --- |
| Tree | `currency` | Currency code and decimal scale shared by all amounts. |
| Tree | `nodes` | Hash map of stable node IDs to node records. |
| Tree | `next_id` | Generates nonzero IDs without reusing deleted IDs. |
| Node | `id` | Stable identity within one tree; also identifies transactions. |
| Node | `kind` | Root, group, transaction, or split item. |
| Node | `name` | Editable display label. Names need not be unique. |
| Node | `parent_id` | One parent; absent only on the root. |
| Node | `children` | Ordered child IDs. An unsplit transaction has none. |
| Transaction | `expense.amount` | Canonical recorded total in signed 64-bit minor units. |
| Transaction | `expense.date` | Required calendar date used for range filtering. |
| Transaction | `expense.account_id` | Required reference to the account that paid. |
| Transaction | `expense.ledger_entry_id` | Optional link to a separately stored ledger entry. |
| Transaction | `expense.location_id` | Optional link to separately stored location details. |
| Transaction | `expense.merchant` | Merchant name independent of the category path. |
| Transaction | `expense.note` | Transaction-level explanation. |
| Transaction | `expense.tags` | Multiple overlapping labels without duplicating money. |
| Item | `item_amount` | Component of its parent transaction's total. |
| Item | `item_note` | Item-specific explanation. |
| Date range | `first`, `last` | Valid, inclusive calendar-date endpoints. |

`expense` is present only on transactions; `item_amount` only on items. Root and
group nodes have neither. Read access returns const nodes, and the container
validates all writes. Item dates, accounts, locations, and merchant information
are inherited through the parent transaction, so metadata does not drift when
the transaction is corrected. Use IDs for references rather than keeping node
pointers across deletions. IDs are local to one tree, not global database IDs;
persistence will need to restore them and the allocation counter consistently.

Account, ledger, and location IDs are separate typed identifiers. This container
checks that supplied IDs are nonzero; the application/storage layer must validate
that the referenced records exist. Coordinates and addresses belong in the
location store. Balances, income, loan principal, investments, and exchange rates
do not belong on expense nodes.

## Tag filtering

Tags classify a transaction across category boundaries. For example, a streaming
payment under Entertainment and a gym payment under Health can both carry the
`Subscription` tag. They remain in their existing tree branches while a tag
query can list them together and calculate their combined spending.

```cpp
// Add a label to an existing transaction without changing its accounting data.
auto expense = *tree.node(transaction_id).expense;
expense.tags.push_back("#Subscription");
tree.update_expense(transaction_id, expense);

const auto subscriptions = tree.transactions_by_tag(tree.root_id(), "subscription");
const auto spent = tree.total_by_tag(tree.root_id(), "#Subscription");
const auto monthly = tree.total_by_tag(tree.root_id(), "subscription", DateRange{
    std::chrono::year{2026} / std::chrono::October / 1,
    std::chrono::year{2026} / std::chrono::October / 31});
```

Both queries match the complete label, ignoring ASCII letter case, surrounding
whitespace, and one optional leading `#`. `Subscription`, `#Subscription`, and
`subscription` therefore match; `subscriptionExtra` does not. Stored labels are
preserved as supplied. Unicode characters are compared exactly; Unicode case
folding is not implemented. Blank query labels are rejected.

Queries can start at the root, a group, or a transaction and can combine a tag
with an inclusive date range. Split items inherit their parent's tags. A query
starting at an item uses its parent transaction for matching; the list returns
the transaction's ID, while the total contributes only that item's amount.
Results follow tree order. No match produces an empty list and a zero total.

A transaction with duplicate matching labels still appears once, and a split
transaction contributes its recorded total once. Different tag reports can
overlap: a transaction tagged both `Subscription` and `Work` belongs to both
reports, so adding those two report totals can count it twice.

Location is separate structured metadata: `location_id` points to a place record.
A label such as `Subscription` describes a transaction without replacing its
account, category, merchant, or location. Adding a label does not create a
recurring payment schedule; recurrence will need its own model.

## Accounting rules

- Root and group nodes contain groups or transactions. Transactions contain
  either zero items or at least two items. Items have no children.
- Each expense and item has a nonnegative amount. `Money` can represent signed
  values for future ledger use, but refunds need an explicit model before the
  expense tree accepts them.
- Monetary arithmetic uses integer minor units: 198 INR is 19800 paise. Currency
  code and decimal scale must both match. Currency metadata must be supplied
  from a trusted catalogue; the code does not implement that catalogue.
- Split amounts must sum exactly to the canonical transaction amount. Retaining
  the canonical amount keeps the accounting record stable while itemization
  changes. Transaction totals therefore equal the sum of their items.
- A group total visits transactions and counts each canonical amount once.
  It does not also add the transaction's items. An individual item query counts
  only that item. This is equivalent to summing leaves while the split invariant
  holds, with less work for fully itemized transactions.
- Moves, renames, splits, and merges preserve the transaction ID and accounting
  metadata. Self-parenting, cycles, moving the root, and detaching individual
  items are rejected.
- Splitting an already split transaction replaces all its items with fresh IDs.
  A rejected split preserves existing items. Merging removes itemization and
  keeps the recorded transaction amount.
- Explicit expense corrections and deletions may change totals. A split
  transaction must be merged before changing its total, then split again if
  needed. `erase_subtree` returns the amount removed.
- Arithmetic overflow throws rather than silently wrapping. This also applies
  to aggregate reports that exceed signed 64-bit minor units.

Financial changes must eventually run through an application service that updates
the ledger and expense records atomically in persistent storage. The tree itself
does not change balances or save to disk. The current implementation is an
in-memory container for use from one thread at a time.

## Queries and complexity

Let `v` be the number of visited group/transaction nodes, `s` the number of nodes
in a deleted subtree, `h` the destination's depth, and `d` the old parent's child
count. These bounds assume expected constant-time hash-map lookup and exclude
the cost of copying text/metadata.

| Operation | Time |
| --- | --- |
| Find by ID | Expected O(1) |
| Add group or expense | Expected amortized O(1) |
| Query transaction or item total | Expected O(1) |
| Query group total, with or without a date range | Expected O(v) |
| List transactions or total spending by tag | Expected O(v + inspected tag text) |
| Move group or transaction | Expected O(h + d), plus amortized destination append |
| Split or replace items | Expected O(new item count + old item count) |
| Merge items | Expected O(item count) |
| Delete subtree | Expected O(s + d) |

Totals and deletion use iterative traversal, so deeply nested categories do not
consume the call stack. Traversal can use O(v) or O(s) temporary memory. Date
queries still scan the subtree's transactions, including those outside the
range; this is not yet an indexed range-query implementation.

Tag queries also scan the selected subtree and compare the eligible transactions'
labels. They do not yet use a tag index. They can use O(v) traversal/result space
plus temporary memory proportional to a normalized label's length.

The first optimization should follow measured usage. Cached all-time subtree
totals can make those reads O(1), with ancestor updates on edits. Arbitrary date
ranges need another structure, such as a date index or maintained per-period
aggregates/prefix sums. Fixed-range cached answers can be O(1); unrestricted
date-range queries are not automatically O(1) merely because the data is a tree.
No cached group totals, duplicate transaction dates, or stored depths are added
in this version, avoiding additional consistency rules until they are needed.

## Example

```cpp
#include "moneta/expenses/expense_tree.hpp"

using namespace moneta;
using namespace std::chrono;

const Currency inr{"INR", 2};
ExpenseTree tree{inr};
const auto food = tree.add_group(tree.root_id(), "Food");
const auto dinner = tree.add_expense(food, "Dinner", Expense{
    Money{19800, inr}, year{2026} / October / 10, AccountId{1},
    std::nullopt, std::nullopt, "Dominos", "", {}});

const auto items = tree.split(dinner, {
    {"Pizza", Money{15000, inr}, ""},
    {"Taco", Money{4800, inr}, ""}});

const auto spent = tree.total(food); // 19800 paise, not 39600.
const auto pizza = tree.total(items.front()); // 15000 paise.
const auto daily = tree.total(food, DateRange{
    year{2026} / October / 10, year{2026} / October / 10});
```

Calendar dates are supplied by the caller; this module does not convert timestamps
or choose a timezone. Location and account stores, persistence, refunds, receipt
attachments, audit history, and indexes remain separate future work.
