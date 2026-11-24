#include "moneta/accounting/book.hpp"
#include "moneta/accounting/exchange_rate.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

using namespace moneta;
using namespace std::chrono;

namespace {
const Currency usd{"USD", 2}, jpy{"JPY", 0};
Money dollars(std::int64_t cents) { return Money{cents, usd}; }
const auto start = year{2024}/January/31;

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected operation to fail");
}

void accounts_and_loans() {
    Book b;
    const auto cash = b.open_account("Cash", usd);
    const auto foreign = b.open_account("Yen", jpy);
    const auto loan = b.open_account("Student loan", usd, AccountKind::payable);
    const auto lent = b.open_account("Loan to sister", usd, AccountKind::receivable);
    b.opening(cash, dollars(10000), start);
    b.principal(loan, cash, dollars(4000), start);
    b.principal(cash, loan, dollars(1000), start);
    b.principal(cash, lent, dollars(1200), start);
    b.principal(lent, cash, dollars(200), start);
    const ExchangeRate quote{usd, jpy, 150, 100};
    b.transfer(cash, foreign, dollars(101), quote.convert(dollars(101)), start);
    check(b.account(foreign).balance == Money{152, jpy}, "FX rounds half up at settlement");
    check(b.account(cash).balance == dollars(11899), "Cash tracks both legs");
    check(b.account(loan).balance == dollars(-3000) && b.account(lent).balance == dollars(1000),
        "Outstanding obligations preserve principal");
    check(b.income_total(cash, {start, start}) == dollars(0), "Principal and transfers excluded from income");
    auto size = b.journal().size();
    rejects([&]{ b.principal(cash, loan, dollars(3001), start); });
    rejects([&]{ b.opening(cash, dollars(1), start); });
    rejects([&]{ b.transfer(cash, cash, dollars(1), dollars(1), start); });
    rejects([&]{ b.spend(cash, dollars(10), start, "Unknown place", LocationId{9}); });
    rejects([&]{ b.spend(cash, dollars(10), start, "Bad parent", {}, NodeId{9}); });
    rejects([&]{ b.open_account("bad scale", Currency{"USD", 0}); });
    check(b.journal().size() == size && b.account(cash).balance == dollars(11899),
        "Invalid transactions leave balance and journal intact");
    const auto existing = b.open_account("Existing debt", usd, AccountKind::payable);
    rejects([&]{ b.opening(existing, dollars(1), start); });
    const auto opening = b.opening(existing, dollars(-400), start);
    check(b.account(existing).last_entry == opening, "Opening obligation references its posting");
    const auto other = b.open_account("Maximum", usd);
    b.opening(other, dollars(INT64_MAX), start);
    rejects([&]{ b.transfer(cash, other, dollars(1), dollars(1), start); });
    rejects([&]{ b.transfer(cash, other, dollars(2), dollars(1), start); });
    check(b.account(cash).balance == dollars(11899), "Second leg failure leaves first intact");
    check(ExchangeRate{usd, usd, UINT32_MAX, UINT32_MAX}.convert(dollars(INT64_MAX)) == dollars(INT64_MAX),
        "Large intermediate converted exactly");
    rejects([&]{ (void)ExchangeRate{usd, jpy, 2, 1}.convert(dollars(INT64_MAX)); });
    rejects([&]{ (void)ExchangeRate{usd, jpy, 2, 0}.convert(dollars(1)); });
}

void interest_and_recurrence() {
    Book b;
    const auto cash = b.open_account("Bank", usd);
    const auto id = b.invest(cash, dollars(100000), 525, start, "Fixed deposit");
    const Date maturity{sys_days{start} + days{365}};
    check(b.interest(id, start) == dollars(0) && b.interest(id, maturity) == dollars(5250), "ACT/365 interest");
    b.redeem(id, maturity);
    check(b.account(cash).balance == dollars(5250) && b.income_total(cash, {start, maturity}) == dollars(5250),
        "Only realized interest is income");
    rejects([&]{ b.redeem(id, maturity); });
    rejects([&]{ (void)b.interest(id, year{2023}/January/1); });
    check(b.interest(id, year{2030}/May/1) == dollars(5250), "Redemption freezes accrual");
    const auto half = b.invest(cash, dollars(1), 5000, start, "Half cent");
    check(b.interest(half, maturity) == dollars(1), "Interest ties round up");

    const auto sub = b.add_schedule(EntryKind::expense, cash, dollars(200), "Monthly", start, Frequency::monthly);
    check(b.run_due(sub, year{2024}/March/31, 2) == 2, "Bounded catch-up");
    check(b.journal().back().date == year{2024}/February/29 && b.schedule(sub).next == year{2024}/March/31,
        "Month-end recurrence preserves January anchor");
    check(b.run_due(sub, year{2024}/March/31) == 1 && b.run_due(sub, year{2024}/March/31) == 0,
        "Repeating cutoff is idempotent");
    b.cancel(sub);
    check(b.run_due(sub, year{2030}/January/1) == 0, "Cancelled schedule");
    auto annual = b.add_schedule(EntryKind::income, cash, dollars(1), "Yearly", year{2024}/February/29, Frequency::yearly);
    b.run_due(annual, year{2027}/March/1);
    check(b.schedule(annual).next == year{2028}/February/29, "Leap-year anchor restored");
    for (auto frequency : {Frequency::weekly, Frequency::daily, Frequency::monthly, Frequency::yearly}) {
        const auto end = year::max()/December/31;
        const auto last = b.add_schedule(EntryKind::income, cash, dollars(1), "Final", end, frequency);
        check(b.run_due(last, end) == 1 && !b.schedule(last).next, "Finite calendar exhaustion");
    }
    Book full;
    auto maxed = full.open_account("Full", usd);
    full.opening(maxed, dollars(INT64_MAX - 1), start);
    auto daily = full.add_schedule(EntryKind::income, maxed, dollars(1), "Daily", start, Frequency::daily);
    rejects([&]{ full.run_due(daily, year{2024}/February/1); });
    check(full.journal().size() == 2 && full.schedule(daily).next == year{2024}/February/1,
        "Failure in catch-up retains first success and leaves next occurrence due");
}

void legacy_tree() {
    Book b;
    auto cash = b.open_account("Cash", usd);
    ExpenseTree tree{usd};
    auto id = tree.add_expense(tree.root_id(), "Legacy", {dollars(30), start, cash, LedgerEntryId{99}, LocationId{1}, "shop", {}});
    tree.split(id, {{"A", dollars(10), {}, {}}, {"B", dollars(20), {}, {}}});
    rejects([&]{ b.import_expenses(tree); });
    check(b.journal().empty() && b.account(cash).balance == dollars(0), "Import is atomic");
    auto place = b.add_location("Home");
    b.import_expenses(tree);
    check(b.account(cash).balance == dollars(-30) && b.expenses(usd).expense(id).ledger_entry_id == LedgerEntryId{1},
        "Migration relinks purchases");
    check(b.spending_by_location(usd, id).at(place) == dollars(30), "Split import reports leaves once");
    rejects([&]{ b.import_expenses(tree); });
}

void persistence(const std::filesystem::path& path) {
    Book b;
    const auto cash = b.open_account("Cash", usd);
    const auto wallet = b.open_account("Wallet", jpy);
    const auto debt = b.open_account("Loan", usd, AccountKind::payable);
    const auto place = b.add_location("Bookshop");
    b.principal(debt, cash, dollars(5000), start);
    b.transfer(cash, wallet, dollars(1000), Money{1500, jpy}, start);
    b.spend(wallet, Money{3, jpy}, start, "Snack");
    b.income(cash, dollars(500), start, "Work");
    const auto deposit = b.invest(cash, dollars(1000), 500, start, "CD");
    const auto closed = b.invest(cash, dollars(100), 500, start, "Closed");
    b.redeem(closed, start);
    const auto id = b.spend(cash, dollars(300), start, "Quoted \"receipt\"\npart 2", place);
    auto tree = b.expenses(usd);
    const auto purchase = b.entry(id).purchase;
    auto tag = tree.create_tag("Food");
    tree.split(purchase, {{"Food", dollars(200), {}, {tag}}, {"Soap", dollars(100), {}, {}}});
    b.replace_expenses(std::move(tree));
    tree = b.expenses(usd);
    tree.erase_subtree(purchase);
    rejects([&]{ b.replace_expenses(std::move(tree)); });
    const auto sub = b.add_schedule(EntryKind::expense, cash, dollars(50), "Weekly", start, Frequency::weekly);
    b.run_due(sub, start);
    b.export_legacy(path);
    auto restored = Book::load(path);
    check(restored.account(cash).balance == b.account(cash).balance &&
        restored.account(wallet).balance == b.account(wallet).balance &&
        restored.account(debt).balance == dollars(-5000), "Balances recovered by replay");
    check(restored.entry(id).description == b.entry(id).description && restored.location(place) == "Bookshop",
        "Quoted text and links survive");
    check(restored.investment(closed).redeemed == start && !restored.investment(deposit).redeemed,
        "Investment states restored");
    check(restored.income_total(cash, {start,start}) == dollars(500), "Income report after reload");
    const auto totals = restored.spending_by_location(usd, NodeId{1});
    check(totals.at(place) == dollars(300) && totals.at(std::nullopt) == dollars(50), "Location report");
    QueryFilter filter;
    filter.tags = {tag};
    check(restored.spending_by_location(usd, purchase, filter).at(place) == dollars(200), "Partial tagged receipt");
    check(restored.run_due(sub, start) == 0 && restored.run_due(sub, year{2024}/February/7) == 1, "Resume saved schedule");
    restored.save(path);
    check(Book::load(path).journal().size() == restored.journal().size(), "Atomic replacement roundtrip");
    std::ifstream in{path};
    const std::string valid{std::istreambuf_iterator<char>{in}, {}};
    auto corrupt = [&](std::string text) {
        { std::ofstream out{path}; out << text; }
        rejects([&]{ (void)Book::load(path); });
    };
    corrupt(valid + "trailing");
    corrupt(valid.substr(0, valid.size()/2));
    // A stored purchase amount must reconcile with its receipt and journal.
    const auto expense_row = valid.find("EXPENSE ");
    check(expense_row != std::string::npos, "Find stored purchase");
    auto wrong_amount = valid;
    const auto amount = wrong_amount.find(' ', expense_row + 8) + 1;
    wrong_amount.replace(amount, wrong_amount.find(' ', amount) - amount, "99");
    corrupt(wrong_amount);
    auto altered = valid;
    auto pos = altered.find("0 \"Weekly\"");
    check(pos != std::string::npos, "Find weekly ledger record");
    altered.replace(pos, 1, "99");
    corrupt(altered);
}

} // namespace

int main() {
    try {
        accounts_and_loans();
        interest_and_recurrence();
        legacy_tree();
        const auto path = std::filesystem::temp_directory_path() /
            ("moneta-book-test-" + std::to_string(steady_clock::now().time_since_epoch().count()));
        if (!std::filesystem::create_directory(path)) throw std::runtime_error("Temporary directory unavailable");
        try { persistence(path/"accounting.moneta"); }
        catch (...) { std::filesystem::remove_all(path); throw; }
        std::filesystem::remove_all(path);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
