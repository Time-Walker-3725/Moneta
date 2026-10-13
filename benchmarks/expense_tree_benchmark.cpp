#include "moneta/expenses/expense_tree.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace moneta;
using namespace std::chrono;

namespace {

constexpr std::size_t default_count = 100'000;
constexpr std::size_t maximum_count = 1'000'000;
constexpr std::size_t category_count = 64;
constexpr std::size_t total_repetitions = 10'000;

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

std::size_t parse_count(int argc, char** argv) {
    std::size_t count = default_count;
    if (argc == 1) {
        return count;
    }
    if (argc != 3 || std::string_view{argv[1]} != "--count") {
        throw std::invalid_argument("Usage: expense_tree_benchmark [--count N], 1 <= N <= 1000000");
    }
    const std::string_view text{argv[2]};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    if (error != std::errc{} || end != text.data() + text.size() || count == 0 || count > maximum_count) {
        throw std::invalid_argument("Count must be an integer from 1 through 1000000");
    }
    return count;
}

template <typename Action>
void timed(std::string_view label, Action action) {
    const auto start = steady_clock::now();
    action();
    const auto elapsed = duration<double, std::milli>{steady_clock::now() - start}.count();
    std::cout << std::left << std::setw(44) << label << std::right << std::fixed
        << std::setprecision(6) << elapsed << " ms\n";
}

std::int64_t amount_for(std::size_t index) {
    return static_cast<std::int64_t>(100 + index % 900);
}

Date date_for(std::size_t index) {
    return Date{sys_days{year{2026} / January / 1} + days{index % 365}};
}

std::size_t primary_tag(std::size_t index) {
    // Rotate by category round so every category contains mixed tags rather
    // than accidentally correlating index%64 categories with index%4 tags.
    return (index + index / category_count) % 4;
}

bool has_subscription(std::size_t index) {
    return primary_tag(index) == 0;
}

Expense purchase_for(std::size_t index, const Currency& currency) {
    return Expense{Money{amount_for(index), currency}, date_for(index),
        AccountId{1 + index % 8}, std::nullopt,
        index % 3 == 0 ? std::optional{LocationId{1 + index % 5}} : std::nullopt,
        "Merchant", ""};
}

void validate_view(const ExpenseTree& tree, const FilteredTree& view,
                   std::int64_t expected_total, std::size_t expected_leaves) {
    require(view.revision == tree.revision(), "View revision must match the source tree");
    require(view.root == tree.root_id(), "View must record its source root");
    if (view.nodes.empty()) {
        require(expected_leaves == 0 && expected_total == 0, "An empty view cannot omit matches");
        return;
    }
    require(view.nodes.front().id == tree.root_id(), "The first view row must be the root");
    require(view.nodes.front().amount_minor_units == expected_total, "View root total is incorrect");
    std::int64_t leaf_total = 0;
    std::size_t leaves = 0;
    for (std::size_t row = 0; row < view.nodes.size(); ++row) {
        const auto& filtered = view.nodes[row];
        if (row != 0) {
            require(filtered.parent_index < row, "View parents must precede their children");
        } else {
            require(filtered.parent_index == std::numeric_limits<std::size_t>::max(),
                "View root must have no parent row");
        }
        const auto& source = tree.node(filtered.id);
        if (source.kind == NodeKind::transaction && source.children.empty()) {
            require(filtered.amount_minor_units == source.amount_minor_units,
                "Matching unsplit leaves retain their amount");
            leaf_total += filtered.amount_minor_units;
            ++leaves;
        }
    }
    require(leaf_total == expected_total && leaves == expected_leaves,
        "Filtered leaves must agree with reference count and sum");
}

void run(std::size_t count) {
    const Currency currency{"INR", 2};
    const DateRange quarter{year{2026} / April / 1, year{2026} / June / 30};
    ExpenseTree tree{currency};
    const std::array<TagId, 4> tags{
        tree.create_tag("Subscription"), tree.create_tag("Work"),
        tree.create_tag("Holiday"), tree.create_tag("Recurring")};
    std::array<NodeId, category_count> categories;
    for (std::size_t index = 0; index < categories.size(); ++index) {
        categories[index] = tree.add_group(tree.root_id(), "Category " + std::to_string(index));
    }

    NodeId first_purchase;
    // Includes payload and record allocation. With fixed shallow depth, building
    // is expected O(n) amortized; small sorted tag vectors avoid per-tag strings.
    // IDs, metadata, postings and node records still incur real storage overhead.
    timed("Build transactions", [&] {
        for (std::size_t index = 0; index < count; ++index) {
            std::vector<TagId> explicit_tags{tags[primary_tag(index)]};
            if (index % 7 == 0) {
                explicit_tags.push_back(tags[1]);
            }
            const auto id = tree.add_expense(categories[index % categories.size()],
                "Purchase", purchase_for(index, currency), std::move(explicit_tags));
            if (index == 0) {
                first_purchase = id;
            }
        }
    });

    // Independent reference arithmetic stays outside measured query sections.
    std::int64_t expected_total = 0;
    std::int64_t expected_tag = 0;
    std::int64_t expected_date = 0;
    std::int64_t expected_combined = 0;
    std::size_t combined_leaves = 0;
    std::array<std::int64_t, category_count> category_totals{};
    for (std::size_t index = 0; index < count; ++index) {
        const auto amount = amount_for(index);
        expected_total += amount;
        category_totals[index % categories.size()] += amount;
        if (has_subscription(index)) {
            expected_tag += amount;
        }
        if (quarter.contains(date_for(index))) {
            expected_date += amount;
            if (has_subscription(index)) {
                expected_combined += amount;
                ++combined_leaves;
            }
        }
    }
    require(tree.size() == count + category_count + 1, "Node count is incorrect");

    std::cout << "Transactions: " << count << "; nodes: " << tree.size()
        << "; quarter subscription matches: " << combined_leaves << '\n';
    std::cout << "Single-run wall times; cold means the required index/view has not been built.\n";

    std::int64_t checksum = 0;
    // Cached ordinary root total is O(1); repetitions make the short operation measurable.
    timed("Cached ordinary total x10000", [&] {
        for (std::size_t iteration = 0; iteration < total_repetitions; ++iteration) {
            checksum += tree.total(tree.root_id()).minor_units();
        }
    });
    require(checksum == expected_total * static_cast<std::int64_t>(total_repetitions),
        "Cached ordinary total is incorrect");

    const QueryFilter tag_filter{{tags[0]}, TagMatch::any, std::nullopt, std::nullopt, std::nullopt};
    const QueryFilter date_filter{{}, TagMatch::any, quarter, std::nullopt, std::nullopt};
    const QueryFilter combined{{tags[0]}, TagMatch::any, quarter, std::nullopt, std::nullopt};
    std::int64_t result = 0;

    // The first tag query builds an O(n) leaf/subtree index. Warm work depends on
    // selected explicit roots: O(m log m + r), not a universal constant-time sum.
    timed("Tag total cold (structural index)", [&] {
        result = tree.filtered_total(tree.root_id(), tag_filter).minor_units();
    });
    require(result == expected_tag, "Cold tag total is incorrect");
    timed("Tag total warm", [&] {
        result = tree.filtered_total(tree.root_id(), tag_filter).minor_units();
    });
    require(result == expected_tag, "Warm tag total is incorrect");

    // Cold date work builds its sorted date/prefix index, O(L log L); structural
    // indexing is already warm. The whole-tree warm date sum uses O(log L) bounds.
    timed("Date total cold (date index)", [&] {
        result = tree.filtered_total(tree.root_id(), date_filter).minor_units();
    });
    require(result == expected_date, "Cold date total is incorrect");
    timed("Date total warm", [&] {
        result = tree.filtered_total(tree.root_id(), date_filter).minor_units();
    });
    require(result == expected_date, "Warm date total is incorrect");

    // Combined predicates inspect candidate leaves: worst O(L), plus tag ranges.
    timed("Tag + date total (indexes warm)", [&] {
        result = tree.filtered_total(tree.root_id(), combined).minor_units();
    });
    require(result == expected_combined, "Combined total is incorrect");

    const auto matches = tree.matching_leaves(tree.root_id(), combined);
    std::int64_t matching_total = 0;
    for (const auto id : matches) {
        matching_total += tree.node(id).amount_minor_units;
    }
    require(matches.size() == combined_leaves && matching_total == expected_combined,
        "Matching leaf IDs disagree with reference arithmetic");

    // View construction is O(n) worst case and allocates O(k) output rows. The
    // generous row budget is explicit here so both benchmark sizes test a hit.
    // A production application should choose its cache budget for its memory cap.
    tree.set_filter_cache_limits(2, count + category_count + 1);
    std::shared_ptr<const FilteredTree> cold_view;
    std::shared_ptr<const FilteredTree> warm_view;
    timed("Filtered view miss (indexes warm)", [&] { cold_view = tree.filter_tree(tree.root_id(), combined); });
    validate_view(tree, *cold_view, expected_combined, combined_leaves);
    timed("Filtered view cache hit", [&] { warm_view = tree.filter_tree(tree.root_id(), combined); });
    require(warm_view == cold_view, "Warm view must reuse the same immutable cached result");

    const auto previous_revision = tree.revision();
    auto replacement = tree.expense(first_purchase);
    constexpr std::int64_t correction = 17;
    replacement.amount = Money{amount_for(0) + correction, currency};
    timed("Edit first purchase amount", [&] { tree.update_expense(first_purchase, replacement); });
    timed("Move first purchase between categories", [&] { tree.move(first_purchase, categories[1]); });
    require(tree.revision() > previous_revision, "Edits must advance the tree revision");
    require(tree.total(tree.root_id()).minor_units() == expected_total + correction,
        "Editing and moving must preserve corrected root total");
    require(tree.total(categories[0]).minor_units() == category_totals[0] - amount_for(0),
        "Move must remove corrected purchase from its old category");
    require(tree.total(categories[1]).minor_units() == category_totals[1] + amount_for(0) + correction,
        "Move must add corrected purchase to its new category");

    // This backend invalidates lazy indexes on edits. The next indexed query can
    // therefore rebuild O(n) state; the mutation time alone omits that later work.
    timed("Combined total after edit/move", [&] {
        result = tree.filtered_total(tree.root_id(), combined).minor_units();
    });
    require(result == expected_combined, "January correction must not change the second-quarter sum");
    std::shared_ptr<const FilteredTree> refreshed;
    timed("Filtered view after edit/move", [&] { refreshed = tree.filter_tree(tree.root_id(), combined); });
    require(refreshed != cold_view, "Mutation must invalidate the old filtered view cache");
    validate_view(tree, *refreshed, expected_combined, combined_leaves);
    require(cold_view->revision == previous_revision, "Already returned views remain immutable snapshots");
    require(tree.filtered_total(tree.root_id(), tag_filter).minor_units() == expected_tag + correction,
        "Corrected amount must be reflected in the subscription report");

    std::shared_ptr<const FilteredTree> complete_view, complete_cached;
    timed("Complete view miss (indexes warm)", [&] {
        complete_view = tree.filter_tree(tree.root_id());
    });
    validate_view(tree, *complete_view, expected_total + correction, count);
    timed("Complete view cache hit", [&] {
        complete_cached = tree.filter_tree(tree.root_id());
    });
    require(complete_cached == complete_view, "Complete view must reuse its cached rows");

    const auto inherited = tree.create_tag("Inherited category");
    timed("Apply inherited category tag", [&] { tree.add_tag(categories[0], inherited); });
    QueryFilter inherited_filter;
    inherited_filter.tags = {inherited};
    timed("Inherited tag total (index reused)", [&] {
        result = tree.filtered_total(tree.root_id(), inherited_filter).minor_units();
    });
    require(result == category_totals[0] - amount_for(0), "Category tag must cover only current descendants");

    std::cout << "Validated all totals, matched leaves, view rows, cache reuse and invalidation.\n";
    std::cout << "Timings depend on compiler optimization, machine and workload; no latency guarantee.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        run(parse_count(argc, argv));
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
