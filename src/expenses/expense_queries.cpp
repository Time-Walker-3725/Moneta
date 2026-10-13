#include "moneta/expenses/expense_tree.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace moneta {

// O(n + L) space. Built only after a query needs subtree/leaf intervals.
struct ExpenseTree::QueryIndex {
    struct Span {
        std::size_t preorder;
        std::size_t preorder_end;
        std::size_t leaf_begin;
        std::size_t leaf_end;
    };
    struct Entry {
        NodeId id;
        std::size_t parent;
    };
    struct Leaf {
        NodeId id;
        NodeId transaction;
        std::int64_t amount;
        std::int64_t day;
        AccountId account;
        LocationId location; // Zero means no location.
    };
    struct DatedLeaf {
        std::int64_t day;
        std::size_t leaf;
    };

    std::uint64_t revision{};
    std::unordered_map<std::uint64_t, Span> spans;
    std::vector<Entry> preorder;
    std::vector<Leaf> leaves;
    std::vector<std::int64_t> prefix{0};
    bool dates_ready{};
    std::vector<DatedLeaf> dates;
    std::vector<std::int64_t> date_prefix;
};

struct ExpenseTreeQueryAccess {
    using Index = ExpenseTree::QueryIndex;
    struct Range {
        std::size_t begin;
        std::size_t end;
    };
    struct Predicates {
        std::int64_t first_day{std::numeric_limits<std::int64_t>::min()};
        std::int64_t last_day{std::numeric_limits<std::int64_t>::max()};
        AccountId account;
        LocationId location;
    };
    static constexpr auto no_parent = std::numeric_limits<std::size_t>::max();

    // O(1). Calendar validity is checked when transactions/ranges are created.
    static std::int64_t day(Date date) {
        return std::chrono::sys_days{date}.time_since_epoch().count();
    }

    // O(u log u), u=requested tags; expected O(u) registry validation.
    static QueryFilter normalized(const ExpenseTree& tree, const QueryFilter& filter) {
        if (filter.tag_match != TagMatch::any && filter.tag_match != TagMatch::all) {
            throw std::invalid_argument("Unknown tag matching mode");
        }
        if ((filter.account && filter.account->value == 0) ||
            (filter.location && filter.location->value == 0)) {
            throw std::invalid_argument("Query account and location IDs must be nonzero");
        }
        QueryFilter result = filter;
        if (!std::is_sorted(result.tags.begin(), result.tags.end())) {
            std::sort(result.tags.begin(), result.tags.end());
        }
        result.tags.erase(std::unique(result.tags.begin(), result.tags.end()), result.tags.end());
        for (TagId tag : result.tags) {
            (void)tree.tags_.get(tag);
        }
        // Any/all are equivalent when there is zero or one selected tag.
        if (result.tags.size() <= 1) {
            result.tag_match = TagMatch::any;
        }
        return result;
    }

    // Expected O(n) initial/rebuild cost, O(1) warm; iterative, O(h) DFS stack.
    static Index& index(const ExpenseTree& tree) {
        if (tree.query_index_ && tree.query_index_->revision == tree.data_revision_) {
            return *tree.query_index_;
        }
        auto replacement = std::make_shared<Index>();
        replacement->revision = tree.data_revision_;
        replacement->spans.reserve(tree.nodes_.size());
        replacement->preorder.reserve(tree.nodes_.size());
        replacement->leaves.reserve(tree.expenses_.size());
        replacement->prefix.reserve(tree.expenses_.size() + 1);
        struct Frame {
            NodeId id;
            std::size_t child{};
        };
        std::vector<Frame> stack;
        auto enter = [&](NodeId id, std::size_t parent) {
            const auto& node = tree.nodes_.at(id.value);
            const auto position = replacement->preorder.size();
            replacement->preorder.push_back({id, parent});
            replacement->spans.emplace(id.value, Index::Span{position, position + 1,
                replacement->leaves.size(), replacement->leaves.size()});
            if (node.children.empty() &&
                (node.kind == NodeKind::transaction || node.kind == NodeKind::item)) {
                const NodeId owner = node.kind == NodeKind::transaction ? id : node.transaction_id;
                const auto& expense = tree.expenses_.at(owner.value);
                replacement->leaves.push_back({id, owner, node.amount_minor_units,
                    day(expense.date), expense.account_id,
                    expense.location_id.value_or(LocationId{})});
                replacement->prefix.push_back(replacement->prefix.back() + node.amount_minor_units);
            }
            stack.push_back({id, 0});
        };
        enter(tree.root_id(), no_parent);
        while (!stack.empty()) {
            auto& frame = stack.back();
            const auto& node = tree.nodes_.at(frame.id.value);
            if (frame.child < node.children.size()) {
                const auto child = node.children[frame.child++];
                const auto parent = replacement->spans.at(frame.id.value).preorder;
                enter(child, parent);
            } else {
                auto& span = replacement->spans.at(frame.id.value);
                span.preorder_end = replacement->preorder.size();
                span.leaf_end = replacement->leaves.size();
                stack.pop_back();
            }
        }
        tree.query_index_ = std::move(replacement);
        return *tree.query_index_;
    }

    // O(L log L) once per revision; O(1) after the date index has been built.
    static void index_dates(Index& index) {
        if (index.dates_ready) {
            return;
        }
        std::vector<Index::DatedLeaf> dates;
        dates.reserve(index.leaves.size());
        for (std::size_t i = 0; i < index.leaves.size(); ++i) {
            dates.push_back({index.leaves[i].day, i});
        }
        std::sort(dates.begin(), dates.end(), [](const auto& left, const auto& right) {
            return left.day < right.day;
        });
        std::vector<std::int64_t> prefix;
        prefix.reserve(dates.size() + 1);
        prefix.push_back(0);
        for (const auto& record : dates) {
            prefix.push_back(prefix.back() + index.leaves[record.leaf].amount);
        }
        index.dates = std::move(dates);
        index.date_prefix = std::move(prefix);
        index.dates_ready = true;
    }

    // O(log L). Inclusive calendar endpoints become a half-open index interval.
    static Range date_range(const Index& index, const DateRange& dates) {
        const auto first = day(dates.first());
        const auto last = day(dates.last());
        const auto begin = std::lower_bound(index.dates.begin(), index.dates.end(), first,
            [](const auto& record, std::int64_t value) { return record.day < value; });
        const auto end = std::upper_bound(begin, index.dates.end(), last,
            [](std::int64_t value, const auto& record) { return value < record.day; });
        return {static_cast<std::size_t>(begin - index.dates.begin()),
            static_cast<std::size_t>(end - index.dates.begin())};
    }

    // O(a log a) time, O(a) temporary/output space; merge overlapping coverage.
    static std::vector<Range> merged(std::vector<Range> ranges) {
        std::sort(ranges.begin(), ranges.end(), [](Range left, Range right) {
            return left.begin < right.begin || (left.begin == right.begin && left.end < right.end);
        });
        std::size_t output = 0;
        for (Range range : ranges) {
            if (output != 0 && range.begin <= ranges[output - 1].end) {
                ranges[output - 1].end = std::max(ranges[output - 1].end, range.end);
            } else {
                ranges[output++] = range;
            }
        }
        ranges.resize(output);
        return ranges;
    }

    // Expected O(m) gathering, O(m log m + r) sorting/sweep. An explicit tag on
    // any ancestor covers all monetary leaves beneath it; repeated tags do not
    // multiply money. Intersecting with scope also includes tagged ancestors.
    static std::vector<Range> ranges(const ExpenseTree& tree, const Index& index,
        NodeId subtree, const QueryFilter& filter) {
        const auto& scope = index.spans.at(subtree.value);
        if (scope.leaf_begin == scope.leaf_end) {
            return {};
        }
        if (filter.tags.empty()) {
            return {{scope.leaf_begin, scope.leaf_end}};
        }
        std::vector<Range> result;
        struct Boundary {
            std::size_t position;
            bool start;
        };
        std::vector<Boundary> boundaries;
        for (TagId tag : filter.tags) {
            std::vector<Range> per_tag;
            if (const auto posting = tree.tag_nodes_.find(tag.value); posting != tree.tag_nodes_.end()) {
                per_tag.reserve(posting->second.size());
                for (auto id : posting->second) {
                    const auto& span = index.spans.at(id);
                    const auto begin = std::max(span.leaf_begin, scope.leaf_begin);
                    const auto end = std::min(span.leaf_end, scope.leaf_end);
                    if (begin < end) {
                        per_tag.push_back({begin, end});
                    }
                }
            }
            if (filter.tag_match == TagMatch::any) {
                result.insert(result.end(), per_tag.begin(), per_tag.end());
            } else {
                per_tag = merged(std::move(per_tag));
                if (per_tag.empty()) {
                    return {};
                }
                for (Range range : per_tag) {
                    boundaries.push_back({range.begin, true});
                    boundaries.push_back({range.end, false});
                }
            }
        }
        if (filter.tag_match == TagMatch::any) {
            return merged(std::move(result));
        }
        std::sort(boundaries.begin(), boundaries.end(), [](const auto& left, const auto& right) {
            return left.position < right.position;
        });
        std::size_t active = 0;
        std::size_t previous = boundaries.front().position;
        for (std::size_t i = 0; i < boundaries.size();) {
            const auto position = boundaries[i].position;
            if (active == filter.tags.size() && previous < position) {
                if (!result.empty() && result.back().end == previous) {
                    result.back().end = position;
                } else {
                    result.push_back({previous, position});
                }
            }
            std::size_t starts = 0;
            std::size_t ends = 0;
            do {
                boundaries[i].start ? ++starts : ++ends;
                ++i;
            } while (i < boundaries.size() && boundaries[i].position == position);
            active += starts;
            active -= ends;
            previous = position;
        }
        return result;
    }

    // O(1). Convert calendar endpoints once, outside potentially large scans.
    static Predicates predicates(const QueryFilter& filter) {
        Predicates result;
        if (filter.dates) {
            result.first_day = day(filter.dates->first());
            result.last_day = day(filter.dates->last());
        }
        result.account = filter.account.value_or(AccountId{});
        result.location = filter.location.value_or(LocationId{});
        return result;
    }

    // O(1). Tag coverage is handled by ranges, leaving numeric predicates.
    static bool accepts(const Index::Leaf& leaf, const Predicates& filter) {
        if (filter.account.value && leaf.account != filter.account) {
            return false;
        }
        if (filter.location.value && leaf.location != filter.location) {
            return false;
        }
        return leaf.day >= filter.first_day && leaf.day <= filter.last_day;
    }

    // O(1). Empty categories carry no monetary contribution.
    // O(u) expected necessary condition: unused tag definitions and trees with
    // no purchases cannot yield matches, so no global index needs building.
    static bool coverage_possible(const ExpenseTree& tree, const QueryFilter& filter) {
        if (tree.expenses_.empty()) {
            return false;
        }
        if (filter.tags.empty()) {
            return true;
        }
        for (const auto tag : filter.tags) {
            const auto posting = tree.tag_nodes_.find(tag.value);
            const bool exists = posting != tree.tag_nodes_.end() && !posting->second.empty();
            if (filter.tag_match == TagMatch::any && exists) return true;
            if (filter.tag_match == TagMatch::all && !exists) return false;
        }
        return filter.tag_match == TagMatch::all;
    }

    // O(1). Empty categories carry no monetary contribution.
    static bool monetary_leaf(const ExpenseNode& node) {
        return node.children.empty() &&
            (node.kind == NodeKind::transaction || node.kind == NodeKind::item);
    }

    // O(u*h*log(t+1)) worst case for inherited tags, O(1) otherwise. Direct
    // leaf scopes never need a global structural/date/tag-coverage index.
    static bool accepts_leaf(const ExpenseTree& tree, const ExpenseNode& node,
        const QueryFilter& filter) {
        const auto& metadata = tree.expenses_.at(node.transaction_id.value);
        if ((filter.account && metadata.account_id != *filter.account) ||
            (filter.location && metadata.location_id != filter.location) ||
            (filter.dates && !filter.dates->contains(metadata.date))) {
            return false;
        }
        if (filter.tags.empty()) {
            return true;
        }
        for (TagId tag : filter.tags) {
            const bool matches = tree.has_tag(node.id, tag);
            if (filter.tag_match == TagMatch::any && matches) {
                return true;
            }
            if (filter.tag_match == TagMatch::all && !matches) {
                return false;
            }
        }
        return filter.tag_match == TagMatch::all;
    }

    // O(log r), or O(1) for a single interval.
    static bool covered(std::size_t leaf, const std::vector<Range>& ranges) {
        const auto after = std::upper_bound(ranges.begin(), ranges.end(), leaf,
            [](std::size_t value, Range range) { return value < range.begin; });
        return after != ranges.begin() && leaf < std::prev(after)->end;
    }

    // O(c+k), c=leaves in the selected coverage; output stays in tree order.
    static std::vector<std::size_t> matching(const Index& index,
        const std::vector<Range>& ranges, const QueryFilter& filter) {
        std::vector<std::size_t> result;
        const auto prepared = predicates(filter);
        for (Range range : ranges) {
            for (auto i = range.begin; i < range.end; ++i) {
                if (accepts(index.leaves[i], prepared)) {
                    result.push_back(i);
                }
            }
        }
        return result;
    }

    // Normalized-filter query cost: O(m log m+r) coverage preparation, plus
    // O(c) scanning or O(D log(r+1)) cheaper date candidates; global date-only
    // totals use O(log L) prefix lookups. Lazy builds O(n) and O(L log L).
    // Direct leaf scope O(u*h*log(t+1)), without building any global index.
    static Money sum(const ExpenseTree& tree, NodeId subtree, const QueryFilter& filter) {
        const auto& source = tree.nodes_.at(subtree.value);
        if (filter.tags.empty() && !filter.dates && !filter.account && !filter.location) {
            return Money{source.amount_minor_units, tree.currency_};
        }
        if (!coverage_possible(tree, filter)) {
            return Money{0, tree.currency_};
        }
        if (monetary_leaf(source)) {
            return Money{accepts_leaf(tree, source, filter) ? source.amount_minor_units : 0, tree.currency_};
        }
        if (source.children.empty()) {
            return Money{0, tree.currency_}; // Empty category: no global index needed.
        }
        auto& indexed = index(tree);
        const auto selected = ranges(tree, indexed, subtree, filter);
        std::int64_t sum = 0;
        if (!filter.dates && !filter.account && !filter.location) {
            for (auto range : selected) {
                sum += indexed.prefix[range.end] - indexed.prefix[range.begin];
            }
            return Money{sum, tree.currency_};
        }
        if (selected.empty()) {
            return Money{0, tree.currency_};
        }
        const auto prepared = predicates(filter);
        if (filter.dates) {
            index_dates(indexed);
            const auto dates = date_range(indexed, *filter.dates);
            if (subtree == tree.root_id() && filter.tags.empty() && !filter.account && !filter.location) {
                return Money{indexed.date_prefix[dates.end] - indexed.date_prefix[dates.begin], tree.currency_};
            }
            std::size_t candidates = 0;
            for (auto range : selected) {
                candidates += range.end - range.begin;
            }
            const auto date_count = dates.end - dates.begin;
            const auto membership_cost = std::max<std::size_t>(1, std::bit_width(selected.size()));
            if (date_count <= candidates / membership_cost) {
                for (auto i = dates.begin; i < dates.end; ++i) {
                    const auto leaf = indexed.dates[i].leaf;
                    if (covered(leaf, selected) && accepts(indexed.leaves[leaf], prepared)) {
                        sum += indexed.leaves[leaf].amount;
                    }
                }
                return Money{sum, tree.currency_};
            }
        }
        for (auto range : selected) {
            for (auto i = range.begin; i < range.end; ++i) {
                if (accepts(indexed.leaves[i], prepared)) {
                    sum += indexed.leaves[i].amount;
                }
            }
        }
        return Money{sum, tree.currency_};
    }

    // O(c+k), plus coverage preparation/lazy O(n) structure. Owners occupy
    // contiguous leaf intervals, so deduplication needs no allocation/hash set.
    // Direct leaf scope O(u*h*log(t+1)), without building any global index.
    static std::vector<NodeId> transaction_ids(const ExpenseTree& tree,
        NodeId subtree, const QueryFilter& filter) {
        const auto& source = tree.nodes_.at(subtree.value);
        if (!coverage_possible(tree, filter)) {
            return {};
        }
        if (monetary_leaf(source)) {
            return accepts_leaf(tree, source, filter) ? std::vector<NodeId>{source.transaction_id}
                : std::vector<NodeId>{};
        }
        if (source.children.empty()) {
            return {};
        }
        const auto& indexed = index(tree);
        const auto selected = ranges(tree, indexed, subtree, filter);
        const auto prepared = predicates(filter);
        std::vector<NodeId> result;
        for (auto range : selected) {
            for (auto i = range.begin; i < range.end; ++i) {
                const auto& leaf = indexed.leaves[i];
                if (accepts(leaf, prepared) &&
                    (result.empty() || result.back() != leaf.transaction)) {
                    result.push_back(leaf.transaction);
                }
            }
        }
        return result;
    }

    // O(capacity + evicted rows); retained shared views delay their reclamation.
    static void trim_cache(const ExpenseTree& tree) {
        std::size_t rows = 0;
        for (const auto& entry : tree.view_cache_) {
            rows += entry.value->nodes.size();
        }
        while (!tree.view_cache_.empty() &&
            (tree.view_cache_.size() > tree.cache_entries_ || rows > tree.cache_rows_)) {
            rows -= tree.view_cache_.front().value->nodes.size();
            tree.view_cache_.pop_front();
        }
    }

    // O(cache_capacity + copied query tags + evicted rows). Oversize views are
    // returned without retention, and external shared owners survive eviction.
    static std::shared_ptr<const FilteredTree> retain_view(const ExpenseTree& tree,
        NodeId subtree, const QueryFilter& filter, std::shared_ptr<FilteredTree> result) {
        if (tree.cache_entries_ != 0 && result->nodes.size() <= tree.cache_rows_) {
            tree.view_cache_.push_back({subtree, filter, tree.revision_, result});
            trim_cache(tree);
        }
        return result;
    }
};

// O(1) all-time; dated queries have filtered_total's index/query costs.
Money ExpenseTree::total(NodeId subtree, std::optional<DateRange> range) const {
    if (!range) {
        return Money{node(subtree).amount_minor_units, currency_};
    }
    QueryFilter filter;
    filter.dates = range;
    return filtered_total(subtree, filter);
}

// O(u log u) normalization; lazy O(n) structure/O(L log L) date builds. Warm
// tag-only O(m log m+r), global date-only O(log L). Combined predicates choose
// coverage scanning O(c), or date candidates O(D log(r+1)); worst O(L) scan
// after range setup because the date route is used only when cheaper.
// Direct leaf scope O(u*h*log(t+1)), plus filter normalization.
Money ExpenseTree::filtered_total(NodeId subtree, const QueryFilter& requested) const {
    (void)node(subtree);
    const auto filter = ExpenseTreeQueryAccess::normalized(*this, requested);
    return ExpenseTreeQueryAccess::sum(*this, subtree, filter);
}

// O(c+k), plus O(u log u + m log m+r) preparation/lazy O(n) structural build.
// Direct leaf scope O(u*h*log(t+1)), plus filter normalization.
std::vector<NodeId> ExpenseTree::matching_leaves(NodeId subtree, const QueryFilter& requested) const {
    const auto& source = node(subtree);
    const auto filter = ExpenseTreeQueryAccess::normalized(*this, requested);
    if (!ExpenseTreeQueryAccess::coverage_possible(*this, filter)) {
        return {};
    }
    if (ExpenseTreeQueryAccess::monetary_leaf(source)) {
        return ExpenseTreeQueryAccess::accepts_leaf(*this, source, filter) ? std::vector<NodeId>{subtree}
            : std::vector<NodeId>{};
    }
    if (source.children.empty()) {
        return {};
    }
    const auto& index = ExpenseTreeQueryAccess::index(*this);
    const auto ranges = ExpenseTreeQueryAccess::ranges(*this, index, subtree, filter);
    const auto prepared = ExpenseTreeQueryAccess::predicates(filter);
    std::vector<NodeId> result;
    for (auto range : ranges) {
        for (auto i = range.begin; i < range.end; ++i) {
            if (ExpenseTreeQueryAccess::accepts(index.leaves[i], prepared)) {
                result.push_back(index.leaves[i].id);
            }
        }
    }
    return result;
}

// Same cost as matching_leaves; transaction owners are contiguous in tree order,
// so deduplication needs no hash set and item scopes return the owning purchase.
std::vector<NodeId> ExpenseTree::transactions(NodeId subtree, const QueryFilter& requested) const {
    (void)node(subtree);
    const auto filter = ExpenseTreeQueryAccess::normalized(*this, requested);
    return ExpenseTreeQueryAccess::transaction_ids(*this, subtree, filter);
}

// O(name length) resolution followed by transactions' query cost.
std::vector<NodeId> ExpenseTree::transactions_by_tag(NodeId subtree,
    std::string_view name, std::optional<DateRange> range) const {
    (void)node(subtree);
    const auto found = tags_.find(name);
    if (!found) {
        if (TagRegistry::normalize(name).empty()) {
            throw std::invalid_argument("A tag filter must not be blank");
        }
        return {};
    }
    QueryFilter filter;
    filter.tags = {*found};
    filter.dates = range;
    return ExpenseTreeQueryAccess::transaction_ids(*this, subtree, filter);
}

// O(name length) resolution followed by filtered_total's query cost.
Money ExpenseTree::total_by_tag(NodeId subtree,
    std::string_view name, std::optional<DateRange> range) const {
    (void)node(subtree);
    const auto found = tags_.find(name);
    if (!found) {
        if (TagRegistry::normalize(name).empty()) {
            throw std::invalid_argument("A tag filter must not be blank");
        }
        return Money{0, currency_};
    }
    QueryFilter filter;
    filter.tags = {*found};
    filter.dates = range;
    return ExpenseTreeQueryAccess::sum(*this, subtree, filter);
}

// Miss: O(s + c + m log m + u log u), lazy structural build O(n), and O(s+k)
// workspace/output. Hit: O(u log u + cache_capacity*u), excluding reclamation.
// Direct leaf miss O(u*h*log(t+1)), plus normalization/cache metadata work.
std::shared_ptr<const FilteredTree> ExpenseTree::filter_tree(
    NodeId subtree, const QueryFilter& requested) const {
    const auto& source = node(subtree);
    const auto filter = ExpenseTreeQueryAccess::normalized(*this, requested);
    for (auto cached = view_cache_.begin(); cached != view_cache_.end(); ++cached) {
        if (cached->root == subtree && cached->revision == revision_ && cached->filter == filter) {
            const auto value = cached->value;
            // Move only existing metadata/pointers; rotation allocates nothing.
            std::rotate(cached, std::next(cached), view_cache_.end());
            return value;
        }
    }
    const bool possible = ExpenseTreeQueryAccess::coverage_possible(*this, filter);
    if (source.children.empty() || !possible) {
        auto result = std::make_shared<FilteredTree>();
        result->revision = revision_;
        result->root = subtree;
        if (possible && ExpenseTreeQueryAccess::monetary_leaf(source) &&
            ExpenseTreeQueryAccess::accepts_leaf(*this, source, filter)) {
            result->nodes.push_back({subtree, ExpenseTreeQueryAccess::no_parent, source.amount_minor_units});
        }
        return ExpenseTreeQueryAccess::retain_view(*this, subtree, filter, std::move(result));
    }
    const auto& index = ExpenseTreeQueryAccess::index(*this);
    const auto ranges = ExpenseTreeQueryAccess::ranges(*this, index, subtree, filter);
    const auto matched = ExpenseTreeQueryAccess::matching(index, ranges, filter);
    const auto& scope = index.spans.at(subtree.value);
    const auto count = scope.preorder_end - scope.preorder;
    std::vector<std::int64_t> amounts(count, 0);
    std::vector<unsigned char> included(count, 0);
    for (auto leaf : matched) {
        const auto& record = index.leaves[leaf];
        const auto position = index.spans.at(record.id.value).preorder - scope.preorder;
        amounts[position] = record.amount;
        included[position] = 1; // A matching zero-valued leaf must remain visible.
    }
    for (auto i = scope.preorder_end; i-- > scope.preorder;) {
        const auto local = i - scope.preorder;
        const auto parent = index.preorder[i].parent;
        if (included[local] && parent != ExpenseTreeQueryAccess::no_parent && parent >= scope.preorder) {
            amounts[parent - scope.preorder] += amounts[local];
            included[parent - scope.preorder] = 1;
        }
    }
    auto result = std::make_shared<FilteredTree>();
    result->revision = revision_;
    result->root = subtree;
    result->nodes.reserve(static_cast<std::size_t>(std::count(included.begin(), included.end(),
        static_cast<unsigned char>(1))));
    std::vector<std::size_t> row_indices(count, ExpenseTreeQueryAccess::no_parent);
    for (auto i = scope.preorder; i < scope.preorder_end; ++i) {
        const auto local = i - scope.preorder;
        if (!included[local]) {
            continue;
        }
        const auto& entry = index.preorder[i];
        const auto parent = entry.parent == ExpenseTreeQueryAccess::no_parent || entry.parent < scope.preorder
            ? ExpenseTreeQueryAccess::no_parent : row_indices[entry.parent - scope.preorder];
        row_indices[local] = result->nodes.size();
        result->nodes.push_back({entry.id, parent, amounts[local]});
    }
    return ExpenseTreeQueryAccess::retain_view(*this, subtree, filter, std::move(result));
}

// O(cache_capacity + evicted rows); changes budgets without changing source data.
void ExpenseTree::set_filter_cache_limits(std::size_t entries, std::size_t rows) {
    cache_entries_ = entries;
    cache_rows_ = rows;
    ExpenseTreeQueryAccess::trim_cache(*this);
}

// O(cache entries + reclaimed rows); shared views held by callers remain valid.
void ExpenseTree::clear_filter_cache() const {
    view_cache_.clear();
}

// O(s+B*P) worst case for standard substring search, B=inspected bytes and
// P=query length; O(s) traversal/output space. Includes name, node note, and
// transaction merchant/note. Empty text matches every node.
std::vector<NodeId> ExpenseTree::search(NodeId subtree, std::string_view text) const {
    (void)node(subtree);
    std::vector<NodeId> stack{subtree};
    std::vector<NodeId> result;
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        const auto& current = nodes_.at(id.value);
        bool matches = current.name.find(text) != std::string::npos;
        if (!matches) {
            if (const auto note = notes_.find(id.value); note != notes_.end()) {
                matches = note->second.find(text) != std::string::npos;
            }
        }
        if (!matches && current.kind == NodeKind::transaction) {
            const auto& metadata = expenses_.at(id.value);
            matches = metadata.merchant.find(text) != std::string::npos ||
                metadata.note.find(text) != std::string::npos;
        }
        if (matches) {
            result.push_back(id);
        }
        stack.insert(stack.end(), current.children.rbegin(), current.children.rend());
    }
    return result;
}

} // namespace moneta
