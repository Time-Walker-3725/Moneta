#include "book_records.hpp"

#include <iomanip>
#include <locale>
#include <sstream>

namespace moneta::storage_detail {
namespace {
template<class... Fields>
std::string fields(const Fields&... values) {
    std::ostringstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.imbue(std::locale::classic());
    bool first = true;
    auto append = [&](const auto& v) {
        if (!first) out.put(' ');
        first = false;
        out << v;
    };
    (append(values), ...);
    return std::move(out).str();
}

auto day(Date date) { return std::chrono::sys_days{date}.time_since_epoch().count(); }

template<class Id>
std::string ids(const std::vector<Id>& list) {
    auto result = std::to_string(list.size());
    for (auto id : list) result += " " + std::to_string(id.value);
    return result;
}
} // namespace

// One stable key per domain record. Child order is stored per edge so adding
// one receipt does not duplicate the parent's entire child vector in history.
BookRecords encode_book(const Book& book) {
    using enum RecordKind;
    BookRecords rows;
    std::map<std::string, Currency> currencies;
    std::uint64_t index = 0;
    for (const auto& a : book.accounts()) {
        const auto c = a.balance.currency();
        currencies.emplace(c.code(), c);
        rows[{"", account, ++index}].data = fields(std::quoted(a.name), int(a.kind),
            std::quoted(c.code()), unsigned(c.decimal_places()));
    }
    index = 0;
    for (const auto& s : book.locations()) rows[{"", location, ++index}].data = fields(std::quoted(s));
    index = 0;
    for (const auto& i : book.investments())
        rows[{"", investment, ++index}].data = fields(std::quoted(i.name), i.settlement.value,
            i.principal.minor_units(), i.annual_basis_points, day(i.start));
    index = 0;
    for (const auto& e : book.journal()) {
        rows[{"", journal, ++index}].data = fields(int(e.kind), day(e.date), e.account.value,
            e.change, e.other.value, e.other_change, e.investment.value, e.purchase.value,
            e.location.value_or(LocationId{}).value, std::quoted(e.description));
    }
    index = 0;
    for (const auto& s : book.schedules()) {
        auto row = fields(int(s.kind), s.account.value, s.amount.minor_units(), std::quoted(s.description),
            day(s.first), int(s.frequency), s.location.value_or(LocationId{}).value, int(s.next.has_value()));
        if (s.next) row += " " + std::to_string(day(*s.next));
        rows[{"", schedule, ++index}].data = std::move(row);
    }
    for (const auto& [code, c] : currencies) {
        const auto& tree = book.expenses(c);
        const auto snapshot = tree.snapshot();
        rows[{code, currency, 0}].data = fields("CURRENCY", std::quoted(code), unsigned(c.decimal_places()))
            + "\n" + fields("COUNTERS", snapshot.next_node_id, snapshot.next_tag_id);
        for (const auto& t : snapshot.tags)
            rows[{code, tag, t.id.value}].data = fields("TAG", t.id.value, std::quoted(t.name), std::quoted(t.key));
        for (const auto& n : snapshot.nodes) {
            // Only the trailing tag list is split on read. Names can contain newlines.
            rows[{code, node, n.id.value}] = {fields("NODE", n.id.value, int(n.kind), std::quoted(n.name),
                n.parent_id.value, n.amount_minor_units, n.transaction_id.value) + "\n" + ids(n.tags),
                n.transaction_id.value};
            std::size_t order = 0;
            for (const auto child : n.children)
                rows[{code, edge, child.value}] = {fields(n.id.value, order++), tree.node(child).transaction_id.value};
        }
        for (const auto& r : snapshot.expenses) {
            const auto& e = r.expense;
            rows[{code, expense, r.id.value}] = {fields("EXPENSE", r.id.value, e.amount.minor_units(),
                int(e.date.year()), unsigned(e.date.month()), unsigned(e.date.day()), e.account_id.value,
                e.ledger_entry_id.value_or(LedgerEntryId{}).value, e.location_id.value_or(LocationId{}).value,
                std::quoted(e.merchant), std::quoted(e.note)), r.id.value};
        }
        for (const auto& n : snapshot.notes)
            rows[{code, note, n.id.value}] = {fields("NOTE", n.id.value, std::quoted(n.text)), tree.node(n.id).transaction_id.value};
    }
    return rows;
}

// Reuse the validated book reader for balance replay, ownership and tree sums.
Book decode_book(const BookRecords& rows, LoadLimits limits) {
    using enum RecordKind;
    std::ostringstream out;
    out.exceptions(std::ios::badbit | std::ios::failbit);
    out.imbue(std::locale::classic());
    std::string code;
    auto range = [&](RecordKind kind) {
        return std::pair{rows.lower_bound({code, kind, 0}), rows.upper_bound({code, kind, UINT64_MAX})};
    };
    auto section = [&](const char* name, RecordKind kind) {
        auto [begin, end] = range(kind);
        out << name << ' ' << std::distance(begin, end) << '\n';
        for (auto i = begin; i != end; ++i) out << i->second.data << '\n';
    };
    out << "MONETA_BOOK 1\n";
    section("ACCOUNTS", account);
    section("LOCATIONS", location);
    section("INVESTMENTS", investment);
    section("JOURNAL", journal);
    section("SCHEDULES", schedule);
    std::vector<std::string> currencies;
    for (const auto& [key, record] : rows)
        if (key.kind == currency) currencies.push_back(key.currency);
    out << "TREES " << currencies.size() << '\n';
    for (const auto& next : currencies) {
        code = next;
        out << "MONETA_EXPENSE_TREE 1\n" << rows.at({code, currency, 0}).data << '\n';
        section("TAGS", tag);
        std::map<std::uint64_t, std::map<std::size_t, std::uint64_t>> children;
        auto [edges, edges_end] = range(edge);
        for (auto it = edges; it != edges_end; ++it) {
            std::istringstream input{it->second.data};
            input.imbue(std::locale::classic());
            std::uint64_t parent;
            std::size_t position;
            if (!(input >> parent >> position) || !children[parent].emplace(position, it->first.id).second)
                throw std::runtime_error("Invalid stored child order");
        }
        auto [nodes, end] = range(node);
        out << "NODES " << std::distance(nodes, end) << '\n';
        for (auto it = nodes; it != end; ++it) {
            const auto& data = it->second.data;
            const auto split = data.rfind('\n');
            if (split == std::string::npos) throw std::runtime_error("Invalid stored node");
            out.write(data.data(), split);
            auto list = children.find(it->first.id);
            out << ' ' << (list == children.end() ? 0 : list->second.size());
            if (list != children.end())
                for (auto [pos, id] : list->second) out << ' ' << id;
            out << ' ' << data.substr(split + 1) << '\n';
        }
        section("EXPENSES", expense);
        section("NOTES", note);
        out << "END\n";
    }
    out << "END\n";
    std::istringstream input{std::move(out).str()};
    return Book::read(input, limits);
}
} // namespace moneta::storage_detail
