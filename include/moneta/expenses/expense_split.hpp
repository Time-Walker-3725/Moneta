#pragma once

#include "moneta/core/money.hpp"
#include "moneta/core/identifiers.hpp"

#include <string>
#include <vector>

namespace moneta {

// Input for splitting or replacing the items of a transaction.
struct ExpenseSplit {
    std::string name;
    Money amount;
    std::string note;
    std::vector<TagId> tags;
};

} // namespace moneta
