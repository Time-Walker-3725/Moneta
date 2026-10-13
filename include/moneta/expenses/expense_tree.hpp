#pragma once

#include "moneta/expenses/expense.hpp"
#include "moneta/expenses/expense_split.hpp"
#include "moneta/expenses/tag_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace moneta {

enum class NodeKind { root, group, transaction, item };
enum class TagMatch { any, all };

// Transaction metadata and nonempty notes are stored separately. Categories and
// items therefore do not reserve space for a complete Expense.
struct ExpenseNode {
    NodeId id;
    NodeKind kind;
    std::string name;
    NodeId parent_id; // Zero only for the root.
    std::vector<NodeId> children; // Display order is preserved.
    std::vector<TagId> tags; // Sorted, unique, explicit tags; never copied down.
    std::int64_t amount_minor_units{}; // Leaf amount or cached child sum.
    NodeId transaction_id; // Owning transaction; zero outside transactions.
};

struct QueryFilter {
    std::vector<TagId> tags;
    TagMatch tag_match{TagMatch::any};
    std::optional<DateRange> dates;
    std::optional<AccountId> account;
    std::optional<LocationId> location;
    bool operator==(const QueryFilter&) const = default;
};

struct FilteredNode {
    NodeId id;
    std::size_t parent_index{std::numeric_limits<std::size_t>::max()};
    std::int64_t amount_minor_units{};
};

// Flat preorder view. Parent indices refer to rows; IDs reference source records.
// Metadata is not copied. Matching zero-valued leaves and ancestors are retained.
struct FilteredTree {
    std::uint64_t revision{};
    NodeId root;
    std::vector<FilteredNode> nodes;
};

struct ExpenseRecord { NodeId id; Expense expense; };
struct NodeNote { NodeId id; std::string text; };
struct TreeSnapshot {
    Currency currency;
    std::uint64_t next_node_id{};
    std::uint64_t next_tag_id{};
    std::vector<TagDefinition> tags;
    std::vector<ExpenseNode> nodes;
    std::vector<ExpenseRecord> expenses;
    std::vector<NodeNote> notes;
};
struct NewExpense {
    NodeId parent;
    std::string name;
    Expense expense;
    std::vector<TagId> tags;
};

class ExpenseTree {
public:
    // n=nodes, h=height, d=affected parents' child counts, s=affected subtree nodes,
    // t=explicit tags/node, A=tags along an ancestor path, L=monetary leaves,
    // m=explicit roots for queried tags, r=merged leaf ranges, c=candidates,
    // k=returned rows. Hash bounds are expected; text adds its processed length.
    // This container is single-threaded; an application coordinates ledger writes.
    explicit ExpenseTree(Currency currency, std::string root_name = "Expenses");
    // O(n + stored tags/text); copy authoritative data, not query caches.
    ExpenseTree(const ExpenseTree&);
    ExpenseTree& operator=(const ExpenseTree&);
    ExpenseTree(ExpenseTree&&) noexcept = default;
    ExpenseTree& operator=(ExpenseTree&&) noexcept = default;

    // O(1).
    [[nodiscard]] NodeId root_id() const noexcept { return NodeId{1}; }
    [[nodiscard]] const Currency& currency() const noexcept { return currency_; }
    [[nodiscard]] std::size_t size() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool contains(NodeId id) const noexcept { return nodes_.contains(id.value); }
    // Expected O(1). References expire on deletion or tree replacement.
    [[nodiscard]] const ExpenseNode& node(NodeId id) const;
    [[nodiscard]] const Expense& expense(NodeId transaction) const;
    // Borrowed text expires when the note changes, the node is erased, or the
    // tree is replaced. Copy it when an independent lifetime is required.
    [[nodiscard]] std::string_view note(NodeId id) const;
    // O(k) for copying a slice; offset indexing O(1).
    [[nodiscard]] std::vector<NodeId> children(NodeId id, std::size_t offset = 0,
        std::size_t limit = std::numeric_limits<std::size_t>::max()) const;
    // O(h), including self.
    [[nodiscard]] bool is_descendant(NodeId id, NodeId ancestor) const;

    // Registry operations: expected O(1) + name length, except erase_tag below.
    TagId create_tag(std::string_view name);
    [[nodiscard]] std::optional<TagId> find_tag(std::string_view name) const;
    [[nodiscard]] const TagDefinition& tag(TagId id) const;
    [[nodiscard]] const TagRegistry& tag_registry() const noexcept { return tags_; }
    void rename_tag(TagId id, std::string_view name);
    // O(m*t): detach only explicit references, not inherited descendants.
    void erase_tag(TagId id);
    // O(t log t) normalization + O(t) posting updates; all IDs must exist.
    void set_tags(NodeId id, std::vector<TagId> tags);
    // O(t) shifts/copies and posting updates; sorted input avoids re-sorting.
    void add_tag(NodeId id, TagId tag);
    void remove_tag(NodeId id, TagId tag);
    // O(log t) explicit check; O(h log t) inherited check.
    [[nodiscard]] bool has_explicit_tag(NodeId id, TagId tag) const;
    [[nodiscard]] bool has_tag(NodeId id, TagId tag) const;
    // O(h + A log A); sorted unique inherited+explicit IDs.
    [[nodiscard]] std::vector<TagId> effective_tags(NodeId id) const;
    // O(m), unspecified order; explicit roots only.
    [[nodiscard]] std::vector<NodeId> explicit_nodes(TagId tag) const;

    // Groups classify transactions. Nested purchase groups are composite items.
    // Add group amortized O(1)+tags; expense O(h)+tags.
    NodeId add_group(NodeId parent, std::string name, std::vector<TagId> tags = {});
    NodeId add_expense(NodeId parent, std::string name, Expense expense,
        std::vector<TagId> tags = {});
    // O(text length), no financial amount changes.
    void rename(NodeId id, std::string name);
    void set_note(NodeId id, std::string text);
    // O(h) amount correction, O(1) otherwise, plus metadata copy. Split totals
    // cannot change until merged; date/account metadata stays on the purchase.
    void update_expense(NodeId transaction, Expense expense);
    // O(h). Edit an item leaf and adjust its purchase's canonical amount.
    void update_item(NodeId item, Money amount);
    // O(h+d). Items move within the SAME transaction; categories/transactions
    // move among category groups. New-path inheritance is automatic.
    void move(NodeId id, NodeId new_parent);
    // O(s+p+removed tag references+new tag normalization), p=new items.
    // Split a purchase or nested item, preserving sum.
    std::vector<NodeId> split(NodeId id, std::vector<ExpenseSplit> items);
    // O(s+removed tag references). Preserve amount; remove descendant labels/notes.
    void merge(NodeId id);
    // O(d+p+tags). Group sibling items without replacing their IDs.
    NodeId group_items(NodeId parent, std::vector<NodeId> items, std::string name,
        std::vector<TagId> tags = {});
    // O(d+p+sum child tags+p*group tags). Preserve group tags on children.
    void ungroup_items(NodeId id);
    // O(s+h+d+removed tags). Root deletion forbidden. Item deletion corrects
    // its owning purchase's canonical amount.
    Money erase_subtree(NodeId id);
    // O(n+stored text/tags+batch*h). Atomic batch through an explicit copy.
    std::vector<NodeId> import_expenses(std::vector<NewExpense> batch);

    // O(1) without dates; otherwise delegates to filtered_total.
    [[nodiscard]] Money total(NodeId subtree, std::optional<DateRange> range = std::nullopt) const;
    // First structural index O(n); first date index O(L log L). Warm tag-only
    // sum O(m log m+r), using leaf prefixes; whole-tree date-only O(log L).
    // Combined predicates scan candidates, worst O(L), plus range preparation.
    // Monetary-leaf scopes bypass indexes: O(u*h*log(t+1)) inherited tag checks.
    // Empty scopes/no purchases/unused tag coverage bypass index construction.
    [[nodiscard]] Money filtered_total(NodeId subtree, const QueryFilter& filter = {}) const;
    // O(c+k), plus index/range preparation; IDs follow tree order.
    [[nodiscard]] std::vector<NodeId> matching_leaves(NodeId subtree, const QueryFilter& filter = {}) const;
    [[nodiscard]] std::vector<NodeId> transactions(NodeId subtree, const QueryFilter& filter = {}) const;
    // Name resolution then query cost. Unknown tag empty/zero; blank invalid.
    [[nodiscard]] std::vector<NodeId> transactions_by_tag(NodeId subtree,
        std::string_view tag, std::optional<DateRange> range = std::nullopt) const;
    [[nodiscard]] Money total_by_tag(NodeId subtree, std::string_view tag,
        std::optional<DateRange> range = std::nullopt) const;
    // Miss O(s+c+m log m+u log u), plus cold O(n) index build; O(s+k) space.
    // Monetary-leaf scopes bypass indexes. Hit O(u log u+capacity*(u+1));
    // returns a shared immutable view with no row copying.
    [[nodiscard]] std::shared_ptr<const FilteredTree> filter_tree(
        NodeId subtree, const QueryFilter& filter = {}) const;
    // Row budget: oversize views are returned without caching. Reclamation
    // costs O(evicted rows), unless another shared_ptr retains them.
    void set_filter_cache_limits(std::size_t entries, std::size_t rows);
    void clear_filter_cache() const;
    // O(visited nodes+B*P) worst case, B=inspected bytes, P=pattern length;
    // case-sensitive substring search.
    [[nodiscard]] std::vector<NodeId> search(NodeId subtree, std::string_view text) const;

    // O(n+tags/text), plus O(n log n) sorting for deterministic ID-order output.
    [[nodiscard]] TreeSnapshot snapshot() const;
    // Expected O(n+tag references+text). Validate graph, totals, and ownership.
    [[nodiscard]] static ExpenseTree from_snapshot(TreeSnapshot snapshot);
    // Same validation cost; atomic replacement and revision increment.
    void restore(TreeSnapshot snapshot);

private:
    friend struct ExpenseTreeQueryAccess;
    struct QueryIndex;
    struct CachedView {
        NodeId root;
        QueryFilter filter;
        std::uint64_t revision;
        std::shared_ptr<const FilteredTree> value;
    };
    Currency currency_;
    TagRegistry tags_;
    std::unordered_map<std::uint64_t, ExpenseNode> nodes_;
    std::unordered_map<std::uint64_t, Expense> expenses_;
    std::unordered_map<std::uint64_t, std::string> notes_;
    std::unordered_map<std::uint64_t, std::unordered_set<std::uint64_t>> tag_nodes_;
    std::uint64_t next_id_{2};
    std::uint64_t revision_{1};
    std::uint64_t data_revision_{1}; // Leaf topology/amount/date/account/location.
    mutable std::shared_ptr<QueryIndex> query_index_;
    mutable std::deque<CachedView> view_cache_;
    std::size_t cache_entries_{4};
    std::size_t cache_rows_{100'000};

    ExpenseNode& mutable_node(NodeId id);
    NodeId allocate_id();
    void touch(bool data_changed = true);
    void validate_amount(const Money& amount) const;
    void validate_expense(const Expense& expense) const;
    static void validate_name(const std::string& name);
    [[nodiscard]] std::vector<TagId> normalize_tags(std::vector<TagId> tags) const;
    [[nodiscard]] std::vector<NodeId> subtree_ids(NodeId id) const;
    void validate_delta(NodeId start, std::int64_t delta) const;
    void apply_delta(NodeId start, std::int64_t delta);
    void attach_tags(NodeId id, const std::vector<TagId>& tags);
    void detach_tags(NodeId id, const std::vector<TagId>& tags) noexcept;
    void erase_records(const std::vector<NodeId>& ids) noexcept;
};

} // namespace moneta
