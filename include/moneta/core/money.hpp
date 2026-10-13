#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace moneta {

class Currency {
public:
    // Currency metadata must come from a trusted currency catalogue.
    // INR uses 2 decimal places, JPY 0, and KWD 3.
    // O(1): three code bytes and one scale byte, without string allocation.
    Currency(std::string_view code, std::uint8_t decimal_places);

    [[nodiscard]] std::string_view code() const noexcept { return {code_.data(), code_.size()}; }
    [[nodiscard]] std::uint8_t decimal_places() const noexcept { return decimal_places_; }
    bool operator==(const Currency&) const = default;

private:
    std::array<char, 3> code_;
    std::uint8_t decimal_places_;
};

class Money {
public:
    // 198 INR is Money{19800, Currency{"INR", 2}}. No floating point arithmetic.
    // O(1); currency metadata is a four-byte value, not a heap-owned string.
    Money(std::int64_t minor_units, Currency currency);

    [[nodiscard]] std::int64_t minor_units() const noexcept { return minor_units_; }
    [[nodiscard]] const Currency& currency() const noexcept { return currency_; }
    // O(1); mismatched currency/scale and integer overflow throw.
    [[nodiscard]] Money operator+(const Money& other) const;
    bool operator==(const Money&) const = default;

private:
    std::int64_t minor_units_;
    Currency currency_;
};

} // namespace moneta
