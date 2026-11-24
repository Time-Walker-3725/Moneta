#pragma once

#include "moneta/storage/expense_store.hpp"

#include <deque>
#include <filesystem>
#include <map>

namespace moneta {

using InvestmentId = Identifier<struct InvestmentTag>;
using ScheduleId = Identifier<struct ScheduleTag>;

enum class AccountKind { cash, receivable, payable };
enum class EntryKind { opening, income, expense, transfer, principal, invest, redeem };
enum class Frequency { daily, weekly, monthly, yearly };

struct Account {
    std::string name;
    AccountKind kind;
    Money balance;
    LedgerEntryId last_entry{}; // Zero until the first posting.
};

struct Investment {
    std::string name;
    AccountId settlement;
    Money principal;
    std::uint32_t annual_basis_points; // 100 basis points = 1%.
    Date start;
    std::optional<Date> redeemed;
};

struct LedgerEntry {
    EntryKind kind;
    Date date;
    AccountId account;
    std::int64_t change; // Signed minor units in this account's currency.
    AccountId other{};
    std::int64_t other_change{};
    InvestmentId investment{};
    NodeId purchase{}; // In the paying account's currency tree.
    std::optional<LocationId> location;
    std::string description;
};

struct Schedule {
    EntryKind kind; // Income or expense.
    AccountId account;
    Money amount;
    std::string description;
    Date first;
    Frequency frequency;
    std::optional<LocationId> location;
    std::optional<Date> next; // Empty after cancellation or calendar exhaustion.
};

// Single-writer accounting aggregate. IDs start at one and follow insertion order.
// References to account, investment, schedule and journal records survive appends.
class Book {
public:
    // Amortized O(name bytes + log C), C = currencies. Accounts start at zero.
    AccountId open_account(std::string name, Currency currency, AccountKind kind = AccountKind::cash);
    // O(name bytes). IDs index stable append-only records.
    LocationId add_location(std::string name);
    // O(1) indexed reads, no copies.
    const Account& account(AccountId id) const;
    const std::string& location(LocationId id) const;
    const Investment& investment(InvestmentId id) const;
    const Schedule& schedule(ScheduleId id) const;
    const LedgerEntry& entry(LedgerEntryId id) const;
    const std::deque<LedgerEntry>& journal() const noexcept { return entries_; }
    const std::deque<Account>& accounts() const noexcept { return accounts_; }
    const std::deque<std::string>& locations() const noexcept { return locations_; }
    const std::deque<Investment>& investments() const noexcept { return investments_; }
    const std::deque<Schedule>& schedules() const noexcept { return schedules_; }
    // O(log C). One independent expense hierarchy per currency.
    const ExpenseTree& expenses(Currency currency) const;

    // O(1) accounting work, plus text. Negative cash balances are allowed.
    LedgerEntryId opening(AccountId id, Money signed_balance, Date date);
    LedgerEntryId income(AccountId cash, Money amount, Date date, std::string description);
    // O(log C + h + tag normalization + text), expected and amortized.
    LedgerEntryId spend(AccountId cash, Money amount, Date date, std::string description,
        std::optional<LocationId> location = {}, NodeId parent = NodeId{1},
        std::vector<TagId> tags = {});
    // O(1). Actual settled amounts encode the executed FX rate including rounding.
    LedgerEntryId transfer(AccountId from, AccountId to, Money paid, Money received, Date date);
    // O(1). Move principal between one cash and one obligation account in either direction.
    LedgerEntryId principal(AccountId from, AccountId to, Money amount, Date date);
    // O(name bytes) to fund, O(1) to value or redeem.
    InvestmentId invest(AccountId cash, Money amount, std::uint32_t annual_basis_points,
        Date date, std::string name);
    Money interest(InvestmentId id, Date as_of) const;
    LedgerEntryId redeem(InvestmentId id, Date date);

    // Replace a currency's hierarchy after validating all journal-to-purchase links.
    // Supports recategorization, tags and splits with the posted cash facts intact.
    // O(purchases + log currencies), plus copying an lvalue and reclaiming old storage.
    void replace_expenses(ExpenseTree tree);
    // Book copy + sorted source snapshot + linear replay.
    // Adopt an existing standalone tree into an empty currency hierarchy.
    // Register its account and location IDs first. New journal IDs replace
    // external ledger links. Successful import invalidates borrowed references.
    void import_expenses(const ExpenseTree& tree);
    // Leaf query + O(k log(g+1)) for k matching leaves in g location buckets.
    std::map<std::optional<LocationId>, Money> spending_by_location(
        Currency currency, NodeId scope, const QueryFilter& filter = {}) const;
    // O(journal entries), O(1) extra space. Includes realized investment interest.
    Money income_total(AccountId cash, DateRange dates) const;

    // O(description bytes) to register, O(1) to cancel.
    ScheduleId add_schedule(EntryKind kind, AccountId cash, Money amount, std::string description,
        Date first, Frequency frequency, std::optional<LocationId> location = {});
    void cancel(ScheduleId id);
    // Posts up to limit occurrences for this schedule. Each successful occurrence
    // costs one normal posting and O(1) calendar work, and advances its cursor.
    // Repeat with the same cutoff to resume.
    std::size_t run_due(ScheduleId id, Date through, std::size_t limit = 1000);

    // New files use SQLite. Existing version-1 files keep their text format.
    // A detached SQLite snapshot may update only its original database state.
    // Use BookDatabase::edit for immediately durable individual actions.
    void save(const std::filesystem::path& path) const;
    void export_legacy(const std::filesystem::path& path) const;
    static Book read(std::istream& input, LoadLimits limits = {});
    static Book load(const std::filesystem::path& path, LoadLimits limits = {});

private:
    friend class BookDatabase;
    mutable std::string database_stamp_; // Optimistic token for detached saves.
    std::deque<Account> accounts_;
    std::deque<std::string> locations_;
    std::deque<Investment> investments_;
    std::deque<Schedule> schedules_;
    std::deque<LedgerEntry> entries_;
    std::map<std::string, ExpenseTree, std::less<>> trees_;
    LedgerEntryId post(LedgerEntry entry, NodeId parent = NodeId{1}, std::vector<TagId> tags = {},
        bool replay = false);
    void validate_tree(const ExpenseTree& tree, std::size_t expected) const;
};
} // namespace moneta
