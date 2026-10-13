#include "moneta/expenses/expense_history.hpp"

#include <utility>

namespace moneta {

void ExpenseHistory::trim(std::deque<TreeSnapshot>& entries) {
    while (entries.size() > capacity_) entries.pop_front();
}

void ExpenseHistory::checkpoint(const ExpenseTree& tree) {
    if (capacity_ == 0) return;
    undo_.push_back(tree.snapshot());
    redo_.clear();
    trim(undo_);
}

bool ExpenseHistory::undo(ExpenseTree& tree) {
    if (undo_.empty()) return false;
    // Allocate the opposite entry before changing the tree. On failure,
    // neither the original entry nor the tree's atomic restore is consumed.
    redo_.push_back(tree.snapshot());
    try {
        tree.restore(undo_.back());
    } catch (...) {
        redo_.pop_back();
        throw;
    }
    undo_.pop_back();
    trim(redo_);
    return true;
}

bool ExpenseHistory::redo(ExpenseTree& tree) {
    if (redo_.empty()) return false;
    undo_.push_back(tree.snapshot());
    try {
        tree.restore(redo_.back());
    } catch (...) {
        undo_.pop_back();
        throw;
    }
    redo_.pop_back();
    trim(undo_);
    return true;
}

void ExpenseHistory::set_capacity(std::size_t capacity) {
    capacity_ = capacity;
    trim(undo_);
    trim(redo_);
}

void ExpenseHistory::clear() noexcept {
    undo_.clear();
    redo_.clear();
}

} // namespace moneta
