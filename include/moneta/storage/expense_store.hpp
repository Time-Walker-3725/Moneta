#pragma once

#include "moneta/expenses/expense_tree.hpp"

#include <cstddef>
#include <filesystem>
#include <iosfwd>

namespace moneta {

// Bounds apply before allocating declared record/reference arrays; quoted text
// is also bounded while reading. Increase explicitly when importing larger data.
struct LoadLimits {
    std::size_t max_nodes{2'000'000};
    std::size_t max_tags{100'000};
    std::size_t max_references{20'000'000};
    std::size_t max_text_bytes{256'000'000};
};

// Versioned UTF-8-preserving text; IDs, ordering and canonical metadata persist.
// Caches and revisions do not. save: O(n log n + B), snapshot+bytes; O(n+B)
// temporary storage. load: expected O(n+references+B), including validation.
// B includes all processed text/file bytes. Invalid/truncated data, extra data,
// or exceeded limits throw; stream failures throw std::runtime_error.
void save_tree(std::ostream& output, const ExpenseTree& tree);
[[nodiscard]] ExpenseTree load_tree(std::istream& input, LoadLimits limits = {});

// Same costs plus filesystem I/O. Save writes and closes a temporary file on
// the destination filesystem, then atomically renames it. A failed save leaves
// an existing file untouched; crash durability/fsync is not provided.
void save_tree_file(const std::filesystem::path& path, const ExpenseTree& tree);
[[nodiscard]] ExpenseTree load_tree_file(const std::filesystem::path& path,
    LoadLimits limits = {});

// Export atomic monetary leaves, never purchase+split totals together. Dates
// and account metadata belong to the owning transaction; explicit/effective
// tag columns refer to each leaf. RFC 4180 quoting preserves commas/newlines.
// O(query preparation+candidates+k*(h+A log A)+B), effective_tags per leaf;
// O(k+max row bytes) temporary storage. No filtered-tree copy is constructed.
void export_expenses_csv(std::ostream& output, const ExpenseTree& tree,
    NodeId subtree, const QueryFilter& filter = {});

} // namespace moneta
