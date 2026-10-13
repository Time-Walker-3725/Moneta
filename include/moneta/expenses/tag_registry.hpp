#pragma once

#include "moneta/core/identifiers.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace moneta {

struct TagDefinition {
    TagId id;
    std::string name; // Display name; case is preserved.
    std::string key;  // Normalized lookup name.
};

// Stable IDs allow nodes to store small sorted TagId vectors instead of names.
// Hash lookups have expected O(1) cost and O(t) worst case for t definitions.
class TagRegistry {
public:
    // Expected O(L), where L is the name length. Existing names keep their ID.
    // Throws invalid_argument for blank names and overflow_error at ID exhaustion.
    // Blank names and repeated leading # markers are rejected.
    TagId create(std::string_view name);

    // Expected O(L); blank or unknown names return nullopt.
    std::optional<TagId> find(std::string_view name) const;

    // Expected O(1); throws out_of_range for unknown IDs.
    const TagDefinition& get(TagId id) const;

    // Expected O(L); preserves ID and rejects names owned by another tag.
    void rename(TagId id, std::string_view name);

    // Expected O(L) for the stored name; erased IDs are never reused.
    // Nodes referencing this ID must be updated by their owning store first.
    void erase(TagId id);

    // O(t log t + B), where B is the total copied name/key bytes; ID order.
    std::vector<TagDefinition> definitions() const;

    // O(1). The next available ID is always above every allocated live ID.
    std::uint64_t next_id() const noexcept { return next_id_; }

    // Expected O(t + B); validates IDs, normalized keys and the next ID.
    // Replaces the registry only after all validation/allocation succeeds.
    void restore(std::vector<TagDefinition> definitions, std::uint64_t next_id);

    // O(L). Trims ASCII whitespace, removes one leading #, trims again,
    // and lowercases ASCII letters. Other bytes are preserved.
    static std::string normalize(std::string_view name);

    // O(1).
    std::size_t size() const noexcept { return definitions_.size(); }

private:
    friend class ExpenseTree; // Restore preserves allocation high-water in O(1).
    std::unordered_map<std::uint64_t, TagDefinition> definitions_;
    std::unordered_map<std::string, TagId> ids_by_key_;
    std::uint64_t next_id_{1};
};

} // namespace moneta
