#include "moneta/accounting/book.hpp"

#include "moneta/accounting/exchange_rate.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace moneta {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}

void named(std::string_view name) {
    require(std::any_of(name.begin(), name.end(), [](unsigned char c) { return !std::isspace(c); }),
        "Name must contain a non-space character");
}

template<class Records, class Id>
auto& lookup(Records& records, Id id) {
    if (id.value == 0 || id.value > records.size()) throw std::out_of_range("Unknown book ID");
    return records[static_cast<std::size_t>(id.value - 1)];
}

std::int64_t positive(Money amount, const Account& account) {
    require(amount.currency() == account.balance.currency() && amount.minor_units() > 0,
        "Amount must be positive and match account currency and scale");
    return amount.minor_units();
}

void valid_balance(AccountKind kind, Money balance) {
    require(kind != AccountKind::receivable || balance.minor_units() >= 0, "Receivable overpaid");
    require(kind != AccountKind::payable || balance.minor_units() <= 0, "Payable overpaid");
}

LedgerEntry movement(EntryKind kind, Date date, AccountId cash, std::int64_t change) {
    return {kind, date, cash, change, {}, 0, {}, {}, {}, {}};
}

// O(1). Split the product before multiplying to retain cent precision and
// detect overflow without floating point or a compiler-specific wide integer.
std::int64_t scaled_amount(std::uint64_t amount, std::uint64_t factor, std::uint32_t divisor) {
    const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto whole = amount / divisor;
    if (whole && factor > maximum / whole) throw std::overflow_error("Amount exceeds Money range");
    const auto rem = amount % divisor;
    // Each remainder is < 2^32. The rounded fraction is at most factor.
    const auto fraction = rem * (factor / divisor)
        + (rem * (factor % divisor) + divisor / 2) / divisor;
    if (fraction > maximum - whole * factor) throw std::overflow_error("Amount exceeds Money range");
    return static_cast<std::int64_t>(whole * factor + fraction);
}

std::optional<Date> following(const Schedule& schedule, Date current) {
    using namespace std::chrono;
    if (schedule.frequency == Frequency::daily || schedule.frequency == Frequency::weekly) {
        const auto next = sys_days{current} + days{schedule.frequency == Frequency::daily ? 1 : 7};
        if (next > sys_days{year::max()/December/31}) return {};
        return Date{next};
    }
    const int months_to_add = schedule.frequency == Frequency::monthly ? 1 : 12;
    auto month = current.year()/current.month() + months{months_to_add};
    if (month.year() < current.year() || !month.ok()) return {};
    const auto last_day = (month/last).day();
    return month/std::min(schedule.first.day(), last_day);
}
} // namespace

Money ExchangeRate::convert(Money amount) const {
    require(amount.currency() == source && amount.minor_units() >= 0, "Invalid source amount");
    require(target_units && source_units, "Exchange rate legs must be positive");
    return Money{scaled_amount(amount.minor_units(), target_units, source_units), target};
}

const Account& Book::account(AccountId id) const { return lookup(accounts_, id); }
const Investment& Book::investment(InvestmentId id) const { return lookup(investments_, id); }
const Schedule& Book::schedule(ScheduleId id) const { return lookup(schedules_, id); }
const std::string& Book::location(LocationId id) const { return lookup(locations_, id); }
const LedgerEntry& Book::entry(LedgerEntryId id) const { return lookup(entries_, id); }

const ExpenseTree& Book::expenses(Currency currency) const {
    const auto it = trees_.find(currency.code());
    if (it == trees_.end() || it->second.currency() != currency)
        throw std::out_of_range("Unknown book currency or scale");
    return it->second;
}

AccountId Book::open_account(std::string name, Currency currency, AccountKind kind) {
    named(name);
    require(kind == AccountKind::cash || kind == AccountKind::receivable || kind == AccountKind::payable,
        "Invalid account kind");
    auto [tree, inserted] = trees_.try_emplace(std::string(currency.code()), currency);
    require(tree->second.currency() == currency, "Conflicting scale for currency");
    try {
        accounts_.push_back({std::move(name), kind, Money{0, currency}});
    } catch (...) {
        if (inserted) trees_.erase(tree);
        throw;
    }
    return AccountId{accounts_.size()};
}

LocationId Book::add_location(std::string name) {
    named(name);
    locations_.push_back(std::move(name));
    return LocationId{locations_.size()};
}

// Validate both legs before appending. Only scalar, nonthrowing updates follow
// a successful journal/tree append. Principal and transfers never count as income.
LedgerEntryId Book::post(LedgerEntry record, NodeId parent, std::vector<TagId> tags, bool replay) {
    require(record.date.ok(), "Invalid posting date");
    auto& first = lookup(accounts_, record.account);
    const auto currency = first.balance.currency();
    Account* second = record.other.value ? &lookup(accounts_, record.other) : nullptr;
    const bool paired = record.kind == EntryKind::transfer || record.kind == EntryKind::principal;
    require(paired == (second != nullptr) && (paired || record.other_change == 0), "Invalid cash legs");
    require(record.kind == EntryKind::principal || record.kind == EntryKind::opening ||
        first.kind == AccountKind::cash, "Cash account required");
    if (record.location) {
        (void)location(*record.location);
        require(record.kind == EntryKind::expense, "Location requires an expense");
    }
    require((record.investment.value != 0) ==
        (record.kind == EntryKind::invest || record.kind == EntryKind::redeem), "Invalid investment link");
    const bool needs_purchase_id = replay && record.kind == EntryKind::expense;
    require((record.purchase.value != 0) == needs_purchase_id,
        "Invalid purchase link");
    Investment* holding = nullptr;
    switch (record.kind) {
    case EntryKind::opening:
        require(!first.last_entry.value, "Opening must be the first account activity");
        break;
    case EntryKind::expense:
    case EntryKind::income:
        named(record.description);
        require(record.kind == EntryKind::income ? record.change > 0 : record.change <= 0, "Invalid amount");
        break;
    case EntryKind::transfer:
    case EntryKind::principal:
        require(second != &first && record.change < 0 && record.other_change > 0, "Invalid transfer");
        if (record.kind == EntryKind::principal) {
            require((first.kind == AccountKind::cash) != (second->kind == AccountKind::cash)
                && currency == second->balance.currency(), "Principal requires cash and matching obligation");
        } else {
            require(second->kind == AccountKind::cash, "Transfer requires two cash accounts");
        }
        if (currency == second->balance.currency())
            require(record.change == -record.other_change, "Same-currency legs must balance");
        break;
    case EntryKind::invest:
    case EntryKind::redeem: {
        holding = &lookup(investments_, record.investment);
        require(holding->settlement == record.account && !holding->redeemed, "Invalid investment settlement");
        if (record.kind == EntryKind::invest) {
            require(record.date == holding->start && record.change == -holding->principal.minor_units(),
                "Invalid investment funding");
        } else {
            const auto proceeds = holding->principal + interest(record.investment, record.date);
            require(record.change == proceeds.minor_units(), "Invalid redemption proceeds");
        }
        break;
    }
    default: throw std::invalid_argument("Unknown ledger entry kind");
    }
    require(record.change != std::numeric_limits<std::int64_t>::min() || record.kind == EntryKind::opening,
        "Unrepresentable posting magnitude");
    const Money balance = first.balance + Money{record.change, currency};
    valid_balance(first.kind, balance);
    auto other_balance = second ? second->balance + Money{record.other_change, second->balance.currency()} : balance;
    if (second) valid_balance(second->kind, other_balance);
    const LedgerEntryId id{entries_.size() + 1};
    entries_.push_back(std::move(record));
    auto& saved = entries_.back();
    if (saved.kind == EntryKind::expense && !replay) {
        try {
            auto& tree = trees_.at(std::string(currency.code()));
            saved.purchase = tree.add_expense(parent, saved.description,
                {Money{-saved.change, currency}, saved.date, saved.account, id, saved.location, {}, {}},
                std::move(tags));
        } catch (...) { entries_.pop_back(); throw; }
    }
    first.balance = balance;
    first.last_entry = id;
    if (second) {
        second->balance = other_balance;
        second->last_entry = id;
    }
    if (saved.kind == EntryKind::redeem) holding->redeemed = saved.date;
    return id;
}

LedgerEntryId Book::opening(AccountId id, Money amount, Date date) {
    require(account(id).balance.currency() == amount.currency(), "Opening currency mismatch");
    return post(movement(EntryKind::opening, date, id, amount.minor_units()));
}

LedgerEntryId Book::income(AccountId cash, Money amount, Date date, std::string description) {
    auto record = movement(EntryKind::income, date, cash, positive(amount, account(cash)));
    record.description = std::move(description);
    return post(std::move(record));
}

LedgerEntryId Book::spend(AccountId cash, Money amount, Date date, std::string description,
    std::optional<LocationId> location, NodeId parent, std::vector<TagId> tags) {
    require(amount.currency() == account(cash).balance.currency() && amount.minor_units() >= 0,
        "Invalid expense amount or currency");
    auto record = movement(EntryKind::expense, date, cash, -amount.minor_units());
    record.description = std::move(description);
    record.location = location;
    return post(std::move(record), parent, std::move(tags));
}

LedgerEntryId Book::transfer(AccountId from, AccountId to, Money paid, Money received, Date date) {
    auto record = movement(EntryKind::transfer, date, from, -positive(paid, account(from)));
    record.other = to;
    record.other_change = positive(received, account(to));
    return post(std::move(record));
}

LedgerEntryId Book::principal(AccountId from, AccountId to, Money amount, Date date) {
    auto record = movement(EntryKind::principal, date, from, -positive(amount, account(from)));
    record.other = to;
    record.other_change = positive(amount, account(to));
    return post(std::move(record));
}

InvestmentId Book::invest(AccountId cash, Money amount, std::uint32_t rate, Date date, std::string name) {
    const auto debit = -positive(amount, account(cash));
    named(name);
    investments_.push_back({std::move(name), cash, amount, rate, date, {}});
    InvestmentId id{investments_.size()};
    auto record = movement(EntryKind::invest, date, cash, debit);
    record.investment = id;
    try { post(std::move(record)); }
    catch (...) { investments_.pop_back(); throw; }
    return id;
}

Money Book::interest(InvestmentId id, Date as_of) const {
    const auto& holding = investment(id);
    require(as_of.ok() && as_of >= holding.start, "Interest date precedes investment or is invalid");
    if (holding.redeemed) as_of = std::min(as_of, *holding.redeemed);
    const auto days = (std::chrono::sys_days{as_of} - std::chrono::sys_days{holding.start}).count();
    return Money{scaled_amount(holding.principal.minor_units(), static_cast<std::uint64_t>(holding.annual_basis_points) * days, 365 * 10000),
        holding.principal.currency()};
}

LedgerEntryId Book::redeem(InvestmentId id, Date date) {
    const auto& holding = investment(id);
    const auto proceeds = holding.principal + interest(id, date);
    auto record = movement(EntryKind::redeem, date, holding.settlement, proceeds.minor_units());
    record.investment = id;
    return post(std::move(record));
}

// O(purchases + log currencies). Counts and unique IDs reject duplicates,
// orphan purchases, deleted linked nodes, and changes to posted financial facts.
void Book::validate_tree(const ExpenseTree& tree, std::size_t expected) const {
    (void)expenses(tree.currency());
    require(tree.transaction_count() == expected, "Incorrect number of posted purchases");
    for (const auto& [id, purchase] : tree.expenses_) {
        require(purchase.ledger_entry_id.has_value(), "Unposted expense");
        const auto& record = entry(*purchase.ledger_entry_id);
        require(record.kind == EntryKind::expense && record.purchase.value == id
            && tree.currency() == account(record.account).balance.currency()
            && purchase.amount.minor_units() == -record.change && purchase.date == record.date
            && purchase.account_id == record.account && purchase.location_id == record.location,
            "Expense disagrees with journal");
    }
}

void Book::replace_expenses(ExpenseTree tree) {
    auto& existing = trees_.at(std::string(tree.currency().code()));
    validate_tree(tree, existing.transaction_count());
    existing.replace_validated(std::move(tree));
}

// Full candidate copy and one sorted snapshot make this an atomic bulk import.
// Ordinary postings never take this path or copy the book.
void Book::import_expenses(const ExpenseTree& tree) {
    const auto& current = expenses(tree.currency());
    require(current.size() == 1 && current.tag_registry().size() == 0 &&
        current.revision() == 1, "Import requires a fresh currency tree");
    Book staged = *this;
    auto snapshot = tree.snapshot();
    for (auto& item : snapshot.expenses) {
        auto& metadata = item.expense;
        require(account(metadata.account_id).balance.currency() == tree.currency(),
            "Imported purchase currency disagrees with account");
        auto record = movement(EntryKind::expense, metadata.date, metadata.account_id,
            -metadata.amount.minor_units());
        record.description = tree.node(item.id).name;
        record.location = metadata.location_id;
        record.purchase = item.id;
        metadata.ledger_entry_id = staged.post(std::move(record), {}, {}, true);
    }
    staged.trees_.at(std::string(tree.currency().code())) = ExpenseTree::from_snapshot(std::move(snapshot));
    // Swapping the independently prepared state cannot partially assign it.
    accounts_.swap(staged.accounts_);
    entries_.swap(staged.entries_);
    trees_.swap(staged.trees_);
}

std::map<std::optional<LocationId>, Money> Book::spending_by_location(
    Currency currency, NodeId scope, const QueryFilter& filter) const {
    const auto& tree = expenses(currency);
    std::map<std::optional<LocationId>, Money> totals;
    for (auto leaf : tree.matching_leaves(scope, filter)) {
        const auto& node = tree.node(leaf);
        const auto key = tree.expense(node.transaction_id).location_id;
        auto [it, inserted] = totals.try_emplace(key, 0, currency);
        it->second = it->second + Money{node.amount_minor_units, currency};
    }
    return totals;
}

// O(J) time and O(1) space; include only realized interest on redemption.
Money Book::income_total(AccountId cash, DateRange dates) const {
    const auto& source = account(cash);
    Money sum{0, source.balance.currency()};
    for (const auto& record : entries_) {
        if (record.account != cash || !dates.contains(record.date)) continue;
        if (record.kind == EntryKind::income) sum = sum + Money{record.change, sum.currency()};
        if (record.kind == EntryKind::redeem) sum = sum + interest(record.investment, record.date);
    }
    return sum;
}

ScheduleId Book::add_schedule(EntryKind kind, AccountId cash, Money amount, std::string description,
    Date first, Frequency frequency, std::optional<LocationId> place) {
    named(description);
    require(kind == EntryKind::expense || kind == EntryKind::income, "Invalid scheduled entry kind");
    require(first.ok() && frequency >= Frequency::daily && frequency <= Frequency::yearly, "Invalid recurrence");
    require(account(cash).kind == AccountKind::cash, "Schedule requires cash account");
    positive(amount, account(cash));
    if (place) {
        require(kind == EntryKind::expense, "Only expenses have locations");
        (void)location(*place);
    }
    schedules_.push_back({kind, cash, amount, std::move(description), first, frequency, place, first});
    return ScheduleId{schedules_.size()};
}

void Book::cancel(ScheduleId id) { lookup(schedules_, id).next.reset(); }

std::size_t Book::run_due(ScheduleId id, Date through, std::size_t limit) {
    require(through.ok(), "Invalid cutoff");
    auto& item = lookup(schedules_, id);
    std::size_t count = 0;
    while (count < limit && item.next && *item.next <= through) {
        const auto due = *item.next;
        const auto next = following(item, due);
        if (item.kind == EntryKind::expense)
            spend(item.account, item.amount, due, item.description, item.location);
        else
            income(item.account, item.amount, due, item.description);
        item.next = next;
        ++count;
    }
    return count;
}
} // namespace moneta
