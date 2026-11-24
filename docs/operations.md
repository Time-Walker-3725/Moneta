# Operations and complexity

These bounds describe the implemented backend. Hash-map/set costs are expected bounds, with linear worst cases under adverse collisions. Ordered category trees can have height O(n); their height is not automatically O(log n). Text processing, copying returned records, allocation, and storage reclamation also take work.

## What happens when a user selects dates?

A calendar day is an inclusive range with identical endpoints. The same selection has different costs depending on what the caller asks to receive.

| Request, date predicate only | First call after a structural edit | Warm query with a different range |
| --- | --- | --- |
| Total at the root | Expected O(n + L log L) to enumerate nodes and sort monetary leaves by day | O(log L) for two binary searches and a prefix difference |
| Total inside a category with c leaves | Same lazy builds if the category covers purchases | O(log L + min(c, D)), where D counts leaves on those dates across all categories |
| Tree rooted at a category | Expected O(n + c + k) when the structural index is cold. Listing builds only the structural index. | O(c + k) with only structure warm. With dates indexed, O(log L + min(c, D log(D+2)) + k). Here k includes visible ancestors. |
| Repeat the identical tree request at the same revision | A retained result is eligible for reuse | O(E) with no tags, returning the shared view. Reading its k rows still costs O(k). |

These rows describe nontrivial scopes. A single purchase/item leaf checks its date directly in O(1). The view cache defaults to four entries and 100,000 retained rows across entries. A result exceeding the row budget is returned to the caller without retention. Scalar date totals reuse indexes. Only filtered views cache the complete answer.

## Notation

| Symbol | Meaning |
| --- | --- |
| `n`, `T` | Total nodes and registered tag definitions. |
| `h`, `d`, `s` | Affected path height, affected parent child counts, and affected/scoped subtree nodes. |
| `t`, `u`, `A` | Explicit tags on a node, selected query tags, and explicit tag references along one ancestor path. |
| `p`, `b` | New/selected immediate items, and imported purchases. |
| `R`, `B` | Stored/processed references and text bytes. Subscript `s` restricts these to a subtree. |
| `L`, `m`, `r` | Monetary leaves, explicit tag roots inspected across selected postings, and merged coverage ranges. |
| `c`, `D`, `k` | Covered candidate leaves, leaves in the selected global date interval, and returned rows/IDs. |
| `E`, `K` | Cache entry budget and rows whose storage is actually reclaimed. |
| `P` | Text search pattern length. |

`Q(t)` means O(t) normalization/validation for sorted tag IDs, or O(t log t) if sorting is needed. Inputs are deduplicated. `C = O(n + T + R + B)` describes an authoritative-tree copy. `S = O(n log n + T log T + R + B)` describes a deterministically sorted snapshot. Bounds for live-record operations exclude unrelated tree copying because it is not performed.

## Identity, storage access, and mutation

| Public operation | Time | Notes |
| --- | --- | --- |
| Construct tree | O(root-name length) | Creates one root. |
| Copy construct/assign | `C` | Copies authoritative maps/postings, not lazy indexes or view caches. Replacing a destination also reclaims its prior storage. |
| Move construct/assign | O(1) transfer | Assignment additionally destroys prior destination storage. |
| `root_id`, `currency`, `size`, `transaction_count`, `revision`, `tag_registry` | O(1) | Return scalar values/references. |
| `contains`, `node`, `expense`, `note` | Expected O(1) | Return references/views; unknown IDs throw except `contains`. |
| `children(id, offset, limit)` | O(k) | O(1) offset access; copies only the requested ordered slice. |
| `is_descendant` | O(h) | Includes self; walks parent links. |
| `add_group` | Amortized O(1 + Q(t)) | Plus name bytes; creates an empty category. |
| `add_expense` | Amortized O(h + Q(t)) | Updates ancestor sums; plus metadata/name bytes. |
| `rename`, `set_note` | O(processed text) | No financial amount changes; empty nontransaction notes are omitted. |
| `update_expense` | O(h) for an amount change; otherwise O(1), plus metadata bytes | A split purchase's amount cannot change directly. Date/account/location edits invalidate leaf indexes. |
| `update_item` | O(h) | Only monetary item leaves; adjusts the owning purchase amount and ancestor sums. |
| `move` | O(h + d) | Removes a child from an ordered vector; common ancestors receive no amount change. Destination append is amortized O(1), occasionally O(destination children). |
| `split` | O(s + p + R_s + sum Q(item tags)) | Replaces descendants; preserves the split node's sum. Adds new item/name/note storage. |
| `merge` | O(s + R_s) | Removes descendants; preserves amount, removes descendant labels/notes. |
| `group_items` | O(d + p + Q(t)) | Hash-validates selected siblings, preserves their IDs/order. |
| `ungroup_items` | O(d + p + sum child tags + p × group tags) | Sorted unions preserve the removed group's tags on promoted children. |
| `erase_subtree` | O(s + h + d + R_s) | Removes records/postings, returns removed amount, updates purchase amount for item deletion. |
| `import_expenses` | `C` + O(bh + sum Q(tags) + batch bytes) | Atomic candidate copy; destination replacement also reclaims prior data/caches. |

Adding/removing children, grouping, splitting, merging, and moves change topology. Their next indexed query can cost O(n) to rebuild the leaf index, even when the immediate mutation is cheap. A future date query can additionally rebuild its O(L log L) date index. The mutation bounds do not hide that later work.

## Tag registry and membership

| Public operation | Time | Notes |
| --- | --- | --- |
| `create_tag` / registry `create` | Expected O(name length) | Duplicate normalized name returns the existing ID. |
| `find_tag` / registry `find`, registry `normalize` | O(name length), expected hash lookup | Blank lookup returns no result. |
| `tag` / registry `get` | Expected O(1) | Returns definition by ID. |
| `rename_tag` / registry `rename` | Expected O(old + new name length) | Stable ID; collisions rejected. |
| `erase_tag` | O(m × t_max + name length) | Removes explicit references using postings; inherited descendants need no updates. |
| Registry `erase` | Expected O(stored key length) | Registry-only operation; tree-owned deletion should use `erase_tag`. |
| Registry `size`, `next_id` | O(1) | IDs are nonzero, monotonic and never wrap. |
| Registry `definitions` | O(T log T + tag bytes) | Copies definitions in ID order. |
| Registry `restore` | Expected O(T + tag bytes) | Validates names/keys, uniqueness, and counter before swapping. |
| `set_tags` | O(Q(new tags) + old tags) | Updates explicit postings, retains sorted uniqueness. |
| `add_tag`, `remove_tag` | O(t) | Vector copying/shifting and sorted posting updates. |
| `has_explicit_tag` | O(log(t + 1)) | Binary search; validates the tag ID. |
| `has_tag` | O(h log(t_max + 1)) | Walks the ancestor path including self. |
| `effective_tags` | O(h + A log A) | Produces sorted unique inherited and explicit IDs. |
| `explicit_nodes` | O(m) | Copies explicit postings in unspecified order. |

Names/notes/tag edits invalidate filtered views by advancing the public revision while preserving the leaf/date index generation. Tag coverage is prepared from current explicit postings at query time, so inherited membership does not need per-descendant maintenance.

## Query preparation

Every filter validates selected IDs and normalizes tags in `Q(u)` time. Empty category scopes, trees without purchases, and impossible tag coverage (unused definitions) skip index construction. Monetary-leaf scopes use owning transaction metadata and ancestor tag checks directly, costing O(u h log(t_max + 1)) without building a whole-tree index. A cold structural index for other scopes requires expected O(n) time and O(n + L) storage. A cold global date index additionally requires O(L log L) time and O(L) storage.

With structure warm, tag preparation inspects `m` explicit roots, intersects their leaf intervals with the requested scope, and merges/unites/intersects them in O(m log m + r) time and O(m + r) workspace. The roots can include ancestors above the scope and postings elsewhere in the tree. Preparation costs are paid per query; aggregate answers for all tag combinations are not maintained.

| Public query | Warm time after normalization | Notes |
| --- | --- | --- |
| `total(subtree)` | O(1) | Cached all-time sum. |
| `filtered_total(subtree, empty filter)` | O(1) | Same cached sum; no index needed. |
| Tag-only `filtered_total` | O(m log m + r) | Sum disjoint covered intervals using leaf prefixes. |
| Whole-tree date-only `filtered_total` | O(log L) | Global date prefix subtraction. |
| Scoped date total or combined tag/date/account/location total | O(m log m + r + log L + min(c, D log(r + 1))) with dates | Chooses covered-leaf scan or cheaper date candidates. Without dates, O(m log m + r + c). |
| `total(subtree, dates)` | Same as dated `filtered_total` | Global date-only shortcut applies only to the whole tree. |
| `matching_leaves` | O(m log(m+1) + r + F + k) | F is the selection cost below. IDs follow tree order. |
| `transactions` | Same as `matching_leaves` | Deduplicates contiguous transaction owners without a hash set; partial items return their owning purchase. |
| `total_by_tag`, `transactions_by_tag` | Name resolution plus corresponding query | Unknown names produce zero/empty; blank query names throw. |
| `search` | O(s + B × P), worst case | Case-sensitive substring search across names/notes/merchant; empty text matches all nodes. |

For lists and views, let `F = O(c)` for the ordinary scan. With a date predicate and an already-built date index, `F = O(log(L+1) + r + min(c, D × (log(r+1) + log(D+2))))`. Two binary searches find the date candidates. The implementation compares estimated membership and sorting work with the length of the covered scan. The selective path restores tree order by sorting matching leaf positions and uses O(matching leaves) temporary space. The scan streams matches in tree order. A date total needs no output sorting.

Add `Q(u)` to normalized-filter query bounds and cold build costs when applicable. For no tag selection, range preparation is constant; for date totals, the O(log L) term is the date-bound search. The implementation can shortcut empty coverage before constructing date indexes.

A combined total can scan O(L) candidates in the worst case, in addition to sorting tag roots. Listing `k` records necessarily costs at least Ω(k). Child slices support pagination; transaction/leaf query methods return complete ID lists rather than promising paginated logarithmic retrieval.

## Filtered views and cache

| Public operation | Time | Space/behavior |
| --- | --- | --- |
| `filter_tree`, miss | Expected O(Q(u) + E(u+1) + m log(m+1) + r + F + k), plus a cold O(n) structural index and any eviction | k counts matching leaves and visible ancestors. Each emitted edge is climbed once. Path stacks and output use O(k) space beyond selection. |
| `filter_tree`, hit | O(Q(u) + E × (u + 1)) | Returns the same shared immutable view; small bounded LRU comparison/movement. |
| `set_filter_cache_limits` | O(E + K) | Enforces entry/row budgets and reclaims unreferenced evictions. |
| `clear_filter_cache` | O(E + K) | Callers retaining shared pointers keep those rows alive. |

A date-only root view can still take O(n) even for few matches when it must scan all leaves. After selection, assembly costs expected O(k). With tags, sorting m explicit postings can cost O(m log m) even for one selected tag. Views store IDs, parent row indices and amounts, not copied metadata. Oversize results bypass caching. Logical revision invalidation is O(1); old index/view reclamation can occur later and add linear work to replacement/eviction. A bounded cache hit is inexpensive but not literally O(1) with arbitrarily large tag filters or cache capacity.

## Snapshots, persistence, CSV, and history

| Public operation | Time | Space/behavior |
| --- | --- | --- |
| `snapshot` | `S` | Deterministic ID sorting; copies authoritative data, O(n + T + R + B) storage. |
| `from_snapshot` | Expected O(n + T + R + B) | Validates graph, parents, ownership, sorted tags, amounts, and counters; rebuilds explicit postings. |
| Tree `restore` | Same validation plus old-storage reclamation | Atomic replacement; preserves node/tag high-water marks and advances revision. |
| `save_tree`, `save_tree_file` | `S` + output bytes | Snapshot plus versioned serialization; file variant adds filesystem I/O. |
| `load_tree`, `load_tree_file` | Expected O(n + T + R + B) | Bounded parsing and validation; file variant adds filesystem I/O. |
| `export_expenses_csv` | Matching-leaf query + O(k(h + A log(A+1)) + output bytes) | Effective tags computed per matching leaf; O(k + maximum row bytes + maximum path tag references) beyond query preparation. |
| `ExpenseHistory` constructor | O(1) | Opt-in queue capacity; default 20. |
| History `checkpoint` | `S` + discarded snapshot data | Saves state before an edit, clears redo, evicts past capacity. Capacity zero does no copy. |
| History `undo`, `redo` | `S` + expected O(n + T + R + B), plus discarded data | Saves current state and validates/restores target; empty queue returns false. |
| History `can_undo`, `can_redo`, queue sizes, `capacity` | O(1) | Scalar/queue access. |
| History `set_capacity`, `clear` | O(discarded snapshot data) | Bounds undo and redo queues independently. |

Snapshots/serialization/history copy strings and references; they are not inexpensive O(1) backups. Each history entry contains a full tree, with undo and redo queues each bounded by capacity. Ordinary edits do not checkpoint automatically. Use a small capacity or disable history when storage is a concern.

The standalone text reader validates configurable `LoadLimits` before large declared allocations. Stream/file formats preserve source records, not query caches. File replacement is atomic on the destination filesystem, but crash durability through `fsync` is not promised. CSV is a monetary-leaf report with quoted fields and length-prefixed tag-name columns; it is not a tree round-trip format.

`Currency`, `Money`, typed-ID comparisons, and `DateRange` checks use bounded scalar operations; money addition detects currency/scale mismatches and 64-bit overflow. A tree uses one currency and valid calendar dates, with timezone conversion left to callers.

These are operation counts, not wall-time guarantees. Run the [optional benchmark](../README.md#optional-benchmark) with Release optimization and representative data. Cold/warm behavior, allocation/storage layout, OS scheduling, disk I/O and network work materially affect measured latency.


## Accounting book

Let J be journal entries, U be accounts, V be currencies, G be distinct locations in a result, and H be the total in-memory size of a book including strings and trees. String inputs add their processed byte length. Direct IDs index append-only deques. Account balances and investment status are updated when posting. Reading them needs no journal scan.

| Public operation | Time | Storage and details |
| --- | --- | --- |
| `account`, `location`, `investment`, `schedule`, `entry`, `journal`, `accounts`, `locations`, `investments`, `schedules` | O(1) | Borrowed references. Iterating a collection costs its length. |
| `open_account` | Amortized O(log(V+1) + name bytes) | Creates an empty tree on first use of a currency. |
| `add_location`, `add_schedule` | O(name or description bytes + 1) | One persistent record. |
| `expenses(currency)` | O(log(V+1)) | Ordered currency lookup. |
| `opening`, `income`, `principal`, `transfer` | O(1) plus description bytes | One journal entry, at most two balance updates. |
| `ExchangeRate::convert`, `interest` | O(1) | Checked scalar arithmetic and one rounding step. |
| `invest`, `redeem` | O(1) plus name bytes | One journal entry per action, one investment record at funding. |
| `spend` | Expected amortized O(log(V+1) + h + Q(t) + text bytes) | Journal append and one ordinary tree insertion. |
| `replace_expenses` | O(purchases + log(V+1)) plus reclaiming old storage and cached views | Validates the replacement against journal entries by ID, then moves it into place while preserving ID high-water marks and advancing revision. An lvalue argument also copies the source. |
| `Book::import_expenses` | Expected O(H + S + imported description bytes) | Stages a book copy, rewrites external ledger IDs, validates and adopts the old tree. O(H + snapshot size) staging space. |
| `income_total` | O(J) | O(1) working space. Scans posting dates and account IDs, includes realized interest. |
| `spending_by_location` | Leaf query cost + O(log(V+1) + k log(G+2)) | Aggregates k matching monetary leaves. O(k+G) storage beyond query preparation. |
| `cancel` | O(1) | Clears one cursor. |
| `run_due` | O(1) + sum of posting costs over emitted occurrences | Constant calendar arithmetic per occurrence. The caller bounds the batch. |
| `Book::export_legacy`, save to an existing text book | O(file bytes + sum of S over currency trees) | Sequential serialization, one tree snapshot at a time, plus one temporary file. |
| `Book::read`, load a text book | Expected O(file bytes + (U+J) log(V+1) + total tree nodes, references and tags) | Reads records, replays the journal, validates tree links and recurrence dates. Memory includes the restored book and temporary tree/index data. |
| Copy book | O(H) | Copies authoritative records. Tree copies omit derived query caches. |

When adding a public operation, update its declaration comment and this table or the corresponding tree table. Changes to normalization, index invalidation, output order or retention can change query costs even when method signatures stay the same.

## SQLite persistence and history

Let M be live records (including one record per child edge), V be stored record versions, D be the number of records changed by an action, and A be audit events. Add the bytes actually read, compared and written, SQLite page I/O, lock waits and synchronization. P denotes the cost to materialize the current record set into a validated `Book`. This includes ordered-map construction O(M log(M+1)), ordering child edges, and the legacy reader's journal replay and tree validation. In-memory query and mutation costs above still apply.

| Operation | Cost and behavior |
| --- | --- |
| Open, refresh, `Book::load` (SQLite) | Read the materialized live records and pay P. Working memory scales with the current book. Open also checks the schema and journal mode. |
| `edit`, `Book::save` to SQLite | The implementation copies and encodes a candidate book, compares all live records, and validates the candidate. CPU includes P, sorted tree snapshots, and O(M log(M+1)) comparisons. Persistent changes normally require O(D log(V+1)) indexed work plus changed bytes and one commit. A checkpoint every 256 levels adds O(M) version references and their index writes. |
| `undo`, `redo` | O((D+1) log(V+A+2)) indexed work, plus audit rows and a commit. These operations reuse immutable payloads. They defer rebuilding the in-memory book until a full read needs it. |
| `head`, `book` | O(1) cached. After navigation, the first `book()` pays P if the current book needs rebuilding. A session reading its former head after another writer advances uses historical reconstruction. |
| `at` | Read the closest checkpoint on the ancestry path and at most 255 subsequent revisions. Reconstruction visits the checkpoint's records and all intervening changes, reorders them by revision and applies them to ordered maps, then pays P. A single bulk revision can contain many changes. |
| `restore` | Historical reconstruction plus a comparison against the current book and O(D log(V+A+2)) indexed writes. The new revision references existing versions. |
| `history`, `actions` | Linear in returned revisions or cursor events, plus returned label bytes. |
| `receipt_history` | Indexed lookup by currency and purchase ID, then indexed joins on old/new versions. Deduplication and ordering can cost O(K log(K+1)) for K matching changes. |
| `posting_history` | Full history visits all audit rows and looks up linked versions and events. The (origin, entry ID) overload uses indexes to select one posting and its audit rows. Ordering K selected events costs up to O(K log(K+1)). |
| `import_legacy` | Legacy parse/replay plus one edit containing the imported book. |
| `verify`, `backup` | Integrity and foreign-key checks inspect database tables and indexes. Verification also reconstructs head and validates its record set. Backup additionally copies all database pages and requires space for the complete file. |

Undo storage is proportional to changed records, their payload bytes and audit references. A receipt append creates one ordered edge. Reordering many siblings changes the positions of the affected edges. Old versions and branches intentionally continue to occupy disk space.
