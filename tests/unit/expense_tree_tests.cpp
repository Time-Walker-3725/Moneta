#include "moneta/expenses/expense_tree.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace moneta;
using namespace std::chrono;

namespace {

const Currency inr{"INR", 2};
const Date purchase_date = year{2026} / October / 10;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Action>
void rejects(Action action, const std::string& message) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

Expense purchase(std::int64_t amount, Date date = purchase_date) {
    return Expense{Money{amount, inr}, date, AccountId{7}, LedgerEntryId{12},
        LocationId{3}, "Dominos", "Dinner", {"food", "family"}};
}

void tree_workflow() {
    ExpenseTree tree{inr};
    require(tree.total(tree.root_id()).minor_units() == 0, "Empty total must be zero");
    // Start with only a rough classification, then refine it later.
    const auto food = tree.add_group(tree.root_id(), "Food");
    const auto transaction = tree.add_expense(food, "Dinner", purchase(19800));
    const auto merchant = tree.add_group(food, "Dominos");
    tree.move(transaction, merchant);
    const auto before = *tree.node(transaction).expense;

    const auto items = tree.split(transaction, {
        {"Pizza", Money{15000, inr}, "Large"},
        {"Taco", Money{4800, inr}, ""}});
    require(tree.size() == 6, "Split should add exactly two items");
    require(tree.total(tree.root_id()).minor_units() == 19800, "Root must not double count split items");
    require(tree.total(food).minor_units() == 19800, "Category total must match transaction");
    require(tree.total(merchant).minor_units() == 19800, "Merchant total must match transaction");
    require(tree.total(transaction).minor_units() == 19800, "Transaction total must stay constant");
    require(tree.total(items[0]).minor_units() == 15000, "Item must have its own total");
    require(tree.node(items[0]).item_note == "Large", "Keep item notes");

    const auto others = tree.add_group(tree.root_id(), "Other spending");
    tree.move(merchant, others);
    tree.rename(transaction, "Pizza and taco");
    const auto& after = *tree.node(transaction).expense;
    require(after.amount == before.amount && after.date == before.date &&
        after.account_id == before.account_id && after.ledger_entry_id == before.ledger_entry_id &&
        after.location_id == before.location_id && after.merchant == before.merchant &&
        after.note == before.note && after.tags == before.tags, "Refining and moving must preserve metadata");
    require(tree.total(food).minor_units() == 0, "Old category must lose moved spending");
    require(tree.total(others).minor_units() == 19800, "New category must gain moved spending");
    require(tree.total(tree.root_id()).minor_units() == 19800, "Moving must preserve overall spending");
    require(tree.node(items[0]).parent_id == transaction, "Item must remain attached to transaction");
    tree.move(merchant, others); // Moving to the same parent is a no-op.
    require(tree.node(others).children.size() == 1, "No-op move must not duplicate a node");

    const auto replacement = tree.split(transaction, {
        {"Pizza", Money{10000, inr}, ""},
        {"Taco", Money{4800, inr}, ""},
        {"Delivery", Money{5000, inr}, ""}});
    rejects<std::out_of_range>([&] { (void)tree.node(items[0]); }, "Replaced split IDs must expire");
    require(tree.total(transaction).minor_units() == 19800, "Replacing a split must preserve total");
    tree.merge(transaction);
    rejects<std::out_of_range>([&] { (void)tree.node(replacement[0]); }, "Merged item IDs must expire");
    require(tree.node(transaction).children.empty(), "Merge must remove items");
    require(tree.total(tree.root_id()).minor_units() == 19800, "Merge must preserve spending");
    tree.merge(transaction); // Already unsplit is also a no-op.

    tree.split(transaction, {{"First", Money{10000, inr}, ""}, {"Rest", Money{9800, inr}, ""}});
    require(tree.erase_subtree(others).minor_units() == 19800, "Deletion should return removed spending");
    rejects<std::out_of_range>([&] { (void)tree.node(transaction); }, "Deletion must erase descendants");
    require(tree.size() == 2, "Only root and empty Food category should remain");
    require(tree.total(tree.root_id()).minor_units() == 0, "Deleted subtree must not leave stale totals");
    const auto fresh = tree.add_expense(food, "New dinner", purchase(500));
    require(fresh.value > replacement.back().value, "Removed IDs must never be reused");
}

void date_queries() {
    ExpenseTree tree{inr};
    const auto food = tree.add_group(tree.root_id(), "Food");
    const auto prior = tree.add_expense(food, "Prior", purchase(100, year{2026} / October / 9));
    const auto current = tree.add_expense(food, "Current", purchase(19800));
    tree.add_expense(food, "Later", purchase(200, year{2026} / October / 11));
    const auto split_items = tree.split(current, {
        {"Pizza", Money{15000, inr}, ""}, {"Taco", Money{4800, inr}, ""}});
    const DateRange single_day{purchase_date, purchase_date};
    require(tree.total(food, single_day).minor_units() == 19800, "Single-day query must use inclusive bounds");
    require(tree.total(split_items[0], single_day).minor_units() == 15000, "Items must inherit date");
    require(tree.total(prior, single_day).minor_units() == 0, "Exclude transactions outside the range");
    require(tree.total(food, DateRange{year{2026} / October / 9, purchase_date}).minor_units() == 19900,
        "Both range endpoints must be included");
    auto corrected = purchase(19800, year{2026} / October / 11);
    corrected.location_id = LocationId{4};
    tree.update_expense(current, corrected);
    require(tree.total(split_items[0], single_day).minor_units() == 0, "Items must follow corrected transaction dates");
    require(tree.node(current).expense->location_id == LocationId{4}, "Metadata updates must take effect");
    require(tree.total(food).minor_units() == 20100, "Date correction must preserve all-time totals");
    rejects<std::invalid_argument>([] {
        DateRange{year{2026} / February / 30, purchase_date};
    }, "Reject invalid calendar dates");
    rejects<std::invalid_argument>([] {
        DateRange{purchase_date, year{2026} / October / 9};
    }, "Reject reversed ranges");
}

void rejected_changes_preserve_tree() {
    ExpenseTree tree{inr};
    const auto food = tree.add_group(tree.root_id(), "Food");
    const auto merchant = tree.add_group(food, "Dominos");
    const auto transaction = tree.add_expense(merchant, "Dinner", purchase(19800));
    const auto items = tree.split(transaction, {
        {"Pizza", Money{15000, inr}, ""}, {"Taco", Money{4800, inr}, ""}});
    const auto size_before = tree.size();
    rejects<std::invalid_argument>([&] {
        tree.split(transaction, {{"Wrong", Money{1, inr}, ""}, {"Total", Money{2, inr}, ""}});
    }, "Reject unbalanced split");
    rejects<std::invalid_argument>([&] {
        tree.split(transaction, {{"Only one", Money{19800, inr}, ""}});
    }, "Reject single-item split");
    rejects<std::invalid_argument>([&] {
        tree.split(transaction, {{"Negative", Money{-1, inr}, ""}, {"Rest", Money{19801, inr}, ""}});
    }, "Reject negative split items");
    rejects<std::invalid_argument>([&] {
        tree.split(transaction, {{"Foreign", Money{15000, Currency{"USD", 2}}, ""},
            {"Rest", Money{4800, inr}, ""}});
    }, "Reject mixed-currency split");
    rejects<std::invalid_argument>([&] { tree.move(food, merchant); }, "Reject descendant cycle");
    rejects<std::invalid_argument>([&] { tree.move(food, food); }, "Reject self cycle");
    rejects<std::invalid_argument>([&] { tree.move(tree.root_id(), food); }, "Reject moving root");
    rejects<std::invalid_argument>([&] { tree.move(items[0], food); }, "Reject moving split items");
    rejects<std::invalid_argument>([&] { tree.move(food, transaction); }, "Reject transaction as group parent");
    rejects<std::invalid_argument>([&] { tree.erase_subtree(items[0]); }, "Reject partial deletion of a split");
    rejects<std::invalid_argument>([&] { tree.erase_subtree(tree.root_id()); }, "Reject root deletion");
    rejects<std::invalid_argument>([&] { tree.update_expense(transaction, purchase(100)); }, "Reject mismatched split update");
    rejects<std::out_of_range>([&] { tree.move(transaction, NodeId{999}); }, "Reject unknown destination");
    rejects<std::invalid_argument>([&] { tree.rename(food, " \t"); }, "Reject blank names");
    require(tree.size() == size_before && tree.node(transaction).children == items,
        "Rejected edits must preserve existing items and size");
    require(tree.node(food).parent_id == tree.root_id() && tree.node(merchant).parent_id == food,
        "Rejected moves must preserve structure");
    require(tree.total(tree.root_id()).minor_units() == 19800, "Rejected edits must preserve money");
}

void input_validation_and_money() {
    ExpenseTree tree{inr};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Negative", purchase(-1)); }, "Reject negative expense");
    auto invalid = purchase(1);
    invalid.account_id = AccountId{0};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "No account", invalid); }, "Require account reference");
    invalid = purchase(1, year{2026} / February / 29);
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Invalid date", invalid); }, "Validate purchase date");
    invalid = purchase(1);
    invalid.location_id = LocationId{0};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Invalid location", invalid); }, "Reject zero optional IDs");
    invalid = purchase(1);
    invalid.ledger_entry_id = LedgerEntryId{0};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Invalid ledger", invalid); }, "Reject zero ledger IDs");
    invalid = purchase(1);
    invalid.amount = Money{1, Currency{"INR", 3}};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Wrong scale", invalid); }, "Reject incompatible currency scale");
    require(tree.size() == 1, "Rejected inputs must not insert nodes");
    auto minimal = purchase(0);
    minimal.location_id.reset();
    minimal.ledger_entry_id.reset();
    const auto zero = tree.add_expense(tree.root_id(), "Free item", minimal);
    tree.split(zero, {{"One", Money{0, inr}, ""}, {"Two", Money{0, inr}, ""}});
    require(tree.total(tree.root_id()).minor_units() == 0, "Allow explicit zero-value transactions");
    tree.merge(zero);
    tree.update_expense(zero, purchase(100));
    require(tree.total(zero).minor_units() == 100, "Allow explicit unsplit amount corrections");
    rejects<std::out_of_range>([&] { (void)tree.total(NodeId{0}); }, "Unknown subtree must throw");

    rejects<std::invalid_argument>([] { Currency{"inr", 2}; }, "Reject noncanonical currency code");
    rejects<std::invalid_argument>([] { Currency{"INR", 10}; }, "Reject unsupported currency scale");
    rejects<std::invalid_argument>([] {
        (void)(Money{1, inr} + Money{1, Currency{"USD", 2}});
    }, "Reject adding currencies");
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    require((Money{maximum, inr} + Money{minimum, inr}).minor_units() == -1, "Signed boundary sum must be exact");
    rejects<std::overflow_error>([&] { (void)(Money{maximum, inr} + Money{1, inr}); }, "Reject positive overflow");
    rejects<std::overflow_error>([&] { (void)(Money{minimum, inr} + Money{-1, inr}); }, "Reject negative overflow");
    ExpenseTree large{inr};
    const auto huge = large.add_expense(large.root_id(), "Huge", purchase(maximum));
    rejects<std::overflow_error>([&] {
        large.split(huge, {{"Huge", Money{maximum, inr}, ""}, {"Extra", Money{1, inr}, ""}});
    }, "Reject overflowing split total");
    require(large.node(huge).children.empty() && large.size() == 2, "Overflow must preserve transaction");
    large.add_expense(large.root_id(), "Extra", purchase(1));
    rejects<std::overflow_error>([&] { (void)large.total(large.root_id()); }, "Aggregate overflow must be explicit");

    const Currency jpy{"JPY", 0};
    ExpenseTree japanese{jpy};
    japanese.add_expense(japanese.root_id(), "Lunch", Expense{
        Money{198, jpy}, purchase_date, AccountId{7}, std::nullopt, std::nullopt, "", "", {}});
    require(japanese.total(japanese.root_id()) == Money{198, jpy}, "Support currencies without decimal places");
}

void tag_queries() {
    ExpenseTree tree{inr};
    const auto entertainment = tree.add_group(tree.root_id(), "Entertainment");
    const auto utilities = tree.add_group(tree.root_id(), "Utilities");
    auto streaming_expense = purchase(19900);
    streaming_expense.tags = {"#Subscription", "subscription", "Entertainment"};
    const auto streaming = tree.add_expense(entertainment, "Streaming", streaming_expense);
    auto unrelated_expense = purchase(1200);
    unrelated_expense.tags = {"subscriptionExtra"};
    const auto unrelated = tree.add_expense(entertainment, "One-time purchase", unrelated_expense);
    auto gym_expense = purchase(50000, year{2026} / October / 11);
    gym_expense.tags = {" SUBSCRIPTION ", "Health"};
    const auto gym = tree.add_expense(utilities, "Gym", gym_expense);
    auto cloud_expense = purchase(9000, year{2026} / November / 10);
    cloud_expense.tags = {"subscription"};
    const auto cloud = tree.add_expense(utilities, "Cloud storage", cloud_expense);
    const auto parts = tree.split(streaming, {
        {"Base", Money{15000, inr}, ""}, {"Extra", Money{4900, inr}, ""}});

    require(tree.transactions_by_tag(tree.root_id(), "subscription") ==
        std::vector<NodeId>{streaming, gym, cloud}, "Tag queries must cross categories in tree order");
    require(tree.total_by_tag(tree.root_id(), " #SuBsCrIpTiOn ").minor_units() == 78900,
        "Normalize tag spelling and count split/duplicate-tagged transactions only once");
    require(tree.node(streaming).expense->tags == streaming_expense.tags,
        "Filtering must not alter stored tag labels");
    const DateRange october{year{2026} / October / 1, year{2026} / October / 31};
    require(tree.total_by_tag(tree.root_id(), "subscription", october).minor_units() == 69900,
        "Combine tag and date filters");
    require(tree.transactions_by_tag(tree.root_id(), "subscription", october) ==
        std::vector<NodeId>{streaming, gym}, "Date filter must constrain the result list");
    require(tree.total_by_tag(tree.root_id(), "subscription", DateRange{purchase_date, purchase_date})
        .minor_units() == 19900, "Tagged date endpoints must be inclusive");
    require(tree.total_by_tag(utilities, "subscription").minor_units() == 59000,
        "Restrict queries to the selected subtree");
    require(tree.total_by_tag(streaming, "subscription").minor_units() == 19900,
        "Query a tagged transaction directly");
    require(tree.total_by_tag(parts[0], "subscription").minor_units() == 15000,
        "An item inherits tags but contributes only its own amount");
    require(tree.transactions_by_tag(parts[0], "subscription") == std::vector<NodeId>{streaming},
        "An item query must return its owning transaction");
    require(tree.total_by_tag(tree.root_id(), "sub").minor_units() == 0,
        "Match full tags rather than substrings");
    require(tree.transactions_by_tag(tree.root_id(), "missing").empty() &&
        tree.total_by_tag(tree.root_id(), "missing").minor_units() == 0,
        "Unmatched tags must return an empty list and zero spending");
    tree.move(streaming, utilities);
    tree.merge(streaming);
    require(tree.total_by_tag(tree.root_id(), "subscription").minor_units() == 78900,
        "Reclassification and merging must preserve tagged spending");
    gym_expense.tags = {"Health"};
    tree.update_expense(gym, gym_expense);
    require(tree.total_by_tag(tree.root_id(), "subscription").minor_units() == 28900,
        "Removing tags must immediately update query results");
    unrelated_expense.tags.push_back("Subscription");
    tree.update_expense(unrelated, unrelated_expense);
    require(tree.total_by_tag(tree.root_id(), "subscription").minor_units() == 30100,
        "Adding tags must immediately update query results");
    for (const auto* empty_tag : {"", " \t", "#", " # \t"}) {
        rejects<std::invalid_argument>([&] { (void)tree.total_by_tag(tree.root_id(), empty_tag); },
            "Reject blank tag filters");
        rejects<std::invalid_argument>([&] { (void)tree.transactions_by_tag(tree.root_id(), empty_tag); },
            "Reject blank tag list filters");
    }
    rejects<std::out_of_range>([&] { (void)tree.total_by_tag(NodeId{999}, "subscription"); },
        "Reject unknown tagged-query subtrees");

    ExpenseTree huge{inr};
    auto maximum = purchase(std::numeric_limits<std::int64_t>::max());
    maximum.tags = {"subscription"};
    huge.add_expense(huge.root_id(), "Maximum", maximum);
    maximum.amount = Money{1, inr};
    huge.add_expense(huge.root_id(), "Overflow", maximum);
    rejects<std::overflow_error>([&] { (void)huge.total_by_tag(huge.root_id(), "subscription"); },
        "Tagged totals must retain overflow protection");
}

void deep_and_wide_trees() {
    ExpenseTree tree{inr};
    auto deepest = tree.root_id();
    for (int i = 0; i < 2048; ++i) {
        deepest = tree.add_group(deepest, "Nested category");
    }
    tree.add_expense(deepest, "Deep purchase", purchase(500));
    const auto wide = tree.add_group(tree.root_id(), "Many purchases");
    for (int i = 0; i < 2048; ++i) {
        tree.add_expense(wide, "Purchase", purchase(1));
    }
    require(tree.total(tree.root_id()).minor_units() == 2548, "Traversal must handle deep and wide trees");
    tree.move(wide, deepest);
    require(tree.total(tree.root_id()).minor_units() == 2548, "Move must preserve a large subtree");
    const auto first_group = tree.node(tree.root_id()).children.front();
    require(tree.erase_subtree(first_group).minor_units() == 2548, "Iterative deletion must include all descendants");
    require(tree.size() == 1, "Deep deletion must leave only root");
}

} // namespace

int main() {
    try {
        tree_workflow();
        date_queries();
        rejected_changes_preserve_tree();
        input_validation_and_money();
        tag_queries();
        deep_and_wide_trees();
        std::cout << "Passed all 6 expense-tree test suites\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Expense-tree test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
