# Operations and complexity

These bounds describe the implemented backend. Hash-map/set costs are expected bounds, with linear worst cases under adverse collisions. Ordered category trees can have height O(n); their height is not automatically O(log n). Text processing, copying returned records, allocation, and storage reclamation also take work.

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
| `root_id`, `currency`, `size`, `revision`, `tag_registry` | O(1) | Return scalar values/references. |
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
| `rename_tag` / registry `rename` | Expected O(name length) | Stable ID; collisions rejected. |
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
| `matching_leaves` | O(m log m + r + c + k) | Scans covered candidates; IDs follow tree order. |
| `transactions` | Same as `matching_leaves` | Deduplicates contiguous transaction owners without a hash set; partial items return their owning purchase. |
| `total_by_tag`, `transactions_by_tag` | Name resolution plus corresponding query | Unknown names produce zero/empty; blank query names throw. |
| `search` | O(s + B × P), worst case | Case-sensitive substring search across names/notes/merchant; empty text matches all nodes. |

Add `Q(u)` to normalized-filter query bounds and cold build costs when applicable. For no tag selection, range preparation is constant; for date totals, the O(log L) term is the date-bound search. The implementation can shortcut empty coverage before constructing date indexes.

A combined total can scan O(L) candidates in the worst case, in addition to sorting tag roots. Listing `k` records necessarily costs at least Ω(k). Child slices support pagination; transaction/leaf query methods return complete ID lists rather than promising paginated logarithmic retrieval.

## Filtered views and cache

| Public operation | Time | Space/behavior |
| --- | --- | --- |
| `filter_tree`, miss | O(Q(u) + m log m + r + c + s), plus cold O(n) index | O(s + matched leaves + k) workspace/output; scans source scope to roll totals up and emit rows. |
| `filter_tree`, hit | O(Q(u) + E × (u + 1)) | Returns the same shared immutable view; small bounded LRU comparison/movement. |
| `set_filter_cache_limits` | O(E + K) | Enforces entry/row budgets and reclaims unreferenced evictions. |
| `clear_filter_cache` | O(E + K) | Callers retaining shared pointers keep those rows alive. |

A full-root view is O(n) worst case for fixed filter/tag counts, even if few rows survive. Views store IDs, parent row indices and amounts, not copied metadata. Oversize results bypass caching. Logical revision invalidation is O(1); old index/view reclamation can occur later and add linear work to replacement/eviction. A bounded cache hit is inexpensive but not literally O(1) with arbitrarily large tag filters or cache capacity.

## Snapshots, persistence, CSV, and history

| Public operation | Time | Space/behavior |
| --- | --- | --- |
| `snapshot` | `S` | Deterministic ID sorting; copies authoritative data, O(n + T + R + B) storage. |
| `from_snapshot` | Expected O(n + T + R + B) | Validates graph, parents, ownership, sorted tags, amounts, and counters; rebuilds explicit postings. |
| Tree `restore` | Same validation plus old-storage reclamation | Atomic replacement; preserves node/tag high-water marks and advances revision. |
| `save_tree`, `save_tree_file` | `S` + output bytes | Snapshot plus versioned serialization; file variant adds filesystem I/O. |
| `load_tree`, `load_tree_file` | Expected O(n + T + R + B) | Bounded parsing and validation; file variant adds filesystem I/O. |
| `export_expenses_csv` | Query preparation + O(c + k(h + A log A) + output bytes) | Effective tags computed per matching leaf; O(k + maximum row bytes) extra storage. |
| `ExpenseHistory` constructor | O(1) | Opt-in queue capacity; default 20. |
| History `checkpoint` | `S` + discarded snapshot data | Saves state before an edit, clears redo, evicts past capacity. Capacity zero does no copy. |
| History `undo`, `redo` | `S` + expected O(n + T + R + B), plus discarded data | Saves current state and validates/restores target; empty queue returns false. |
| History `can_undo`, `can_redo`, queue sizes, `capacity` | O(1) | Scalar/queue access. |
| History `set_capacity`, `clear` | O(discarded snapshot data) | Bounds undo and redo queues independently. |

Snapshots/serialization/history copy strings and references; they are not inexpensive O(1) backups. Each history entry contains a full tree, with undo and redo queues each bounded by capacity. Ordinary edits do not checkpoint automatically. Use a small capacity or disable history when storage is a concern.

Persistence validates configurable `LoadLimits` before large declared allocations. Stream/file formats preserve source records, not query caches. File replacement is atomic on the destination filesystem, but crash durability through `fsync` is not promised. CSV is a monetary-leaf report with quoted fields and length-prefixed tag-name columns; it is not a tree round-trip format.

`Currency`, `Money`, typed-ID comparisons, and `DateRange` checks use bounded scalar operations; money addition detects currency/scale mismatches and 64-bit overflow. A tree uses one currency and valid calendar dates, with timezone conversion left to callers.

These are operation counts, not wall-time guarantees. Run the [optional benchmark](../README.md#optional-benchmark) with Release optimization and representative data. Cold/warm behavior, allocation/storage layout, OS scheduling, disk I/O and network work materially affect measured latency.
