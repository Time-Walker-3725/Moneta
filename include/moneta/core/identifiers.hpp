#pragma once

#include <compare>
#include <cstdint>

namespace moneta {

// Zero is reserved for an invalid/unassigned ID. IDs are local to their store.
template <typename Tag>
struct Identifier {
    std::uint64_t value{};
    auto operator<=>(const Identifier&) const = default;
};

using NodeId = Identifier<struct NodeTag>;
using AccountId = Identifier<struct AccountTag>;
using LedgerEntryId = Identifier<struct LedgerEntryTag>;
using LocationId = Identifier<struct LocationTag>;

} // namespace moneta
