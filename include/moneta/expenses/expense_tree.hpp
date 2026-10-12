#pragma once

#include "moneta/expenses/expense.hpp"
#include "moneta/expenses/expense_split.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace moneta {

enum class NodeKind { root, group, transaction, item };

// The tree exposes const nodes only. Mutation goes through validated operations.
struct ExpenseNode {
    NodeId id;
    NodeKind kind;
    std::string name;
    std::optional<NodeId> parent_id;
    std::vector<NodeId> children;
    std::optional<Expense> expense; // Set only for transactions.
    std::optional<Money> item_amount; // Set only for split items.
    std::string item_note; // Item-specific notes; transaction notes live in Expense.
};

class ExpenseTree {
public:
    // A tree has one currency. Convert explicitly before insertion or report
    // separate trees per currency; exchange-rate policy belongs outside the tree.
    explicit ExpenseTree(Currency currency, std::string root_name = "Expenses");

    [[nodiscard]] NodeId root_id() const noexcept { return NodeId{1}; }
    [[nodiscard]] const Currency& currency() const noexcept { return currency_; }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    // Throws out_of_range for an unknown ID. References expire on removal or
    // destruction; callers should keep IDs rather than pointers to nodes.
    [[nodiscard]] const ExpenseNode& node(NodeId id) const;

    NodeId add_group(NodeId parent, std::string name);
    NodeId add_expense(NodeId parent, std::string name, Expense expense);
    void rename(NodeId id, std::string name);
    void update_expense(NodeId transaction, Expense expense);
    void move(NodeId id, NodeId new_parent);

    // Requires >=2 nonnegative items whose sum equals the recorded expense.
    // Replacing a split creates fresh item IDs. Transaction ID/metadata survive.
    std::vector<NodeId> split(NodeId transaction, std::vector<ExpenseSplit> items);
    void merge(NodeId transaction);

    // Explicit deletion reduces totals. An application service must coordinate
    // corresponding ledger changes; this container does not change accounts.
    Money erase_subtree(NodeId id);

    // Counts a transaction once, whether split or unsplit. An item query uses
    // that item's amount and its transaction's date. Range endpoints are inclusive.
    [[nodiscard]] Money total(NodeId subtree, std::optional<DateRange> range = std::nullopt) const;

    // Match a complete tag, ignoring ASCII case, outer whitespace, and one
    // optional leading '#'. Querying an item returns its owning transaction ID.
    // Results follow tree order, and split transactions appear only once.
    [[nodiscard]] std::vector<NodeId> transactions_by_tag(
        NodeId subtree, std::string_view tag, std::optional<DateRange> range = std::nullopt) const;

    // Count matched spending once. For an item subtree, use only that item's
    // amount while matching its parent transaction's tags and date.
    [[nodiscard]] Money total_by_tag(
        NodeId subtree, std::string_view tag, std::optional<DateRange> range = std::nullopt) const;

private:
    Currency currency_;
    std::unordered_map<std::uint64_t, ExpenseNode> nodes_;
    std::uint64_t next_id_{2};

    ExpenseNode& mutable_node(NodeId id);
    ExpenseNode& group_parent(NodeId id);
    ExpenseNode& transaction_node(NodeId id);
    NodeId allocate_id();
    NodeId insert(NodeId parent, ExpenseNode node);
    void validate_expense(const Expense& expense) const;
    void validate_amount(const Money& amount) const;
    static void validate_name(const std::string& name);
    [[nodiscard]] std::vector<NodeId> tagged_nodes(
        NodeId subtree, std::string_view tag, std::optional<DateRange> range) const;
};

} // namespace moneta
