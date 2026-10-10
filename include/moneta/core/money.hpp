#pragma once

#include <cstdint>
#include <string>

namespace moneta {

class Currency {
public:
    // Currency metadata must come from a trusted currency catalogue.
    // INR uses 2 decimal places, JPY 0, and KWD 3.
    Currency(std::string code, std::uint8_t decimal_places);

    [[nodiscard]] const std::string& code() const noexcept { return code_; }
    [[nodiscard]] std::uint8_t decimal_places() const noexcept { return decimal_places_; }
    bool operator==(const Currency&) const = default;

private:
    std::string code_;
    std::uint8_t decimal_places_;
};

class Money {
public:
    // 198 INR is Money{19800, Currency{"INR", 2}}. No floating point arithmetic.
    Money(std::int64_t minor_units, Currency currency);

    [[nodiscard]] std::int64_t minor_units() const noexcept { return minor_units_; }
    [[nodiscard]] const Currency& currency() const noexcept { return currency_; }
    [[nodiscard]] Money operator+(const Money& other) const;
    bool operator==(const Money&) const = default;

private:
    std::int64_t minor_units_;
    Currency currency_;
};

} // namespace moneta
