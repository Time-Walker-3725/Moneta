#pragma once

#include "moneta/core/date_range.hpp"
#include "moneta/core/identifiers.hpp"
#include "moneta/core/money.hpp"

#include <optional>
#include <string>
#include <vector>

namespace moneta {

// A transaction owns accounting metadata; its split items inherit this context.
// Account/location/ledger IDs refer to separate stores, not expense-tree nodes.
struct Expense {
    Money amount;
    Date date;
    AccountId account_id;
    std::optional<LedgerEntryId> ledger_entry_id;
    std::optional<LocationId> location_id;
    std::string merchant;
    std::string note;
    std::vector<std::string> tags; // Free-form labels; preserved as supplied.
};

} // namespace moneta
