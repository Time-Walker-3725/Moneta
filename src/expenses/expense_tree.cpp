#include "moneta/expenses/expense_tree.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace moneta {
namespace {

// O(1), exact integer arithmetic. Every cached amount remains nonnegative.
std::int64_t adjusted(std::int64_t amount, std::int64_t delta) {
    if ((delta > 0 && amount > std::numeric_limits<std::int64_t>::max() - delta) ||
        (delta < 0 && (delta == std::numeric_limits<std::int64_t>::min() || amount < -delta))) {
        throw std::overflow_error("Expense amount exceeds the nonnegative 64-bit range");
    }
    return amount + delta;
}

bool category(const ExpenseNode& node) {
    return node.kind == NodeKind::root || node.kind == NodeKind::group;
}

static_assert(std::is_nothrow_move_assignable_v<Expense>);

} // namespace

ExpenseTree::ExpenseTree(Currency currency, std::string root_name)
    : currency_(std::move(currency)) {
    validate_name(root_name);
    nodes_.emplace(1, ExpenseNode{root_id(), NodeKind::root, std::move(root_name),
        {}, {}, {}, 0, {}});
}

ExpenseTree::ExpenseTree(const ExpenseTree& other)
    : currency_(other.currency_), tags_(other.tags_), nodes_(other.nodes_),
      expenses_(other.expenses_), notes_(other.notes_), tag_nodes_(other.tag_nodes_),
      next_id_(other.next_id_), revision_(other.revision_), data_revision_(other.data_revision_),
      cache_entries_(other.cache_entries_), cache_rows_(other.cache_rows_) {}

ExpenseTree& ExpenseTree::operator=(const ExpenseTree& other) {
    if (this != &other) {
        ExpenseTree candidate{other};
        if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("Tree revision exhausted");
        }
        candidate.revision_ = revision_ + 1;
        *this = std::move(candidate);
    }
    return *this;
}

const ExpenseNode& ExpenseTree::node(NodeId id) const { return nodes_.at(id.value); }
ExpenseNode& ExpenseTree::mutable_node(NodeId id) { return nodes_.at(id.value); }
const Expense& ExpenseTree::expense(NodeId id) const { return expenses_.at(id.value); }

std::string_view ExpenseTree::note(NodeId id) const {
    const auto& record = node(id);
    if (record.kind == NodeKind::transaction) {
        return expense(id).note;
    }
    const auto found = notes_.find(id.value);
    return found == notes_.end() ? std::string_view{} : std::string_view{found->second};
}

std::vector<NodeId> ExpenseTree::children(NodeId id, std::size_t offset, std::size_t limit) const {
    const auto& ids = node(id).children;
    if (offset >= ids.size()) {
        return {};
    }
    const auto count = std::min(limit, ids.size() - offset);
    return {ids.begin() + static_cast<std::ptrdiff_t>(offset),
        ids.begin() + static_cast<std::ptrdiff_t>(offset + count)};
}

bool ExpenseTree::is_descendant(NodeId id, NodeId ancestor) const {
    (void)node(id);
    (void)node(ancestor);
    while (id.value != 0) {
        const auto& current = node(id);
        if (id == ancestor) {
            return true;
        }
        id = current.parent_id;
    }
    return false;
}

void ExpenseTree::validate_name(const std::string& name) {
    if (name.empty() || std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    })) {
        throw std::invalid_argument("Node name cannot be blank");
    }
}

void ExpenseTree::validate_amount(const Money& amount) const {
    if (amount.currency() != currency_ || amount.minor_units() < 0) {
        throw std::invalid_argument("Amount must be nonnegative and match the tree currency/scale");
    }
}

void ExpenseTree::validate_expense(const Expense& value) const {
    validate_amount(value.amount);
    if (!value.date.ok() || value.account_id.value == 0 ||
        (value.ledger_entry_id && value.ledger_entry_id->value == 0) ||
        (value.location_id && value.location_id->value == 0)) {
        throw std::invalid_argument("Expense requires a valid date and nonzero reference IDs");
    }
}

NodeId ExpenseTree::allocate_id() {
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Node ID space exhausted");
    }
    return NodeId{next_id_++};
}

// O(1) logical invalidation. Old indexes/views are reclaimed on replacement,
// not during ordinary edits; their potentially linear destruction is deferred.
void ExpenseTree::touch(bool data_changed) {
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        // Prevent wraparound matching an ancient cache generation.
        query_index_.reset();
        view_cache_.clear();
        revision_ = 1;
        data_revision_ = 1;
    } else {
        ++revision_;
        if (data_changed) {
            if (data_revision_ == std::numeric_limits<std::uint64_t>::max()) {
                query_index_.reset();
                data_revision_ = 1;
            } else {
                ++data_revision_;
            }
        }
    }
}

std::vector<TagId> ExpenseTree::normalize_tags(std::vector<TagId> ids) const {
    for (const auto id : ids) {
        tags_.get(id);
    }
    if (!std::is_sorted(ids.begin(), ids.end())) {
        std::sort(ids.begin(), ids.end());
    }
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

// O(t) expected. Stage additions and roll back allocation failures. Existing
// references are left intact, allowing set_tags to share this helper.
void ExpenseTree::attach_tags(NodeId id, const std::vector<TagId>& ids) {
    std::vector<TagId> added;
    added.reserve(ids.size());
    try {
        for (const auto tag_id : ids) {
            auto& posting = tag_nodes_[tag_id.value];
            if (posting.insert(id.value).second) {
                added.push_back(tag_id);
            }
        }
    } catch (...) {
        detach_tags(id, added);
        throw;
    }
}

void ExpenseTree::detach_tags(NodeId id, const std::vector<TagId>& ids) noexcept {
    for (const auto tag_id : ids) {
        auto found = tag_nodes_.find(tag_id.value);
        if (found != tag_nodes_.end()) {
            found->second.erase(id.value);
            if (found->second.empty()) {
                tag_nodes_.erase(found);
            }
        }
    }
}

TagId ExpenseTree::create_tag(std::string_view name) {
    if (const auto found = tags_.find(name)) {
        return *found;
    }
    const auto id = tags_.create(name);
    touch(false);
    return id;
}

std::optional<TagId> ExpenseTree::find_tag(std::string_view name) const { return tags_.find(name); }
const TagDefinition& ExpenseTree::tag(TagId id) const { return tags_.get(id); }

void ExpenseTree::rename_tag(TagId id, std::string_view name) {
    tags_.rename(id, name);
    touch(false);
}

void ExpenseTree::erase_tag(TagId id) {
    tags_.get(id);
    const auto found = tag_nodes_.find(id.value);
    if (found != tag_nodes_.end()) {
        for (const auto node_id : found->second) {
            auto& ids = nodes_.at(node_id).tags;
            const auto position = std::lower_bound(ids.begin(), ids.end(), id);
            ids.erase(position);
        }
        tag_nodes_.erase(found);
    }
    tags_.erase(id);
    touch(false);
}

void ExpenseTree::set_tags(NodeId id, std::vector<TagId> ids) {
    auto& current = mutable_node(id);
    ids = normalize_tags(std::move(ids));
    if (ids == current.tags) {
        return;
    }
    std::vector<TagId> removed;
    removed.reserve(current.tags.size());
    std::set_difference(current.tags.begin(), current.tags.end(), ids.begin(), ids.end(),
        std::back_inserter(removed));
    attach_tags(id, ids);
    detach_tags(id, removed);
    current.tags.swap(ids);
    touch(false);
}

void ExpenseTree::add_tag(NodeId id, TagId tag_id) {
    tags_.get(tag_id);
    auto ids = node(id).tags;
    const auto position = std::lower_bound(ids.begin(), ids.end(), tag_id);
    if (position == ids.end() || *position != tag_id) {
        ids.insert(position, tag_id);
        set_tags(id, std::move(ids));
    }
}

void ExpenseTree::remove_tag(NodeId id, TagId tag_id) {
    tags_.get(tag_id);
    auto ids = node(id).tags;
    const auto position = std::lower_bound(ids.begin(), ids.end(), tag_id);
    if (position != ids.end() && *position == tag_id) {
        ids.erase(position);
        set_tags(id, std::move(ids));
    }
}

bool ExpenseTree::has_explicit_tag(NodeId id, TagId tag_id) const {
    tags_.get(tag_id);
    const auto& ids = node(id).tags;
    return std::binary_search(ids.begin(), ids.end(), tag_id);
}

bool ExpenseTree::has_tag(NodeId id, TagId tag_id) const {
    (void)node(id);
    tags_.get(tag_id);
    while (id.value != 0) {
        const auto& current = node(id);
        if (std::binary_search(current.tags.begin(), current.tags.end(), tag_id)) {
            return true;
        }
        id = current.parent_id;
    }
    return false;
}

std::vector<TagId> ExpenseTree::effective_tags(NodeId id) const {
    (void)node(id);
    std::vector<TagId> result;
    while (id.value != 0) {
        const auto& current = node(id);
        result.insert(result.end(), current.tags.begin(), current.tags.end());
        id = current.parent_id;
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::vector<NodeId> ExpenseTree::explicit_nodes(TagId id) const {
    tags_.get(id);
    std::vector<NodeId> result;
    const auto found = tag_nodes_.find(id.value);
    if (found != tag_nodes_.end()) {
        result.reserve(found->second.size());
        for (const auto node_id : found->second) {
            result.push_back(NodeId{node_id});
        }
    }
    return result;
}

// O(h); all validation finishes before cached amounts are changed.
void ExpenseTree::validate_delta(NodeId id, std::int64_t delta) const {
    while (id.value != 0) {
        const auto& current = node(id);
        (void)adjusted(current.amount_minor_units, delta);
        id = current.parent_id;
    }
}

void ExpenseTree::apply_delta(NodeId id, std::int64_t delta) {
    while (id.value != 0) {
        auto& current = mutable_node(id);
        current.amount_minor_units += delta;
        id = current.parent_id;
    }
}

NodeId ExpenseTree::add_group(NodeId parent_id, std::string name, std::vector<TagId> ids) {
    validate_name(name);
    auto& parent = mutable_node(parent_id);
    if (!category(parent)) {
        throw std::invalid_argument("Category groups belong under root/groups; use group_items inside a purchase");
    }
    ids = normalize_tags(std::move(ids));
    const auto id = allocate_id();
    parent.children.push_back(id);
    try {
        nodes_.emplace(id.value, ExpenseNode{id, NodeKind::group, std::move(name),
            parent_id, {}, std::move(ids), 0, {}});
        attach_tags(id, node(id).tags);
    } catch (...) {
        const auto found = nodes_.find(id.value);
        if (found != nodes_.end()) {
            detach_tags(id, found->second.tags);
            nodes_.erase(found);
        }
        parent.children.pop_back();
        throw;
    }
    touch();
    return id;
}

NodeId ExpenseTree::add_expense(NodeId parent_id, std::string name, Expense value,
    std::vector<TagId> ids) {
    validate_name(name);
    validate_expense(value);
    auto& parent = mutable_node(parent_id);
    if (!category(parent)) {
        throw std::invalid_argument("Transactions belong under categories");
    }
    ids = normalize_tags(std::move(ids));
    const auto amount = value.amount.minor_units();
    validate_delta(parent_id, amount);
    const auto id = allocate_id();
    parent.children.push_back(id);
    try {
        expenses_.emplace(id.value, std::move(value));
        nodes_.emplace(id.value, ExpenseNode{id, NodeKind::transaction, std::move(name),
            parent_id, {}, std::move(ids), amount, id});
        attach_tags(id, node(id).tags);
    } catch (...) {
        const auto found = nodes_.find(id.value);
        if (found != nodes_.end()) {
            detach_tags(id, found->second.tags);
            nodes_.erase(found);
        }
        expenses_.erase(id.value);
        parent.children.pop_back();
        throw;
    }
    apply_delta(parent_id, amount);
    touch();
    return id;
}

void ExpenseTree::rename(NodeId id, std::string name) {
    validate_name(name);
    auto& current = mutable_node(id);
    if (current.name != name) {
        current.name.swap(name);
        touch(false);
    }
}

void ExpenseTree::set_note(NodeId id, std::string text) {
    const auto& current = node(id);
    if (note(id) == text) {
        return;
    }
    if (current.kind == NodeKind::transaction) {
        expenses_.at(id.value).note.swap(text);
    } else if (text.empty()) {
        notes_.erase(id.value);
    } else {
        notes_.insert_or_assign(id.value, std::move(text));
    }
    touch(false);
}

void ExpenseTree::update_expense(NodeId id, Expense value) {
    auto& current = mutable_node(id);
    if (current.kind != NodeKind::transaction) {
        throw std::invalid_argument("Expected a transaction");
    }
    validate_expense(value);
    const auto delta = value.amount.minor_units() - current.amount_minor_units;
    if (!current.children.empty() && delta != 0) {
        throw std::invalid_argument("Merge a split transaction before changing its recorded total");
    }
    if (delta != 0) {
        validate_delta(id, delta);
    }
    const auto& previous = expense(id);
    const bool data_changed = delta != 0 || previous.date != value.date ||
        previous.account_id != value.account_id || previous.location_id != value.location_id;
    expenses_.at(id.value) = std::move(value);
    if (delta != 0) {
        apply_delta(id, delta);
    }
    touch(data_changed);
}

void ExpenseTree::update_item(NodeId id, Money amount) {
    auto& current = mutable_node(id);
    if (current.kind != NodeKind::item || !current.children.empty()) {
        throw std::invalid_argument("Only monetary item leaves can have their amount edited");
    }
    validate_amount(amount);
    const auto delta = amount.minor_units() - current.amount_minor_units;
    if (delta == 0) {
        return;
    }
    validate_delta(id, delta);
    auto& purchase = expenses_.at(current.transaction_id.value);
    Money canonical{adjusted(purchase.amount.minor_units(), delta), currency_};
    purchase.amount = std::move(canonical);
    apply_delta(id, delta);
    touch();
}

void ExpenseTree::move(NodeId id, NodeId destination) {
    auto& current = mutable_node(id);
    auto& parent = mutable_node(destination);
    if (current.kind == NodeKind::root || is_descendant(destination, id)) {
        throw std::invalid_argument("Move would relocate the root or create a cycle");
    }
    if (current.kind == NodeKind::item) {
        if ((parent.kind != NodeKind::transaction && parent.kind != NodeKind::item) ||
            parent.transaction_id != current.transaction_id ||
            (parent.children.empty() && parent.amount_minor_units != 0)) {
            throw std::invalid_argument("Move items within the same purchase, into a composite or zero leaf");
        }
    } else if (!category(parent)) {
        throw std::invalid_argument("Categories and purchases move under categories");
    }
    if (current.parent_id == destination) {
        return;
    }
    std::vector<NodeId> old_path, new_path;
    for (auto cursor = current.parent_id; cursor.value != 0; cursor = node(cursor).parent_id) {
        old_path.push_back(cursor);
    }
    for (auto cursor = destination; cursor.value != 0; cursor = node(cursor).parent_id) {
        new_path.push_back(cursor);
    }
    auto old_count = old_path.size();
    auto new_count = new_path.size();
    while (old_count && new_count && old_path[old_count - 1] == new_path[new_count - 1]) {
        --old_count;
        --new_count;
    }
    const auto amount = current.amount_minor_units;
    for (std::size_t i = 0; i < old_count; ++i) {
        (void)adjusted(node(old_path[i]).amount_minor_units, -amount);
    }
    for (std::size_t i = 0; i < new_count; ++i) {
        (void)adjusted(node(new_path[i]).amount_minor_units, amount);
    }
    auto& old_parent = mutable_node(current.parent_id);
    parent.children.push_back(id); // Finish allocation before unlinking.
    std::erase(old_parent.children, id);
    current.parent_id = destination;
    for (std::size_t i = 0; i < old_count; ++i) {
        mutable_node(old_path[i]).amount_minor_units -= amount;
    }
    for (std::size_t i = 0; i < new_count; ++i) {
        mutable_node(new_path[i]).amount_minor_units += amount;
    }
    touch();
}

std::vector<NodeId> ExpenseTree::subtree_ids(NodeId id) const {
    (void)node(id);
    std::vector<NodeId> result{id};
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& children = node(result[i]).children;
        result.insert(result.end(), children.begin(), children.end());
    }
    return result;
}

void ExpenseTree::erase_records(const std::vector<NodeId>& ids) noexcept {
    for (const auto id : ids) {
        const auto found = nodes_.find(id.value);
        if (found != nodes_.end()) {
            detach_tags(id, found->second.tags);
            nodes_.erase(found);
        }
        expenses_.erase(id.value);
        notes_.erase(id.value);
    }
}

std::vector<NodeId> ExpenseTree::split(NodeId id, std::vector<ExpenseSplit> items) {
    auto& current = mutable_node(id);
    if ((current.kind != NodeKind::transaction && current.kind != NodeKind::item) || items.size() < 2) {
        throw std::invalid_argument("Split a purchase/item into at least two components");
    }
    std::int64_t sum = 0;
    for (auto& item : items) {
        validate_name(item.name);
        validate_amount(item.amount);
        item.tags = normalize_tags(std::move(item.tags));
        sum = adjusted(sum, item.amount.minor_units());
    }
    if (sum != current.amount_minor_units) {
        throw std::invalid_argument("Components must sum to the original amount");
    }
    auto old_ids = subtree_ids(id);
    old_ids.erase(old_ids.begin());
    std::vector<ExpenseNode> staged;
    std::vector<NodeNote> notes;
    std::vector<NodeId> children, inserted;
    staged.reserve(items.size());
    notes.reserve(items.size());
    children.reserve(items.size());
    inserted.reserve(items.size());
    for (auto& item : items) {
        const auto child_id = allocate_id();
        children.push_back(child_id);
        staged.push_back(ExpenseNode{child_id, NodeKind::item, std::move(item.name), id,
            {}, std::move(item.tags), item.amount.minor_units(), current.transaction_id});
        if (!item.note.empty()) {
            notes.push_back(NodeNote{child_id, std::move(item.note)});
        }
    }
    auto result = children;
    try {
        for (auto& child : staged) {
            const auto child_id = child.id;
            nodes_.emplace(child_id.value, std::move(child));
            inserted.push_back(child_id);
            attach_tags(child_id, node(child_id).tags);
        }
        for (auto& note : notes) {
            notes_.emplace(note.id.value, std::move(note.text));
        }
    } catch (...) {
        erase_records(inserted);
        throw;
    }
    current.children.swap(children);
    erase_records(old_ids);
    touch();
    return result;
}

void ExpenseTree::merge(NodeId id) {
    auto& current = mutable_node(id);
    if (current.kind != NodeKind::transaction && current.kind != NodeKind::item) {
        throw std::invalid_argument("Only purchases/items can be merged");
    }
    if (current.children.empty()) {
        return;
    }
    auto ids = subtree_ids(id);
    ids.erase(ids.begin());
    current.children.clear();
    erase_records(ids);
    touch();
}

NodeId ExpenseTree::group_items(NodeId parent_id, std::vector<NodeId> selected,
    std::string name, std::vector<TagId> ids) {
    auto& parent = mutable_node(parent_id);
    validate_name(name);
    if ((parent.kind != NodeKind::transaction && parent.kind != NodeKind::item) || selected.empty()) {
        throw std::invalid_argument("Group one or more sibling items inside a purchase");
    }
    ids = normalize_tags(std::move(ids));
    std::unordered_set<std::uint64_t> wanted;
    wanted.reserve(selected.size());
    for (const auto item : selected) {
        const auto& child = node(item);
        if (child.kind != NodeKind::item || child.parent_id != parent_id || !wanted.insert(item.value).second) {
            throw std::invalid_argument("Group unique sibling items");
        }
    }
    const auto id = allocate_id();
    std::vector<NodeId> grouped, replacement;
    grouped.reserve(selected.size());
    replacement.reserve(parent.children.size() - selected.size() + 1);
    std::int64_t sum = 0;
    bool placed = false;
    for (const auto child : parent.children) {
        if (wanted.contains(child.value)) {
            if (!placed) {
                replacement.push_back(id);
                placed = true;
            }
            grouped.push_back(child);
            sum = adjusted(sum, node(child).amount_minor_units);
        } else {
            replacement.push_back(child);
        }
    }
    nodes_.emplace(id.value, ExpenseNode{id, NodeKind::item, std::move(name), parent_id,
        std::move(grouped), std::move(ids), sum, parent.transaction_id});
    try {
        attach_tags(id, node(id).tags);
    } catch (...) {
        nodes_.erase(id.value);
        throw;
    }
    for (const auto child : node(id).children) {
        mutable_node(child).parent_id = id;
    }
    parent.children.swap(replacement);
    touch();
    return id;
}

void ExpenseTree::ungroup_items(NodeId id) {
    const auto& current = node(id);
    if (current.kind != NodeKind::item || current.children.empty()) {
        throw std::invalid_argument("Ungroup a composite item");
    }
    auto& parent = mutable_node(current.parent_id);
    std::vector<NodeId> replacement;
    replacement.reserve(parent.children.size() - 1 + current.children.size());
    for (const auto child : parent.children) {
        if (child == id) {
            replacement.insert(replacement.end(), current.children.begin(), current.children.end());
        } else {
            replacement.push_back(child);
        }
    }
    std::vector<std::vector<TagId>> merged, added;
    merged.reserve(current.children.size());
    added.reserve(current.children.size());
    for (const auto child_id : current.children) {
        const auto& old = node(child_id).tags;
        std::vector<TagId> next, difference;
        next.reserve(old.size() + current.tags.size());
        std::set_union(old.begin(), old.end(), current.tags.begin(), current.tags.end(), std::back_inserter(next));
        std::set_difference(next.begin(), next.end(), old.begin(), old.end(), std::back_inserter(difference));
        merged.push_back(std::move(next));
        added.push_back(std::move(difference));
    }
    std::size_t attached = 0;
    try {
        for (std::size_t i = 0; i < current.children.size(); ++i) {
            attach_tags(current.children[i], added[i]);
            ++attached;
        }
    } catch (...) {
        for (std::size_t i = 0; i < attached; ++i) {
            detach_tags(current.children[i], added[i]);
        }
        throw;
    }
    for (std::size_t i = 0; i < current.children.size(); ++i) {
        auto& child = mutable_node(current.children[i]);
        child.tags.swap(merged[i]);
        child.parent_id = current.parent_id;
    }
    parent.children.swap(replacement);
    detach_tags(id, current.tags);
    nodes_.erase(id.value);
    notes_.erase(id.value);
    touch();
}

Money ExpenseTree::erase_subtree(NodeId id) {
    const auto& current = node(id);
    if (current.kind == NodeKind::root) {
        throw std::invalid_argument("Cannot delete the root");
    }
    const auto parent_id = current.parent_id;
    const auto transaction_id = current.transaction_id;
    const auto amount = current.amount_minor_units;
    const bool is_item = current.kind == NodeKind::item;
    Money result{amount, currency_};
    auto ids = subtree_ids(id);
    validate_delta(parent_id, -amount);
    std::optional<Money> canonical;
    if (is_item) {
        canonical.emplace(adjusted(expense(transaction_id).amount.minor_units(), -amount), currency_);
    }
    if (canonical) {
        expenses_.at(transaction_id.value).amount = std::move(*canonical);
    }
    std::erase(mutable_node(parent_id).children, id);
    apply_delta(parent_id, -amount);
    erase_records(ids);
    touch();
    return result;
}

std::vector<NodeId> ExpenseTree::import_expenses(std::vector<NewExpense> batch) {
    if (batch.empty()) {
        return {};
    }
    ExpenseTree candidate{*this};
    std::vector<NodeId> ids;
    ids.reserve(batch.size());
    for (auto& record : batch) {
        ids.push_back(candidate.add_expense(record.parent, std::move(record.name),
            std::move(record.expense), std::move(record.tags)));
    }
    *this = std::move(candidate);
    return ids;
}

TreeSnapshot ExpenseTree::snapshot() const {
    TreeSnapshot result{currency_, next_id_, tags_.next_id(), tags_.definitions(), {}, {}, {}};
    result.nodes.reserve(nodes_.size());
    result.expenses.reserve(expenses_.size());
    result.notes.reserve(notes_.size());
    for (const auto& [id, record] : nodes_) {
        (void)id;
        result.nodes.push_back(record);
    }
    for (const auto& [id, record] : expenses_) {
        result.expenses.push_back(ExpenseRecord{NodeId{id}, record});
    }
    for (const auto& [id, text] : notes_) {
        result.notes.push_back(NodeNote{NodeId{id}, text});
    }
    auto by_id = [](const auto& left, const auto& right) { return left.id < right.id; };
    std::sort(result.nodes.begin(), result.nodes.end(), by_id);
    std::sort(result.expenses.begin(), result.expenses.end(), by_id);
    std::sort(result.notes.begin(), result.notes.end(), by_id);
    return result;
}

ExpenseTree ExpenseTree::from_snapshot(TreeSnapshot snapshot) {
    ExpenseTree tree{snapshot.currency};
    tree.tags_.restore(std::move(snapshot.tags), snapshot.next_tag_id);
    tree.nodes_.clear();
    tree.nodes_.reserve(snapshot.nodes.size());
    tree.expenses_.reserve(snapshot.expenses.size());
    std::uint64_t maximum_id = 0;
    for (auto& record : snapshot.nodes) {
        validate_name(record.name);
        if (record.id.value == 0 || record.amount_minor_units < 0 ||
            !std::is_sorted(record.tags.begin(), record.tags.end()) ||
            std::adjacent_find(record.tags.begin(), record.tags.end()) != record.tags.end() ||
            (record.kind != NodeKind::root && record.kind != NodeKind::group &&
                record.kind != NodeKind::transaction && record.kind != NodeKind::item)) {
            throw std::invalid_argument("Invalid stored node");
        }
        for (const auto id : record.tags) {
            (void)tree.tags_.get(id);
        }
        maximum_id = std::max(maximum_id, record.id.value);
        const auto id = record.id.value;
        if (!tree.nodes_.emplace(id, std::move(record)).second) {
            throw std::invalid_argument("Duplicate stored node ID");
        }
    }
    if (snapshot.next_node_id <= maximum_id || snapshot.next_node_id < 2 || !tree.contains(tree.root_id())) {
        throw std::invalid_argument("Invalid next node ID or missing root");
    }
    tree.next_id_ = snapshot.next_node_id;
    for (auto& record : snapshot.expenses) {
        tree.validate_expense(record.expense);
        if (tree.node(record.id).kind != NodeKind::transaction ||
            !tree.expenses_.emplace(record.id.value, std::move(record.expense)).second) {
            throw std::invalid_argument("Invalid/duplicate transaction record");
        }
    }
    for (auto& record : snapshot.notes) {
        if (tree.node(record.id).kind == NodeKind::transaction || record.text.empty() ||
            !tree.notes_.emplace(record.id.value, std::move(record.text)).second) {
            throw std::invalid_argument("Invalid/duplicate node note");
        }
    }
    struct Frame { NodeId id; NodeId parent; NodeId transaction; };
    std::vector<Frame> pending{{tree.root_id(), {}, {}}};
    std::vector<NodeId> preorder;
    preorder.reserve(tree.nodes_.size());
    std::unordered_set<std::uint64_t> visited;
    visited.reserve(tree.nodes_.size());
    while (!pending.empty()) {
        const auto frame = pending.back();
        pending.pop_back();
        auto& current = tree.mutable_node(frame.id);
        if (!visited.insert(frame.id.value).second || current.parent_id != frame.parent) {
            throw std::invalid_argument("Stored graph contains cycles, shared children, or inconsistent parents");
        }
        auto owner = frame.transaction;
        if (frame.id == tree.root_id()) {
            if (current.kind != NodeKind::root || frame.parent.value != 0) {
                throw std::invalid_argument("Invalid root");
            }
        } else if (current.kind == NodeKind::root) {
            throw std::invalid_argument("Only one root is allowed");
        } else if (current.kind == NodeKind::transaction) {
            if (owner.value != 0 || !category(tree.node(frame.parent)) || !tree.expenses_.contains(frame.id.value)) {
                throw std::invalid_argument("Invalid transaction placement/metadata");
            }
            owner = frame.id;
        } else if (current.kind == NodeKind::group) {
            if (owner.value != 0 || !category(tree.node(frame.parent))) {
                throw std::invalid_argument("Category nested inside a purchase");
            }
        } else if (owner.value == 0 || category(tree.node(frame.parent))) {
            throw std::invalid_argument("Item outside a purchase");
        }
        if (current.transaction_id != owner) {
            throw std::invalid_argument("Invalid transaction ownership");
        }
        preorder.push_back(frame.id);
        tree.attach_tags(frame.id, current.tags);
        for (auto it = current.children.rbegin(); it != current.children.rend(); ++it) {
            pending.push_back(Frame{*it, frame.id, owner});
        }
    }
    if (visited.size() != tree.nodes_.size()) {
        throw std::invalid_argument("Disconnected stored nodes");
    }
    for (auto it = preorder.rbegin(); it != preorder.rend(); ++it) {
        const auto& current = tree.node(*it);
        if (!current.children.empty() || category(current)) {
            std::int64_t sum = 0;
            for (const auto child : current.children) {
                sum = adjusted(sum, tree.node(child).amount_minor_units);
            }
            if (sum != current.amount_minor_units) {
                throw std::invalid_argument("Stored amount disagrees with child sums");
            }
        }
        if (current.kind == NodeKind::transaction &&
            tree.expense(current.id).amount.minor_units() != current.amount_minor_units) {
            throw std::invalid_argument("Stored purchase total disagrees with components");
        }
    }
    return tree;
}

void ExpenseTree::restore(TreeSnapshot snapshot) {
    auto candidate = from_snapshot(std::move(snapshot));
    candidate.next_id_ = std::max(candidate.next_id_, next_id_);
    candidate.tags_.next_id_ = std::max(candidate.tags_.next_id_, tags_.next_id());
    if (revision_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Tree revision exhausted");
    }
    candidate.revision_ = revision_ + 1;
    candidate.cache_entries_ = cache_entries_;
    candidate.cache_rows_ = cache_rows_;
    *this = std::move(candidate);
}

} // namespace moneta
