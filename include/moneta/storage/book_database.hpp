#pragma once

#include "moneta/accounting/book.hpp"

#include <functional>
#include <memory>

namespace moneta {

using RevisionId = std::int64_t;

struct BookRevision {
    RevisionId id;
    RevisionId parent; // Zero for the initial empty book.
    RevisionId restored_from; // Zero for an ordinary edit.
    std::string description;
    std::string created_utc;
};

// A row in the durable financial audit. origin + entry_id uniquely identifies
// an original posting, including across branches that reuse book-local IDs.
// Apply direction (+1 or -1) to both legs. Retaining the original signed amount
// also represents reversal of an INT64_MIN opening balance without overflow.
struct PostingEvent {
    std::int64_t event;
    std::string created_utc;
    RevisionId origin;
    LedgerEntryId entry_id;
    LedgerEntry posting;
    int direction;
    std::string action; // edit, restore, undo or redo.
    RevisionId active_revision;
};

struct BookAction {
    std::int64_t event;
    RevisionId active_revision;
    std::string action;
    std::string created_utc;
};

class BookDatabase {
public:
    // Opens or creates a file. An existing unrelated database is rejected.
    explicit BookDatabase(const std::filesystem::path& path, LoadLimits limits = {});
    ~BookDatabase();
    BookDatabase(const BookDatabase&) = delete;
    BookDatabase& operator=(const BookDatabase&) = delete;

    // Navigation updates SQLite immediately. The first full book read after
    // navigation materializes and validates the selected revision on demand.
    const Book& book() const;
    RevisionId head() const noexcept;
    void refresh();

    // Exactly one SQLite transaction per successful callback. Borrowed book
    // references expire on edit, navigation or refresh. No-op edits retain head.
    // Diffing and domain validation currently visit the whole book; disk writes
    // are proportional to changed records except at reference checkpoints.
    RevisionId edit(std::string description, const std::function<void(Book&)>& action);
    // Work proportional to changed records times index lookup cost.
    bool undo();
    bool redo();
    RevisionId restore(RevisionId revision, std::string description);
    Book at(RevisionId revision) const;
    std::vector<BookRevision> history() const;
    std::vector<BookRevision> receipt_history(Currency currency, NodeId purchase) const;
    std::vector<BookAction> actions() const;
    std::vector<PostingEvent> posting_history() const;
    // Indexed lookup of one posting's applications and reversals across branches.
    std::vector<PostingEvent> posting_history(RevisionId origin, LedgerEntryId entry) const;
    // Explicit database, foreign-key and live-state verification in one read transaction.
    void verify() const;

    // Imports a version-1 text book into a fresh database in a single revision.
    RevisionId import_legacy(const std::filesystem::path& source);
    // Online SQLite backup, including other branches and the redo cursor.
    // The destination must be new.
    void backup(const std::filesystem::path& destination) const;

private:
    friend class Book;
    void save_snapshot(const Book& book);
    static Book read_snapshot(const std::filesystem::path& path, LoadLimits limits);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moneta
