#pragma once

#include "moneta/core/money.hpp"

namespace moneta {

// A settled price in minor units. For USD (2 decimals) to JPY (0),
// 150 JPY per USD is 150 target minor units per 100 source minor units.
struct ExchangeRate {
    Currency source;
    Currency target;
    std::uint32_t target_units;
    std::uint32_t source_units;

    // O(1) integer arithmetic. Round once to nearest minor unit, ties upward.
    [[nodiscard]] Money convert(Money amount) const;
};

} // namespace moneta
