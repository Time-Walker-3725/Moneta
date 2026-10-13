#pragma once

#include <chrono>
#include <stdexcept>

namespace moneta {

using Date = std::chrono::year_month_day;

class DateRange {
public:
    // O(1). Both calendar-date endpoints are inclusive.
    DateRange(Date first, Date last) : first_(first), last_(last) {
        if (!first_.ok() || !last_.ok() || first_ > last_) {
            throw std::invalid_argument("Date range requires valid, ordered endpoints");
        }
    }

    [[nodiscard]] Date first() const noexcept { return first_; }
    [[nodiscard]] Date last() const noexcept { return last_; }
    bool operator==(const DateRange&) const = default;
    [[nodiscard]] bool contains(Date date) const noexcept {
        return date.ok() && date >= first_ && date <= last_;
    }

private:
    Date first_;
    Date last_;
};

} // namespace moneta
