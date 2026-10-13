#pragma once

#include <compare>
#include <cstdint>

namespace moneta {

// O(1) scalar comparisons. Zero is invalid/unassigned; IDs are store-local.
template <typename Tag>
struct Identifier {
    std::uint64_t value{};
    auto operator<=>(const Identifier&) const = default;
};

using NodeId = Identifier<struct NodeTag>;
using AccountId = Identifier<struct AccountTag>;
using LedgerEntryId = Identifier<struct LedgerEntryTag>;
using LocationId = Identifier<struct LocationTag>;
using TagId = Identifier<struct ExpenseTag>;

} // namespace moneta
