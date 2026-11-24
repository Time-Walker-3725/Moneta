#pragma once

#include "moneta/accounting/book.hpp"

namespace moneta::storage_detail {

enum class RecordKind { account, location, investment, journal, schedule, currency, tag, node, edge, expense, note };
struct RecordKey {
    std::string currency;
    RecordKind kind;
    std::uint64_t id;
    auto operator<=>(const RecordKey&) const = default;
};

struct BookRecord {
    std::string data;
    std::uint64_t purchase{};
    bool operator==(const BookRecord&) const = default;
};

using BookRecords = std::map<RecordKey, BookRecord>;
BookRecords encode_book(const Book& book);
Book decode_book(const BookRecords& records, LoadLimits limits);
LedgerEntry decode_posting(const std::string& text, LoadLimits limits);
} // namespace moneta::storage_detail
