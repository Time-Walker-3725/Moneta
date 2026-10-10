#pragma once

#include "moneta/core/money.hpp"

#include <string>

namespace moneta {

// Input for splitting or replacing the items of a transaction.
struct ExpenseSplit {
    std::string name;
    Money amount;
    std::string note;
};

} // namespace moneta
