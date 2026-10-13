#pragma once

#include "moneta/expenses/expense_tree.hpp"

#include <cstddef>
#include <deque>

namespace moneta {

// Opt-in snapshots: call checkpoint BEFORE a user edit/batch. Routine edits
// incur no history overhead. Each entry costs O(n+stored text/tags) memory.
class ExpenseHistory {
public:
    explicit ExpenseHistory(std::size_t capacity = 20) : capacity_(capacity) {}

    // O(n log n+B+D), deterministic snapshot; D=discarded snapshot data.
    // Clears redo after a new branch and evicts entries beyond capacity.
    // A zero capacity disables checkpointing without copying the tree.
    void checkpoint(const ExpenseTree& tree);
    // O(n log n+B) to save the present, then expected O(n+references+B) restore.
    // Empty histories return false. Failed restores retain both histories.
    // ExpenseTree::restore preserves ID high-water marks across undo branches.
    bool undo(ExpenseTree& tree);
    bool redo(ExpenseTree& tree);

    // O(1).
    [[nodiscard]] bool can_undo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool can_redo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] std::size_t undo_size() const noexcept { return undo_.size(); }
    [[nodiscard]] std::size_t redo_size() const noexcept { return redo_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    // O(total discarded snapshot data); bounds both queues independently.
    void set_capacity(std::size_t capacity);
    void clear() noexcept;

private:
    std::size_t capacity_;
    std::deque<TreeSnapshot> undo_;
    std::deque<TreeSnapshot> redo_;
    void trim(std::deque<TreeSnapshot>& entries);
};

} // namespace moneta
