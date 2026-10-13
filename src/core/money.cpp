#include "moneta/core/money.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace moneta {

Currency::Currency(std::string_view code, std::uint8_t decimal_places)
    : decimal_places_(decimal_places) {
    if (code.size() != 3 || !std::all_of(code.begin(), code.end(), [](char c) {
            return c >= 'A' && c <= 'Z';
        }) || decimal_places_ > 9) {
        throw std::invalid_argument("Currency requires a three-letter uppercase code and 0..9 decimal places");
    }
    std::copy(code.begin(), code.end(), code_.begin());
}

Money::Money(std::int64_t minor_units, Currency currency)
    : minor_units_(minor_units), currency_(std::move(currency)) {}

Money Money::operator+(const Money& other) const {
    if (currency_ != other.currency_) {
        throw std::invalid_argument("Cannot add different currencies or currency scales");
    }
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    if ((other.minor_units_ > 0 && minor_units_ > maximum - other.minor_units_) ||
        (other.minor_units_ < 0 && minor_units_ < minimum - other.minor_units_)) {
        throw std::overflow_error("Money addition exceeds the 64-bit range");
    }
    return Money{minor_units_ + other.minor_units_, currency_};
}

} // namespace moneta
