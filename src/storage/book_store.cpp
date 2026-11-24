#include "moneta/accounting/book.hpp"
#include "file_io.hpp"
#include "book_records.hpp"
#include "moneta/storage/book_database.hpp"

namespace moneta {
namespace {
using namespace storage_detail;
bool sqlite_file(const std::filesystem::path& path) {
    std::ifstream in{path, std::ios::binary};
    char header[16];
    return in.read(header,sizeof(header)) && std::string_view(header,sizeof(header)) ==
        std::string_view("SQLite format 3\0",16);
}

auto serial_day(Date date) {
    return std::chrono::sys_days{date}.time_since_epoch().count();
}

Date read_day(Reader& reader) {
    using namespace std::chrono;
    const auto value = reader.number<std::int64_t>();
    if (value < serial_day(year::min()/January/1) || value > serial_day(year::max()/December/31))
        invalid_file();
    return Date{sys_days{days{static_cast<days::rep>(value)}}};
}

std::optional<LocationId> read_location(Reader& reader) {
    const auto id = reader.number<std::uint64_t>();
    return id ? std::optional{LocationId{id}} : std::nullopt;
}

LedgerEntry read_posting(Reader& reader) {
    const auto kind = static_cast<EntryKind>(reader.number<int>());
    return {kind, read_day(reader), AccountId{reader.number<std::uint64_t>()},
        reader.number<std::int64_t>(), AccountId{reader.number<std::uint64_t>()},
        reader.number<std::int64_t>(), InvestmentId{reader.number<std::uint64_t>()},
        NodeId{reader.number<std::uint64_t>()}, read_location(reader), reader.quoted()};
}

bool is_occurrence(const Schedule& s, Date date) {
    using namespace std::chrono;
    if (date < s.first) return false;
    if (s.frequency == Frequency::daily) return true;
    if (s.frequency == Frequency::weekly)
        return (sys_days{date} - sys_days{s.first}).count() % 7 == 0;
    const auto month = date.year()/date.month();
    return (s.frequency != Frequency::yearly || date.month() == s.first.month())
        && date.day() == std::min(s.first.day(), (month/last).day());
}
} // namespace

LedgerEntry storage_detail::decode_posting(const std::string& text, LoadLimits limits) {
    std::istringstream in{text};
    StreamFormat formatting{in};
    Reader reader{in, limits};
    return read_posting(reader);
}

void Book::save(const std::filesystem::path& path) const {
    if (std::filesystem::exists(path) && std::filesystem::file_size(path) && !sqlite_file(path)) {
        std::ifstream in{path,std::ios::binary};
        StreamFormat formatting{in};
        Reader header{in,{}};
        header.expect("MONETA_BOOK");
        if (header.number<unsigned>() != 1)
            throw std::runtime_error("Destination has an unrecognized book format");
        export_legacy(path);
        return;
    }
    BookDatabase db{path};
    db.save_snapshot(*this);
}

void Book::export_legacy(const std::filesystem::path& path) const {
    if (sqlite_file(path)) throw std::invalid_argument("Choose a separate file for legacy export");
    atomic_write(path, [&](std::ostream& out) {
        StreamFormat formatting{out};
        out << "MONETA_BOOK 1\nACCOUNTS " << accounts_.size() << '\n';
        for (const auto& a : accounts_) {
            const auto c = a.balance.currency();
            out << std::quoted(a.name) << ' ' << static_cast<int>(a.kind) << ' '
                << std::quoted(c.code()) << ' ' << unsigned(c.decimal_places()) << '\n';
        }
        out << "LOCATIONS " << locations_.size() << '\n';
        for (const auto& name : locations_) out << std::quoted(name) << '\n';
        out << "INVESTMENTS " << investments_.size() << '\n';
        for (const auto& i : investments_) {
            out << std::quoted(i.name) << ' ' << i.settlement.value << ' ' << i.principal.minor_units()
                << ' ' << i.annual_basis_points << ' ' << serial_day(i.start) << '\n';
        }
        out << "JOURNAL " << entries_.size() << '\n';
        for (const auto& e : entries_) {
            out << static_cast<int>(e.kind) << ' ' << serial_day(e.date) << ' ' << e.account.value
                << ' ' << e.change << ' ' << e.other.value << ' ' << e.other_change << ' '
                << e.investment.value << ' ' << e.purchase.value << ' '
                << e.location.value_or(LocationId{}).value << ' ' << std::quoted(e.description) << '\n';
        }
        out << "SCHEDULES " << schedules_.size() << '\n';
        for (const auto& s : schedules_) {
            out << static_cast<int>(s.kind) << ' ' << s.account.value << ' ' << s.amount.minor_units()
                << ' ' << std::quoted(s.description) << ' ' << serial_day(s.first) << ' '
                << static_cast<int>(s.frequency) << ' ' << s.location.value_or(LocationId{}).value
                << ' ' << static_cast<unsigned>(s.next.has_value());
            if (s.next) out << ' ' << serial_day(*s.next);
            out << '\n';
        }
        out << "TREES " << trees_.size() << '\n';
        for (const auto& [code, tree] : trees_) save_tree(out, tree);
        out << "END\n";
    });
}

Book Book::load(const std::filesystem::path& path, LoadLimits limits) {
    if (sqlite_file(path)) return BookDatabase::read_snapshot(path,limits);
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("Cannot open Moneta book");
    return read(input, limits);
}

Book Book::read(std::istream& input, LoadLimits limits) {
    StreamFormat formatting{input};
    Reader reader{input, limits};
    reader.expect("MONETA_BOOK");
    if (reader.number<unsigned>() != 1) throw std::runtime_error("Unsupported Moneta book version");
    auto section = [&](std::string_view name) {
        reader.expect(name);
        return reader.count(limits.max_nodes);
    };
    Book book;
    for (auto count = section("ACCOUNTS"); count; --count) {
        auto name = reader.quoted();
        const auto kind = static_cast<AccountKind>(reader.number<int>());
        const auto code = reader.quoted();
        const auto scale = static_cast<std::uint8_t>(reader.count(9));
        book.open_account(std::move(name), Currency{code, scale}, kind);
    }
    for (auto count = section("LOCATIONS"); count; --count) book.add_location(reader.quoted());
    for (auto count = section("INVESTMENTS"); count; --count) {
        auto name = reader.quoted();
        const AccountId cash{reader.number<std::uint64_t>()};
        const auto amount = reader.number<std::int64_t>();
        const auto rate = reader.number<std::uint32_t>();
        const auto date = read_day(reader);
        if (amount <= 0 || name.find_first_not_of(" \t\n\r\f\v") == std::string::npos) invalid_file();
        book.investments_.push_back({std::move(name), cash, Money{amount, book.account(cash).balance.currency()},
            rate, date, {}});
    }
    // Every investment has exactly one funding, in ID order, before any redemption.
    std::size_t funded = 0;
    std::map<std::string, std::size_t, std::less<>> purchase_counts;
    for (auto count = section("JOURNAL"); count; --count) {
        auto record = read_posting(reader);
        const auto kind = record.kind;
        if (kind == EntryKind::invest && record.investment.value != ++funded) invalid_file();
        if (kind == EntryKind::redeem && record.investment.value > funded) invalid_file();
        if (kind == EntryKind::expense)
            ++purchase_counts[std::string(book.account(record.account).balance.currency().code())];
        book.post(std::move(record), {}, {}, true);
    }
    if (funded != book.investments_.size()) invalid_file();
    for (auto count = section("SCHEDULES"); count; --count) {
        const auto kind = static_cast<EntryKind>(reader.number<int>());
        const AccountId cash{reader.number<std::uint64_t>()};
        const Money amount{reader.number<std::int64_t>(), book.account(cash).balance.currency()};
        auto text = reader.quoted();
        const auto first = read_day(reader);
        const auto frequency = static_cast<Frequency>(reader.number<int>());
        const auto id = book.add_schedule(kind, cash, amount, std::move(text), first, frequency, read_location(reader));
        if (reader.count(1)) {
            const auto next = read_day(reader);
            if (!is_occurrence(book.schedule(id), next)) invalid_file();
            book.schedules_.back().next = next;
        } else book.cancel(id);
    }
    const auto currencies = section("TREES");
    if (currencies != book.trees_.size()) invalid_file();
    // Writer emits currencies in map order. Reject duplicates or missing trees.
    for (auto& [code, tree] : book.trees_) {
        auto loaded = read_tree_record(reader, limits);
        if (tree.currency() != loaded.currency()) invalid_file();
        book.validate_tree(loaded, purchase_counts[code]);
        tree = std::move(loaded);
    }
    reader.finish();
    return book;
}
} // namespace moneta
