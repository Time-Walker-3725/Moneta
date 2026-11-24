# SQLite books

Open a local file with `BookDatabase` and call `edit(label, callback)` once per user action. The callback receives a working `Book` and can use the ordinary accounting and expense-tree APIs. All writes from that callback commit together. A successful no-op keeps the current revision. An exception escaping the callback rolls back the action. Keep external side effects outside the callback.

A handle serves a consistent local view. Use one handle per thread and `refresh()` to see another writer's commits. SQLite serializes writers. A writer whose view is stale receives an error before its callback runs, even if the other writer undid its changes and returned to the same revision. Refresh and reevaluate the intended action. Book references borrowed from a handle should be reacquired after edits, navigation or refresh.

## Receipts and reversals

`book().journal()` and expense reports describe the active revision. `posting_history()` exposes the append-only financial audit, including reversals. Each row carries the original posting, its originating revision, the cursor event and a direction. Apply that direction to both signed cash legs. Undoing a payment restores its cash balance and removes it from active spending. Undoing a split keeps the original cash posting and restores its former receipt structure. Redo applies the corresponding changes once.

```cpp
auto edits = db.receipt_history(usd, purchase_id);
auto audit = db.posting_history(original_revision, ledger_entry_id);
auto actions = db.actions();   // UTC timestamp and active revision after each action.
auto versions = db.history();  // Includes parents, labels and restore sources.
```

Book-local numeric IDs can be reused on different branches. Receipt lookup includes all branches using that currency code and purchase ID. The pair of originating revision and ledger entry ID identifies an individual posting across branches.

## Revisions and branches

`undo()` selects the current revision's parent. `redo()` follows its preferred child. Both return false at the applicable history boundary. The choice survives closing the file. An edit after Undo becomes the preferred branch. The former branch stays in `history()`. Inspect it with `at(id)`, which returns an independent book. Calling `restore(id, "reason")` appends a revision based on the chosen contents. Undoing the restore returns to its previous active state.

The database stores immutable versions of accounts, locations, postings, investments, schedules and receipt records. A revision records old and new version IDs for changed records. Child positions have individual records to keep receipt appends small. A materialized set identifies the live records. Every 256 levels, a checkpoint stores a set of version IDs to bound the number of revisions replayed for a historical read. Current implementation costs for diffing edits and materializing reads are in the [operation reference](operations.md).

## Existing files and backups

```cpp
BookDatabase db{"migrated.sqlite"};     // Create a fresh destination.
db.import_legacy("old-household.moneta");
db.verify();
db.backup("backups/household-001.sqlite"); // The parent directory must exist.
```

Import parses and validates the original version-1 text book, then commits it as one undoable action. The original file remains available. Standalone expense-tree files can still be loaded with `load_tree_file` and imported with `Book::import_expenses` inside a database edit.

`Book::save` creates a SQLite database for a new filename. A detached book loaded from SQLite can save back while its originating database state is current. An existing version-1 text destination keeps its format. `Book::export_legacy` explicitly writes a text snapshot of the active state. History and backups use the SQLite format.

`backup` uses the SQLite online backup API, verifies the copy, and publishes it under a new filename. Use a destination filesystem supporting hard links. The temporary file is created on that same filesystem. Open the backup normally to recover its book, branches, cursor and financial audit. Schedule separate backups according to the amount of data loss you can tolerate.

SQLite foreign keys and full synchronous journaling are enabled. The transaction includes the materialized book, version deltas, audit entries and navigation cursor. The database header carries an application ID and schema version. Unsupported databases are rejected. `verify()` checks SQLite page/index integrity, foreign keys and the active revision's agreement with its stored history and accounting records. Recovery after device loss requires an independent backup. Power-loss durability depends on the filesystem and storage device honoring synchronization.
