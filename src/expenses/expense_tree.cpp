#include "moneta/expenses/expense_tree.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <utility>

namespace moneta {

namespace {

std::string tag_key(std::string_view tag) {
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    auto trim = [&] {
        const auto first = tag.find_first_not_of(whitespace);
        if (first == std::string_view::npos) {
            tag = {};
        } else {
            tag = tag.substr(first, tag.find_last_not_of(whitespace) - first + 1);
        }
    };
    trim();
    if (!tag.empty() && tag.front() == '#') {
        tag.remove_prefix(1);
        trim();
    }
    std::string key{tag};
    for (auto& character : key) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return key;
}

} // namespace

ExpenseTree::ExpenseTree(Currency currency, std::string root_name)
    : currency_(std::move(currency)) {
    validate_name(root_name);
    nodes_.emplace(root_id().value, ExpenseNode{
        root_id(), NodeKind::root, std::move(root_name), std::nullopt, {},
        std::nullopt, std::nullopt, {}});
}

const ExpenseNode& ExpenseTree::node(NodeId id) const {
    return nodes_.at(id.value);
}

ExpenseNode& ExpenseTree::mutable_node(NodeId id) {
    return nodes_.at(id.value);
}

ExpenseNode& ExpenseTree::group_parent(NodeId id) {
    auto& parent = mutable_node(id);
    if (parent.kind != NodeKind::root && parent.kind != NodeKind::group) {
        throw std::invalid_argument("Parent must be the root or a group");
    }
    return parent;
}

ExpenseNode& ExpenseTree::transaction_node(NodeId id) {
    auto& transaction = mutable_node(id);
    if (transaction.kind != NodeKind::transaction) {
        throw std::invalid_argument("Node must be a transaction");
    }
    return transaction;
}

void ExpenseTree::validate_name(const std::string& name) {
    if (name.empty() || std::all_of(name.begin(), name.end(), [](unsigned char c) {
            return std::isspace(c) != 0;
        })) {
        throw std::invalid_argument("Node name cannot be empty or whitespace");
    }
}

void ExpenseTree::validate_amount(const Money& amount) const {
    if (amount.currency() != currency_ || amount.minor_units() < 0) {
        throw std::invalid_argument("Expense amount must be nonnegative and use the tree's currency and scale");
    }
}

void ExpenseTree::validate_expense(const Expense& expense) const {
    validate_amount(expense.amount);
    if (!expense.date.ok() || expense.account_id.value == 0 ||
        (expense.ledger_entry_id && expense.ledger_entry_id->value == 0) ||
        (expense.location_id && expense.location_id->value == 0)) {
        throw std::invalid_argument("Expense requires a valid date and nonzero reference IDs");
    }
}

NodeId ExpenseTree::allocate_id() {
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Expense tree exhausted its node IDs");
    }
    return NodeId{next_id_++};
}

NodeId ExpenseTree::insert(NodeId parent_id, ExpenseNode child) {
    auto& parent = group_parent(parent_id);
    const auto id = child.id;
    parent.children.push_back(id);
    try {
        nodes_.emplace(id.value, std::move(child));
    } catch (...) {
        parent.children.pop_back();
        throw;
    }
    return id;
}

NodeId ExpenseTree::add_group(NodeId parent, std::string name) {
    validate_name(name);
    group_parent(parent);
    return insert(parent, ExpenseNode{allocate_id(), NodeKind::group, std::move(name),
        parent, {}, std::nullopt, std::nullopt, {}});
}

NodeId ExpenseTree::add_expense(NodeId parent, std::string name, Expense expense) {
    validate_name(name);
    validate_expense(expense);
    group_parent(parent);
    return insert(parent, ExpenseNode{allocate_id(), NodeKind::transaction, std::move(name),
        parent, {}, std::move(expense), std::nullopt, {}});
}

void ExpenseTree::rename(NodeId id, std::string name) {
    validate_name(name);
    mutable_node(id).name = std::move(name);
}

void ExpenseTree::update_expense(NodeId id, Expense expense) {
    auto& transaction = transaction_node(id);
    validate_expense(expense);
    if (!transaction.children.empty() && expense.amount != transaction.expense->amount) {
        throw std::invalid_argument("Changing a split total requires merging the items first");
    }
    transaction.expense = std::move(expense);
}

void ExpenseTree::move(NodeId id, NodeId new_parent_id) {
    auto& child = mutable_node(id);
    if (child.kind == NodeKind::root || child.kind == NodeKind::item) {
        throw std::invalid_argument("The root and split items cannot be moved");
    }
    auto& new_parent = group_parent(new_parent_id);
    // Walk ancestors to reject both self-parenting and descendant-parenting.
    for (std::optional<NodeId> ancestor = new_parent_id; ancestor;
         ancestor = node(*ancestor).parent_id) {
        if (*ancestor == id) {
            throw std::invalid_argument("Move would create a cycle");
        }
    }
    if (child.parent_id == new_parent_id) {
        return;
    }
    auto& old_parent = mutable_node(*child.parent_id);
    // Allocate at the destination before unlinking the source.
    new_parent.children.push_back(id);
    std::erase(old_parent.children, id);
    child.parent_id = new_parent_id;
}

std::vector<NodeId> ExpenseTree::split(NodeId id, std::vector<ExpenseSplit> items) {
    auto& transaction = transaction_node(id);
    if (items.size() < 2) {
        throw std::invalid_argument("A split requires at least two items");
    }
    Money sum{0, currency_};
    for (const auto& item : items) {
        validate_name(item.name);
        validate_amount(item.amount);
        sum = sum + item.amount;
    }
    if (sum != transaction.expense->amount) {
        throw std::invalid_argument("Split items must sum to the original transaction amount");
    }

    std::vector<ExpenseNode> staged;
    std::vector<NodeId> new_ids;
    staged.reserve(items.size());
    new_ids.reserve(items.size());
    for (auto& item : items) {
        const auto item_id = allocate_id();
        new_ids.push_back(item_id);
        staged.push_back(ExpenseNode{item_id, NodeKind::item, std::move(item.name),
            id, {}, std::nullopt, std::move(item.amount), std::move(item.note)});
    }
    // Finish all potentially allocating work before replacing existing items.
    auto result = new_ids;
    std::size_t inserted = 0;
    try {
        for (auto& item : staged) {
            nodes_.emplace(item.id.value, std::move(item));
            ++inserted;
        }
    } catch (...) {
        for (std::size_t i = 0; i < inserted; ++i) {
            nodes_.erase(new_ids[i].value);
        }
        throw;
    }
    for (const auto old_id : transaction.children) {
        nodes_.erase(old_id.value);
    }
    transaction.children = std::move(new_ids);
    return result;
}

void ExpenseTree::merge(NodeId id) {
    auto& transaction = transaction_node(id);
    for (const auto item_id : transaction.children) {
        nodes_.erase(item_id.value);
    }
    transaction.children.clear();
}

Money ExpenseTree::erase_subtree(NodeId id) {
    const auto& subtree = node(id);
    if (subtree.kind == NodeKind::root || subtree.kind == NodeKind::item) {
        throw std::invalid_argument("Cannot erase the root or an individual split item");
    }
    const auto removed_total = total(id);
    const auto parent_id = *subtree.parent_id;
    std::vector<NodeId> to_remove{id};
    for (std::size_t i = 0; i < to_remove.size(); ++i) {
        const auto& children = node(to_remove[i]).children;
        to_remove.insert(to_remove.end(), children.begin(), children.end());
    }
    std::erase(mutable_node(parent_id).children, id);
    for (const auto removed : to_remove) {
        nodes_.erase(removed.value);
    }
    return removed_total;
}

Money ExpenseTree::total(NodeId subtree, std::optional<DateRange> range) const {
    Money sum{0, currency_};
    std::vector<NodeId> pending{subtree};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto& current = node(id);
        if (current.kind == NodeKind::transaction) {
            if (!range || range->contains(current.expense->date)) {
                sum = sum + current.expense->amount;
            }
        } else if (current.kind == NodeKind::item) {
            const auto& transaction = node(*current.parent_id);
            if (!range || range->contains(transaction.expense->date)) {
                sum = sum + *current.item_amount;
            }
        } else {
            pending.insert(pending.end(), current.children.begin(), current.children.end());
        }
    }
    return sum;
}

std::vector<NodeId> ExpenseTree::tagged_nodes(
    NodeId subtree, std::string_view tag, std::optional<DateRange> range) const {
    const auto key = tag_key(tag);
    if (key.empty()) {
        throw std::invalid_argument("Tag filter cannot be empty");
    }
    std::vector<NodeId> matches;
    std::vector<NodeId> pending{subtree};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        const auto& current = node(id);
        if (current.kind == NodeKind::root || current.kind == NodeKind::group) {
            // Reverse the stack insertion to visit children in display order.
            pending.insert(pending.end(), current.children.rbegin(), current.children.rend());
            continue;
        }
        const auto& expense = current.kind == NodeKind::item
            ? *node(*current.parent_id).expense : *current.expense;
        if ((!range || range->contains(expense.date)) &&
            std::any_of(expense.tags.begin(), expense.tags.end(), [&](const auto& stored_tag) {
                return tag_key(stored_tag) == key;
            })) {
            matches.push_back(id);
        }
        // Do not descend into a transaction's split items: that transaction
        // and its full recorded amount have already been considered once.
    }
    return matches;
}

std::vector<NodeId> ExpenseTree::transactions_by_tag(
    NodeId subtree, std::string_view tag, std::optional<DateRange> range) const {
    auto matches = tagged_nodes(subtree, tag, range);
    for (auto& id : matches) {
        const auto& current = node(id);
        if (current.kind == NodeKind::item) {
            id = *current.parent_id;
        }
    }
    return matches;
}

Money ExpenseTree::total_by_tag(
    NodeId subtree, std::string_view tag, std::optional<DateRange> range) const {
    Money sum{0, currency_};
    for (const auto id : tagged_nodes(subtree, tag, range)) {
        const auto& current = node(id);
        sum = sum + (current.kind == NodeKind::item
            ? *current.item_amount : current.expense->amount);
    }
    return sum;
}

} // namespace moneta
