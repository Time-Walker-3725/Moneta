#include "moneta/expenses/expense_tree.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace moneta;
using namespace std::chrono;

namespace {
const Currency inr{"INR", 2};
const Date purchase_date = year{2026} / October / 10;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Exception, typename Action>
void rejects(Action action, const std::string& message) {
    try { action(); } catch (const Exception&) { return; }
    throw std::runtime_error(message);
}

Expense purchase(std::int64_t amount, Date date = purchase_date) {
    return {Money{amount, inr}, date, AccountId{7}, LedgerEntryId{12},
        LocationId{3}, "Walmart", "Receipt note"};
}

// Independent oracle: walk source children and inspect explicit ancestor tags.
// It uses no production query helpers or query indexes.
std::vector<NodeId> preorder(const ExpenseTree& tree, NodeId root) {
    std::vector<NodeId> result;
    std::vector<NodeId> pending{root};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        result.push_back(id);
        const auto& children = tree.node(id).children;
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }
    return result;
}

bool reference_tag_match(const ExpenseTree& tree, NodeId id, TagId tag) {
    while (id.value != 0) {
        const auto& node = tree.node(id);
        if (std::find(node.tags.begin(), node.tags.end(), tag) != node.tags.end()) return true;
        id = node.parent_id;
    }
    return false;
}

bool reference_match(const ExpenseTree& tree, NodeId id, const QueryFilter& filter) {
    const auto& node = tree.node(id);
    if (!node.children.empty() ||
        (node.kind != NodeKind::transaction && node.kind != NodeKind::item)) return false;
    const auto& expense = tree.expense(node.transaction_id);
    if (filter.dates && !filter.dates->contains(expense.date)) return false;
    if (filter.account && expense.account_id != *filter.account) return false;
    if (filter.location && expense.location_id != filter.location) return false;
    if (filter.tags.empty()) return true;
    if (filter.tag_match == TagMatch::all) {
        return std::all_of(filter.tags.begin(), filter.tags.end(), [&](TagId tag) {
            return reference_tag_match(tree, id, tag);
        });
    }
    return std::any_of(filter.tags.begin(), filter.tags.end(), [&](TagId tag) {
        return reference_tag_match(tree, id, tag);
    });
}

void verify_queries(const ExpenseTree& tree, NodeId root, const QueryFilter& filter,
    const std::string& context) {
    const auto ordered = preorder(tree, root);
    std::vector<NodeId> leaves, transactions;
    std::unordered_set<std::uint64_t> seen_transactions;
    std::unordered_map<std::uint64_t, std::int64_t> totals;
    std::int64_t expected_total = 0;
    for (const auto id : ordered) {
        if (!reference_match(tree, id, filter)) continue;
        leaves.push_back(id);
        const auto& node = tree.node(id);
        expected_total += node.amount_minor_units;
        if (seen_transactions.insert(node.transaction_id.value).second) transactions.push_back(node.transaction_id);
        for (auto ancestor = id;; ancestor = tree.node(ancestor).parent_id) {
            totals[ancestor.value] += node.amount_minor_units;
            if (ancestor == root) break;
        }
    }
    const auto revision = tree.revision();
    require(tree.filtered_total(root, filter).minor_units() == expected_total,
        context + ": indexed sum differs from reference leaf traversal");
    require(tree.matching_leaves(root, filter) == leaves, context + ": wrong matching leaves or tree order");
    require(tree.transactions(root, filter) == transactions, context + ": wrong deduplicated owning transactions");
    const auto view = tree.filter_tree(root, filter);
    require(view->root == root && view->revision == revision, context + ": wrong view identity or revision");
    std::unordered_map<std::uint64_t, std::size_t> rows;
    std::size_t row = 0;
    for (const auto id : ordered) {
        const auto found = totals.find(id.value);
        if (found == totals.end()) continue;
        require(row < view->nodes.size(), context + ": missing matching ancestor");
        const auto& actual = view->nodes[row];
        const auto parent = id == root ? std::numeric_limits<std::size_t>::max()
            : rows.at(tree.node(id).parent_id.value);
        require(actual.id == id && actual.parent_index == parent && actual.amount_minor_units == found->second,
            context + ": wrong filtered row identity, parent, or total");
        rows.emplace(id.value, row++);
    }
    require(row == view->nodes.size(), context + ": view contains unrelated branches");
    require(tree.revision() == revision, context + ": filtering mutated authoritative revision");
}

std::string source_state(const ExpenseTree& tree) {
    std::ostringstream out;
    out << tree.revision() << ':' << tree.size();
    for (const auto id : preorder(tree, tree.root_id())) {
        const auto& node = tree.node(id);
        out << '|' << id.value << ':' << node.parent_id.value << ':' << static_cast<int>(node.kind)
            << ':' << node.name << ':' << node.amount_minor_units << ':' << node.transaction_id.value
            << ':' << tree.note(id);
        for (const auto tag : node.tags) out << '#' << tag.value;
        if (node.kind == NodeKind::transaction) {
            const auto& expense = tree.expense(id);
            out << '@' << expense.amount.minor_units() << ':' << static_cast<int>(expense.date.year())
                << ':' << static_cast<unsigned>(expense.date.month()) << ':' << static_cast<unsigned>(expense.date.day())
                << ':' << expense.account_id.value << ':' << expense.merchant << ':' << expense.note;
            if (expense.location_id) out << 'L' << expense.location_id->value;
            if (expense.ledger_entry_id) out << 'E' << expense.ledger_entry_id->value;
        }
    }
    return out.str();
}

void tree_workflow() {
    ExpenseTree tree{inr};
    require(tree.total(tree.root_id()).minor_units() == 0, "Empty total must be zero");
    const auto food = tree.add_group(tree.root_id(), "Food");
    const auto other = tree.add_group(tree.root_id(), "Other");
    const auto transaction = tree.add_expense(food, "Dinner", purchase(19800));
    const auto items = tree.split(transaction, {
        {"Pizza", Money{15000, inr}, "Large", {}}, {"Taco", Money{4800, inr}, "", {}}});
    require(tree.note(items[0]) == "Large", "Keep item notes in separate store");
    const auto nested = tree.split(items[0], {
        {"Base", Money{10000, inr}, "", {}}, {"Toppings", Money{5000, inr}, "", {}}});
    require(tree.node(nested[0]).transaction_id == transaction, "Nested items retain canonical ownership");
    tree.update_item(nested[1], Money{5500, inr});
    require(tree.total(food).minor_units() == 20300 && tree.expense(transaction).amount.minor_units() == 20300 &&
        tree.total(items[0]).minor_units() == 15500, "Nested edits correct ancestors and canonical amount");
    tree.move(transaction, other);
    require(tree.total(food).minor_units() == 0 && tree.total(other).minor_units() == 20300,
        "Move corrects both category totals");
    require(tree.expense(transaction).merchant == "Walmart" && tree.expense(transaction).date == purchase_date &&
        tree.note(transaction) == "Receipt note", "Splitting and moving preserve metadata");
    tree.rename(transaction, "Pizza and taco");
    tree.set_note(transaction, "Corrected receipt");
    tree.set_note(nested[0], "Thin crust");
    require(tree.expense(transaction).note == "Corrected receipt" && tree.note(nested[0]) == "Thin crust",
        "Transaction and item note APIs stay coherent");
    tree.move(transaction, other);
    require(tree.node(other).children == std::vector<NodeId>{transaction}, "Same-parent move cannot duplicate child");
    require(tree.erase_subtree(nested[1]).minor_units() == 5500, "Deletion returns removed amount");
    require(tree.expense(transaction).amount.minor_units() == 14800 && tree.total(tree.root_id()).minor_units() == 14800,
        "Deleting nested item corrects canonical transaction and all cached totals");
    const auto replacement = tree.split(transaction, {
        {"Replacement one", Money{5000, inr}, "", {}}, {"Replacement two", Money{9800, inr}, "", {}}});
    require(!tree.contains(items[0]) && !tree.contains(nested[0]) && replacement.front().value > nested.back().value,
        "Replacing a split erases nested descendants and allocates fresh stable IDs");
    tree.merge(transaction);
    require(tree.node(transaction).children.empty() && !tree.contains(replacement[0]) &&
        tree.total(transaction).minor_units() == 14800, "Merge preserves amount and erases descendants");
    tree.merge(transaction);
    tree.update_expense(transaction, purchase(100));
    require(tree.total(other).minor_units() == 100, "Unsplit amount correction updates categories");
    tree.erase_subtree(other);
    const auto fresh = tree.add_expense(food, "New dinner", purchase(500));
    require(fresh.value > nested.back().value && tree.total(tree.root_id()).minor_units() == 500,
        "Deleted node IDs are never reused");
}

void grouping_and_item_moves() {
    ExpenseTree tree{inr};
    const auto recurring = tree.create_tag("Recurring");
    const auto transaction = tree.add_expense(tree.root_id(), "Shopping", purchase(100));
    const auto parts = tree.split(transaction, {
        {"Chicken", Money{40, inr}, "", {}}, {"Beef", Money{30, inr}, "", {}}, {"Rice", Money{30, inr}, "", {}}});
    const auto meat = tree.group_items(transaction, {parts[1], parts[0]}, "Meat", {recurring});
    require(tree.node(meat).kind == NodeKind::item && tree.node(meat).children == std::vector<NodeId>{parts[0], parts[1]} &&
        tree.total(meat).minor_units() == 70 && tree.expense(transaction).amount.minor_units() == 100,
        "Group siblings preserving IDs, order, and money");
    require(tree.has_tag(parts[0], recurring), "Composite item propagates tag");
    tree.move(parts[2], meat);
    require(tree.total(meat).minor_units() == 100 && tree.total(transaction).minor_units() == 100,
        "Moving within purchase preserves canonical amount");
    tree.ungroup_items(meat);
    require(!tree.contains(meat) && tree.node(transaction).children == parts, "Ungroup preserves item identities and order");
    for (const auto part : parts) require(tree.has_explicit_tag(part, recurring), "Ungroup preserves inherited tags explicitly");
    tree.erase_subtree(parts[0]);
    tree.erase_subtree(parts[1]);
    tree.erase_subtree(parts[2]);
    require(tree.node(transaction).children.empty() && tree.expense(transaction).amount.minor_units() == 0,
        "Removing final split item leaves canonical zero-valued unsplit transaction");
    QueryFilter filter;
    verify_queries(tree, tree.root_id(), filter, "Receipt emptied by item deletion");
}

void rejected_changes_preserve_tree() {
    ExpenseTree tree{inr};
    const auto food = tree.add_group(tree.root_id(), "Food");
    const auto nested = tree.add_group(food, "Nested");
    const auto transaction = tree.add_expense(nested, "Shopping", purchase(100));
    const auto other = tree.add_expense(food, "Other", purchase(50));
    const auto parts = tree.split(transaction, {
        {"A", Money{60, inr}, "", {}}, {"B", Money{40, inr}, "", {}}});
    const auto original = source_state(tree);
    rejects<std::invalid_argument>([&] { tree.split(transaction, {
        {"A", Money{1, inr}, "", {}}, {"B", Money{2, inr}, "", {}}}); }, "Reject unbalanced split");
    rejects<std::invalid_argument>([&] { tree.split(transaction, {{"One", Money{100, inr}, "", {}}}); }, "Reject singleton split");
    rejects<std::invalid_argument>([&] { tree.split(transaction, {
        {"Negative", Money{-1, inr}, "", {}}, {"Rest", Money{101, inr}, "", {}}}); }, "Reject negative split");
    rejects<std::invalid_argument>([&] { tree.split(transaction, {
        {"Foreign", Money{60, Currency{"USD", 2}}, "", {}}, {"Rest", Money{40, inr}, "", {}}}); }, "Reject split currencies");
    rejects<std::invalid_argument>([&] { tree.move(food, nested); }, "Reject category cycle");
    rejects<std::invalid_argument>([&] { tree.move(food, food); }, "Reject self cycle");
    rejects<std::invalid_argument>([&] { tree.move(tree.root_id(), food); }, "Reject moving root");
    rejects<std::invalid_argument>([&] { tree.erase_subtree(tree.root_id()); }, "Reject deleting root");
    rejects<std::invalid_argument>([&] { tree.move(parts[0], other); }, "Reject cross-purchase item move");
    rejects<std::invalid_argument>([&] { tree.move(parts[0], food); }, "Reject item under category");
    rejects<std::invalid_argument>([&] { tree.move(food, transaction); }, "Reject category under purchase");
    rejects<std::invalid_argument>([&] { tree.move(parts[0], parts[1]); }, "Reject nonzero leaf destination");
    rejects<std::invalid_argument>([&] { tree.update_expense(transaction, purchase(99)); }, "Split amount edited through leaves");
    rejects<std::invalid_argument>([&] { tree.update_item(transaction, Money{100, inr}); }, "Only item leaves use update_item");
    rejects<std::invalid_argument>([&] { tree.group_items(transaction, {parts[0], parts[0]}, "Duplicate"); }, "Reject duplicate grouped IDs");
    rejects<std::invalid_argument>([&] { tree.group_items(food, {transaction, other}, "Receipts"); }, "Reject grouping purchases as items");
    rejects<std::invalid_argument>([&] { tree.rename(food, " \t"); }, "Reject blank names");
    rejects<std::out_of_range>([&] { tree.move(transaction, NodeId{99999}); }, "Reject unknown destination");
    require(source_state(tree) == original, "Rejected operations preserve source data and revision");
    const auto zero = tree.split(parts[0], {
        {"Remaining", Money{60, inr}, "", {}}, {"Zero", Money{0, inr}, "", {}}});
    tree.move(zero[0], zero[1]);
    require(tree.node(zero[1]).children == std::vector<NodeId>{zero[0]} && tree.total(transaction).minor_units() == 100,
        "Zero item may become composite without losing money");
    rejects<std::invalid_argument>([&] { tree.move(zero[1], zero[0]); }, "Reject nested item cycle");
    tree.move(zero[0], transaction);
    require(tree.node(zero[1]).children.empty() && tree.total(zero[1]).minor_units() == 0,
        "Moving final child leaves a zero monetary leaf");
}

void validation_and_overflow() {
    ExpenseTree tree{inr};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Negative", purchase(-1)); }, "Reject negative expense");
    auto invalid = purchase(1);
    invalid.account_id = AccountId{0};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Account", invalid); }, "Reject zero account ID");
    invalid = purchase(1, year{2026} / February / 29);
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Date", invalid); }, "Reject invalid date");
    invalid = purchase(1); invalid.location_id = LocationId{0};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Location", invalid); }, "Reject zero optional location");
    invalid = purchase(1); invalid.ledger_entry_id = LedgerEntryId{0};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Ledger", invalid); }, "Reject zero optional ledger");
    invalid = purchase(1); invalid.amount = Money{1, Currency{"INR", 3}};
    rejects<std::invalid_argument>([&] { tree.add_expense(tree.root_id(), "Scale", invalid); }, "Reject wrong currency scale");
    require(tree.size() == 1, "Invalid input cannot insert nodes");
    const auto transaction = tree.add_expense(tree.root_id(), "Valid", purchase(5));
    const auto original = source_state(tree);
    invalid = purchase(5); invalid.account_id = AccountId{0};
    rejects<std::invalid_argument>([&] { tree.update_expense(transaction, invalid); }, "Edit validates account");
    invalid = purchase(5, year{2026} / February / 30);
    rejects<std::invalid_argument>([&] { tree.update_expense(transaction, invalid); }, "Edit validates date");
    require(source_state(tree) == original, "Invalid metadata edits preserve source");
    rejects<std::out_of_range>([&] { (void)tree.node(NodeId{0}); }, "Zero node cannot be looked up");
    rejects<std::invalid_argument>([] { Currency{"inr", 2}; }, "Currency code canonical");
    rejects<std::invalid_argument>([] { Currency{"INR", 10}; }, "Reject unsupported scale");
    rejects<std::invalid_argument>([] { (void)(Money{1, inr} + Money{1, Currency{"USD", 2}}); }, "Money rejects currencies");
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto minimum = std::numeric_limits<std::int64_t>::min();
    require((Money{maximum, inr} + Money{minimum, inr}).minor_units() == -1, "Signed boundary sum exact");
    rejects<std::overflow_error>([&] { (void)(Money{maximum, inr} + Money{1, inr}); }, "Reject positive overflow");
    rejects<std::overflow_error>([&] { (void)(Money{minimum, inr} + Money{-1, inr}); }, "Reject negative overflow");
    ExpenseTree huge{inr};
    const auto receipt = huge.add_expense(huge.root_id(), "Huge", purchase(maximum - 1));
    const auto parts = huge.split(receipt, {
        {"Huge", Money{maximum - 2, inr}, "", {}}, {"Small", Money{1, inr}, "", {}}});
    huge.add_expense(huge.root_id(), "Extra", purchase(1));
    const auto before = source_state(huge);
    rejects<std::overflow_error>([&] { huge.add_expense(huge.root_id(), "Overflow", purchase(1)); },
        "Aggregate overflow rejected before inserting");
    rejects<std::overflow_error>([&] { huge.update_item(parts[1], Money{2, inr}); },
        "Item overflow rejected before updating ancestors or metadata");
    rejects<std::overflow_error>([&] { huge.split(receipt, {
        {"A", Money{maximum, inr}, "", {}}, {"B", Money{1, inr}, "", {}}}); }, "Reject overflowing split");
    require(source_state(huge) == before, "Overflow failures preserve complete source");
    const auto destination = huge.add_group(huge.root_id(), "Destination");
    huge.move(receipt, destination);
    require(huge.total(huge.root_id()).minor_units() == maximum &&
        huge.total(destination).minor_units() == maximum - 1,
        "Moving under common ancestors must not falsely overflow a maximum root total");
    const Currency jpy{"JPY", 0};
    ExpenseTree japanese{jpy};
    japanese.add_expense(japanese.root_id(), "Lunch", {
        Money{198, jpy}, purchase_date, AccountId{7}, std::nullopt, std::nullopt, "", ""});
    require(japanese.total(japanese.root_id()) == Money{198, jpy}, "Currency without decimals supported");
}

void tag_registry_and_sorted_storage() {
    ExpenseTree tree{inr};
    const auto recurring = tree.create_tag(" #Recurring ");
    const auto work = tree.create_tag("Work");
    const auto holiday = tree.create_tag("Holiday");
    require(tree.create_tag("RECURRING") == recurring && tree.find_tag("#recurring") == recurring,
        "Normalized tag resolves stable ID");
    const auto category = tree.add_group(tree.root_id(), "Category", {holiday, recurring, work, recurring});
    require(tree.node(category).tags == std::vector<TagId>{recurring, work, holiday}, "Stored tags sorted and unique");
    const auto receipt = tree.add_expense(category, "Purchase", purchase(100));
    require(tree.has_tag(receipt, work) && !tree.has_explicit_tag(receipt, work), "Inherited vs explicit membership");
    tree.set_tags(receipt, {holiday, holiday, work});
    require(tree.node(receipt).tags == std::vector<TagId>{work, holiday}, "set_tags deduplicates");
    tree.add_tag(receipt, recurring); tree.add_tag(receipt, recurring);
    require(tree.node(receipt).tags.size() == 3, "Repeated add cannot duplicate");
    tree.remove_tag(receipt, recurring);
    require(!tree.has_explicit_tag(receipt, recurring) && tree.has_tag(receipt, recurring), "Removal preserves inherited tag");
    require(tree.effective_tags(receipt) == std::vector<TagId>{recurring, work, holiday}, "Effective tags sorted unique");
    const auto before = source_state(tree);
    rejects<std::out_of_range>([&] { tree.set_tags(receipt, {TagId{99999}}); }, "Unknown tag cannot be stored");
    rejects<std::out_of_range>([&] { (void)tree.has_tag(NodeId{}, recurring); }, "Reject zero inherited lookup ID");
    rejects<std::out_of_range>([&] { (void)tree.effective_tags(NodeId{}); }, "Reject zero effective lookup ID");
    rejects<std::out_of_range>([&] { (void)tree.is_descendant(NodeId{}, category); }, "Reject zero ancestry ID");
    require(source_state(tree) == before, "Invalid tags cannot modify source or revision");
    tree.rename_tag(recurring, "Subscription");
    require(!tree.find_tag("recurring") && tree.find_tag("subscription") == recurring &&
        tree.total_by_tag(tree.root_id(), "subscription").minor_units() == 100, "Rename preserves membership by ID");
    rejects<std::invalid_argument>([&] { tree.rename_tag(recurring, "Work"); }, "Reject conflicting name");
    tree.erase_tag(work);
    require(!std::binary_search(tree.node(category).tags.begin(), tree.node(category).tags.end(), work) &&
        !std::binary_search(tree.node(receipt).tags.begin(), tree.node(receipt).tags.end(), work) && !tree.find_tag("work"),
        "Deleting tag detaches every explicit reference");
    require(tree.create_tag("Fresh").value > holiday.value, "Deleted tag IDs never reused");
    const auto unused = tree.create_tag("Unused");
    QueryFilter unused_filter; unused_filter.tags = {unused};
    verify_queries(tree, tree.root_id(), unused_filter, "Unused registered tag yields no matches");
    require(tree.total_by_tag(tree.root_id(), "Missing").minor_units() == 0 &&
        tree.transactions_by_tag(tree.root_id(), "Missing").empty(), "Unknown named tag gives empty result");
    for (const auto* blank : {"", " \t", "#", " # \t"}) {
        rejects<std::invalid_argument>([&] { tree.create_tag(blank); }, "Reject blank tag definition");
        rejects<std::invalid_argument>([&] { (void)tree.total_by_tag(tree.root_id(), blank); }, "Reject blank tag query");
    }
}

void inheritance_and_combined_filters() {
    ExpenseTree tree{inr};
    const auto recurring = tree.create_tag("Recurring"), work = tree.create_tag("Work");
    const auto groceries = tree.add_group(tree.root_id(), "Groceries");
    const auto office = tree.add_group(tree.root_id(), "Office", {work});
    const auto receipt = tree.add_expense(groceries, "Walmart", purchase(100));
    const auto parts = tree.split(receipt, {
        {"Unprocessed", Money{70, inr}, "", {}}, {"Other", Money{20, inr}, "", {}},
        {"Repeat", Money{10, inr}, "", {recurring}}});
    const auto foods = tree.split(parts[0], {
        {"Meat", Money{50, inr}, "", {recurring}}, {"Bread", Money{20, inr}, "", {}}});
    const auto meats = tree.split(foods[0], {
        {"Chicken", Money{30, inr}, "", {}}, {"Beef", Money{20, inr}, "", {recurring}}});
    const auto misc = tree.split(parts[1], {
        {"Tagged", Money{5, inr}, "", {recurring}}, {"Plain", Money{15, inr}, "", {}}});
    const auto other_receipt = tree.add_expense(office, "Subscription", purchase(40, year{2026} / October / 11), {recurring});
    auto remote = purchase(25, year{2026} / November / 1);
    remote.account_id = AccountId{8}; remote.location_id = LocationId{4};
    tree.add_expense(office, "Cloud", remote, {recurring});
    QueryFilter filter; filter.tags = {recurring};
    verify_queries(tree, tree.root_id(), filter, "Descendant tags under untagged purchase");
    require(tree.filtered_total(groceries, filter).minor_units() == 65, "Composite and child duplicate count once");
    require(tree.transactions(receipt, filter) == std::vector<NodeId>{receipt}, "Multiple matching items deduplicate purchase");
    verify_queries(tree, meats[0], filter, "Inherited tag above query root");
    verify_queries(tree, misc[1], filter, "Unmatched leaf root");
    filter.tags = {work, recurring, work}; filter.tag_match = TagMatch::all;
    verify_queries(tree, tree.root_id(), filter, "All tags across ancestor paths");
    require(tree.filtered_total(tree.root_id(), filter).minor_units() == 65, "All-tags matches office expenses only");
    filter.tag_match = TagMatch::any;
    verify_queries(tree, tree.root_id(), filter, "Any-tag union overlap");
    filter.dates = DateRange{purchase_date, year{2026} / October / 11};
    filter.account = AccountId{7}; filter.location = LocationId{3};
    verify_queries(tree, tree.root_id(), filter, "Combined date account location tags");
    require(tree.filtered_total(tree.root_id(), filter).minor_units() == 105, "Combined filter inherits transaction metadata");
    tree.move(receipt, office); filter.tag_match = TagMatch::all;
    verify_queries(tree, tree.root_id(), filter, "Move changes inherited membership");
    require(tree.filtered_total(tree.root_id(), filter).minor_units() == 105, "Moved purchase gains category tag");
    tree.add_tag(tree.root_id(), recurring);
    filter.tags = {recurring}; filter.dates.reset(); filter.account.reset(); filter.location.reset();
    verify_queries(tree, tree.root_id(), filter, "Root tag includes entire tree");
    require(tree.filtered_total(tree.root_id(), filter) == tree.total(tree.root_id()), "Root tag includes all without duplicate sums");
    auto corrected = tree.expense(other_receipt); corrected.date = year{2026} / November / 3;
    corrected.location_id = LocationId{9}; tree.update_expense(other_receipt, corrected);
    filter.dates = DateRange{purchase_date, year{2026} / October / 11};
    verify_queries(tree, tree.root_id(), filter, "Metadata edit refreshes date index");
}

void filtered_views_and_cache() {
    ExpenseTree tree{inr};
    const auto tag = tree.create_tag("Zero");
    const auto category = tree.add_group(tree.root_id(), "Zero category");
    const auto unused = tree.add_group(tree.root_id(), "Empty", {tag});
    const auto zero = tree.add_expense(category, "Free", purchase(0), {tag});
    tree.add_expense(category, "Paid", purchase(100));
    QueryFilter filter; filter.tags = {tag};
    const auto before = source_state(tree);
    verify_queries(tree, tree.root_id(), filter, "Zero-value matching branch");
    verify_queries(tree, zero, filter, "Direct zero-money leaf bypasses global index");
    verify_queries(tree, unused, filter, "Empty tagged category has no expense matches");
    QueryFilter direct = filter;
    direct.dates = DateRange{year{2026} / November / 1, year{2026} / November / 30};
    verify_queries(tree, zero, direct, "Direct zero leaf excluded by date");
    direct.dates.reset(); direct.account = AccountId{99};
    verify_queries(tree, zero, direct, "Direct leaf excluded by account");
    direct.account.reset(); direct.location = LocationId{99};
    verify_queries(tree, zero, direct, "Direct leaf excluded by location");
    auto cached = tree.filter_tree(tree.root_id(), filter);
    require(cached->nodes.size() == 3 && cached->nodes.back().id == zero, "Zero leaf and ancestors survive; empty category excluded");
    require(tree.filter_tree(tree.root_id(), filter) == cached, "Cache hit returns identical shared immutable view");
    auto duplicated_filter = filter;
    duplicated_filter.tags = {tag, tag};
    require(tree.filter_tree(tree.root_id(), duplicated_filter) == cached,
        "Equivalent duplicate tag filters share the normalized cache key");
    require(source_state(tree) == before && tree.node(unused).children.empty(), "Filtering preserves authoritative source");
    tree.update_expense(zero, purchase(5));
    auto changed = tree.filter_tree(tree.root_id(), filter);
    require(changed != cached && changed->nodes.front().amount_minor_units == 5 && cached->nodes.front().amount_minor_units == 0,
        "Mutation invalidates cache; previously returned view remains unchanged");
    tree.set_filter_cache_limits(1, 2);
    auto too_large = tree.filter_tree(tree.root_id(), filter);
    require(tree.filter_tree(tree.root_id(), filter) != too_large, "Oversize view not retained by row budget");
    tree.set_filter_cache_limits(1, 100);
    auto one = tree.filter_tree(tree.root_id(), filter);
    (void)tree.filter_tree(category, filter);
    require(tree.filter_tree(tree.root_id(), filter) != one, "Entry budget evicts prior view");
    tree.clear_filter_cache(); auto fresh = tree.filter_tree(tree.root_id(), filter); tree.clear_filter_cache();
    require(tree.filter_tree(tree.root_id(), filter) != fresh, "Cache clear releases retained view");
    tree.set_filter_cache_limits(0, 100); auto disabled = tree.filter_tree(tree.root_id(), filter);
    require(tree.filter_tree(tree.root_id(), filter) != disabled, "Zero entry budget disables caching");
}

void date_queries_navigation_and_search() {
    ExpenseTree tree{inr};
    const auto food = tree.add_group(tree.root_id(), "Food");
    const auto prior = tree.add_expense(food, "Prior", purchase(100, year{2026} / October / 9));
    const auto current = tree.add_expense(food, "Current", purchase(19800));
    const auto later = tree.add_expense(food, "Later", purchase(200, year{2026} / October / 11));
    const auto parts = tree.split(current, {
        {"Pizza", Money{15000, inr}, "Thin crust", {}}, {"Taco", Money{4800, inr}, "", {}}});
    const DateRange today{purchase_date, purchase_date};
    require(tree.total(food, today).minor_units() == 19800 && tree.total(parts[0], today).minor_units() == 15000 &&
        tree.total(prior, today).minor_units() == 0, "Date endpoints inclusive; items inherit dates");
    require(tree.total(food, DateRange{year{2026} / October / 9, purchase_date}).minor_units() == 19900, "Both endpoints included");
    QueryFilter filter; filter.dates = today;
    verify_queries(tree, tree.root_id(), filter, "Whole-tree date index");
    verify_queries(tree, food, filter, "Category date query");
    require(tree.children(food, 1, 1) == std::vector<NodeId>{current} && tree.children(food, 2, 100) == std::vector<NodeId>{later} &&
        tree.children(food, 100, 1).empty() && tree.children(food, 0, 0).empty(), "Pagination offsets and limits");
    require(tree.is_descendant(parts[0], food) && tree.is_descendant(food, food) && !tree.is_descendant(food, parts[0]),
        "Ancestry includes self and follows correct direction");
    require(tree.search(food, "Pizza") == std::vector<NodeId>{parts[0]}, "Search descendant names");
    require(tree.search(food, "pizza").empty(), "Search is case sensitive");
    require(tree.search(food, "Thin crust") == std::vector<NodeId>{parts[0]}, "Search item notes");
    require(tree.search(food, "Walmart") == std::vector<NodeId>{prior, current, later}, "Search purchase merchants");
    rejects<std::invalid_argument>([] { DateRange{year{2026} / February / 30, purchase_date}; }, "Reject invalid date bounds");
    rejects<std::invalid_argument>([] { DateRange{purchase_date, year{2026} / October / 9}; }, "Reject reversed bounds");
    filter.tags = {TagId{99999}};
    rejects<std::out_of_range>([&] { (void)tree.filtered_total(food, filter); }, "Reject unknown TagId filter");
    filter.tags.clear(); filter.account = AccountId{0};
    rejects<std::invalid_argument>([&] { (void)tree.filtered_total(food, filter); }, "Reject zero account filter");
    filter.account.reset(); filter.location = LocationId{0};
    rejects<std::invalid_argument>([&] { (void)tree.filtered_total(food, filter); }, "Reject zero location filter");
    filter.location.reset(); filter.tag_match = static_cast<TagMatch>(99);
    rejects<std::invalid_argument>([&] { (void)tree.filtered_total(food, filter); }, "Reject invalid tag-match policy");
}

void atomic_import_and_copy() {
    ExpenseTree tree{inr};
    const auto tag = tree.create_tag("Imported");
    const auto category = tree.add_group(tree.root_id(), "Import");
    const auto baseline = tree.add_expense(category, "Existing", purchase(5));
    const auto before = source_state(tree);
    auto invalid = purchase(10); invalid.account_id = AccountId{0};
    rejects<std::invalid_argument>([&] { tree.import_expenses({
        {category, "Valid first", purchase(10), {tag}}, {category, "Invalid later", invalid, {tag}}}); }, "Reject malformed later batch record");
    require(source_state(tree) == before, "Batch failure must not retain valid earlier records");
    rejects<std::out_of_range>([&] { tree.import_expenses({
        {category, "Valid", purchase(1), {}}, {NodeId{9999}, "Bad parent", purchase(1), {}}}); }, "All batch parents validated atomically");
    require(source_state(tree) == before, "Invalid batch parent leaves original unchanged");
    const auto imported = tree.import_expenses({
        {category, "One", purchase(10), {tag}}, {category, "Two", purchase(20), {tag}}});
    require(imported.size() == 2 && tree.total(category).minor_units() == 35 &&
        tree.total_by_tag(category, "Imported").minor_units() == 30, "Import creates nodes and memberships");
    const auto before_overflow = source_state(tree);
    rejects<std::overflow_error>([&] { tree.import_expenses({
        {category, "Valid", purchase(1), {}},
        {category, "Overflow", purchase(std::numeric_limits<std::int64_t>::max()), {}}}); },
        "Batch import rejects aggregate overflow after earlier valid entries");
    require(source_state(tree) == before_overflow, "Batch overflow leaves original source unchanged");
    QueryFilter filter; filter.tags = {tag};
    auto original_view = tree.filter_tree(tree.root_id(), filter);
    ExpenseTree copy = tree; copy.update_expense(imported[0], purchase(100)); copy.rename_tag(tag, "Copied");
    require(tree.total(category).minor_units() == 35 && copy.total(category).minor_units() == 125 &&
        tree.find_tag("Imported") == tag && copy.find_tag("Copied") == tag, "Copy owns independent nodes and registry");
    require(tree.filter_tree(tree.root_id(), filter) == original_view, "Copy edits cannot invalidate source cache");
    ExpenseTree assigned{inr}; assigned = tree; assigned.erase_subtree(baseline);
    require(tree.contains(baseline) && !assigned.contains(baseline), "Assignment owns independent nodes");
}

void deep_and_wide_trees() {
    ExpenseTree tree{inr};
    const auto tag = tree.create_tag("Deep");
    auto deepest = tree.root_id();
    for (int i = 0; i < 2048; ++i) deepest = tree.add_group(deepest, "Nested");
    tree.add_tag(deepest, tag); tree.add_expense(deepest, "Deep purchase", purchase(500));
    const auto wide = tree.add_group(tree.root_id(), "Many purchases");
    for (int i = 0; i < 2048; ++i) tree.add_expense(wide, "Purchase", purchase(1));
    QueryFilter filter; filter.tags = {tag};
    require(tree.total(tree.root_id()).minor_units() == 2548 && tree.filtered_total(tree.root_id(), filter).minor_units() == 500,
        "Deep and wide cached/indexed sums correct");
    auto view = tree.filter_tree(tree.root_id(), filter);
    require(view->nodes.size() == 2050 && view->nodes.front().amount_minor_units == 500, "Deep filter traverses iteratively");
    tree.move(wide, deepest);
    require(tree.total_by_tag(tree.root_id(), "Deep").minor_units() == 2548, "Subtree move refreshes inherited coverage");
    const auto first = tree.node(tree.root_id()).children.front();
    require(tree.erase_subtree(first).minor_units() == 2548 && tree.size() == 1 && tree.total(tree.root_id()).minor_units() == 0,
        "Iterative deletion includes deep and wide descendants and clears totals");
}

void varied_mutations_against_reference() {
    ExpenseTree tree{inr};
    const std::vector<TagId> tags{tree.create_tag("Recurring"), tree.create_tag("Work"), tree.create_tag("Holiday")};
    const std::vector<NodeId> groups{tree.add_group(tree.root_id(), "A", {tags[0]}),
        tree.add_group(tree.root_id(), "B", {tags[1]}), tree.add_group(tree.root_id(), "C")};
    std::mt19937 random{0xC0FFEE};
    for (int i = 0; i < 12; ++i) tree.add_expense(groups[static_cast<std::size_t>(i) % groups.size()], "Seed", purchase(i * 7),
        i % 2 == 0 ? std::vector<TagId>{tags[2]} : std::vector<TagId>{});
    for (int step = 0; step < 120; ++step) {
        const auto nodes = preorder(tree, tree.root_id());
        std::vector<NodeId> transactions, leaves, composites;
        for (const auto id : nodes) {
            const auto& node = tree.node(id);
            if (node.kind == NodeKind::transaction) transactions.push_back(id);
            if (node.kind == NodeKind::transaction || node.kind == NodeKind::item)
                (node.children.empty() ? leaves : composites).push_back(id);
        }
        const auto group = groups[random() % groups.size()];
        switch (random() % 8) {
        case 0:
            tree.add_expense(group, "Added", purchase(random() % 100), {tags[random() % tags.size()]});
            break;
        case 1:
            if (!leaves.empty()) {
                const auto id = leaves[random() % leaves.size()];
                if (tree.node(id).kind == NodeKind::item) tree.update_item(id, Money{static_cast<std::int64_t>(random() % 100), inr});
                else tree.update_expense(id, purchase(random() % 100));
            }
            break;
        case 2:
            if (!transactions.empty()) tree.move(transactions[random() % transactions.size()], group);
            break;
        case 3: {
            const auto id = nodes[random() % nodes.size()];
            const auto tag = tags[random() % tags.size()];
            if (tree.has_explicit_tag(id, tag)) tree.remove_tag(id, tag);
            else tree.add_tag(id, tag);
            break;
        }
        case 4:
            if (!leaves.empty()) {
                const auto id = leaves[random() % leaves.size()];
                const auto amount = tree.node(id).amount_minor_units;
                tree.split(id, {{"First", Money{amount / 2, inr}, "", {tags[random() % tags.size()]}},
                    {"Second", Money{amount - amount / 2, inr}, "", {}}});
            }
            break;
        case 5:
            if (!composites.empty()) tree.merge(composites[random() % composites.size()]);
            break;
        case 6:
            if (!transactions.empty()) {
                const auto id = transactions[random() % transactions.size()];
                auto corrected = tree.expense(id);
                corrected.date = year{2026} / October / day{static_cast<unsigned>(1 + random() % 20)};
                corrected.account_id = AccountId{7 + random() % 2};
                corrected.location_id = LocationId{3 + random() % 2};
                tree.update_expense(id, std::move(corrected));
            }
            break;
        case 7:
            if (transactions.size() > 3) tree.erase_subtree(transactions[random() % transactions.size()]);
            break;
        }
        const auto current = preorder(tree, tree.root_id());
        const std::vector<NodeId> roots{tree.root_id(), group, current[random() % current.size()]};
        for (const auto root : roots) {
            QueryFilter filter;
            verify_queries(tree, root, filter, "Mutation " + std::to_string(step) + " unfiltered");
            filter.tags = {tags[random() % tags.size()]};
            verify_queries(tree, root, filter, "Mutation " + std::to_string(step) + " tag");
            filter.tags = {tags[0], tags[1]}; filter.tag_match = step % 2 == 0 ? TagMatch::all : TagMatch::any;
            verify_queries(tree, root, filter, "Mutation " + std::to_string(step) + " tag combination");
            filter.dates = DateRange{year{2026} / October / 5, year{2026} / October / 15};
            if (step % 3 == 0) filter.account = AccountId{7};
            if (step % 5 == 0) filter.location = LocationId{3};
            verify_queries(tree, root, filter, "Mutation " + std::to_string(step) + " combined");
        }
    }
}
} // namespace

int main() {
    try {
        tree_workflow();
        grouping_and_item_moves();
        rejected_changes_preserve_tree();
        validation_and_overflow();
        tag_registry_and_sorted_storage();
        inheritance_and_combined_filters();
        filtered_views_and_cache();
        date_queries_navigation_and_search();
        atomic_import_and_copy();
        deep_and_wide_trees();
        varied_mutations_against_reference();
        std::cout << "Passed all 11 expense-tree test suites\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Expense-tree test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
