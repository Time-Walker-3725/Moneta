#include "file_io.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace moneta {
namespace {

using namespace storage_detail;

void csv_text(std::ostream& output, std::string_view value) {
    output.put('"');
    for (const char character : value) {
        if (character == '"') output.put('"');
        output.put(character);
    }
    output.put('"');
}

// Length-prefixed names make a tag-list field unambiguous even when a tag
// contains a separator, quote or newline. E.g. 4:Work12:Subscription.
std::string tag_names(const ExpenseTree& tree, const std::vector<TagId>& tags) {
    std::string result;
    for (const auto id : tags) {
        const auto& name = tree.tag(id).name;
        result += std::to_string(name.size());
        result += ':';
        result += name;
    }
    return result;
}

std::string date_text(Date date) {
    std::ostringstream result;
    result.imbue(std::locale::classic());
    const auto year = static_cast<int>(date.year());
    if (year < 0) result << '-';
    result << std::setfill('0') << std::setw(4) << (year < 0 ? -year : year)
           << '-' << std::setw(2) << static_cast<unsigned>(date.month())
           << '-' << std::setw(2) << static_cast<unsigned>(date.day());
    return result.str();
}

} // namespace

void save_tree(std::ostream& output, const ExpenseTree& tree) {
    StreamFormat formatting{output};
    const auto snapshot = tree.snapshot();
    output << "MONETA_EXPENSE_TREE 1\nCURRENCY " << std::quoted(snapshot.currency.code())
           << ' ' << static_cast<unsigned>(snapshot.currency.decimal_places())
           << "\nCOUNTERS " << snapshot.next_node_id << ' ' << snapshot.next_tag_id
           << "\nTAGS " << snapshot.tags.size() << '\n';
    for (const auto& tag : snapshot.tags) {
        output << "TAG " << tag.id.value << ' ' << std::quoted(tag.name)
               << ' ' << std::quoted(tag.key) << '\n';
    }
    output << "NODES " << snapshot.nodes.size() << '\n';
    for (const auto& node : snapshot.nodes) {
        output << "NODE " << node.id.value << ' ' << static_cast<unsigned>(node.kind)
               << ' ' << std::quoted(node.name) << ' ' << node.parent_id.value
               << ' ' << node.amount_minor_units << ' ' << node.transaction_id.value
               << ' ' << node.children.size();
        for (const auto child : node.children) output << ' ' << child.value;
        output << ' ' << node.tags.size();
        for (const auto tag : node.tags) output << ' ' << tag.value;
        output << '\n';
    }
    output << "EXPENSES " << snapshot.expenses.size() << '\n';
    for (const auto& record : snapshot.expenses) {
        const auto& expense = record.expense;
        output << "EXPENSE " << record.id.value << ' ' << expense.amount.minor_units()
               << ' ' << static_cast<int>(expense.date.year())
               << ' ' << static_cast<unsigned>(expense.date.month())
               << ' ' << static_cast<unsigned>(expense.date.day())
               << ' ' << expense.account_id.value
               << ' ' << (expense.ledger_entry_id ? expense.ledger_entry_id->value : 0)
               << ' ' << (expense.location_id ? expense.location_id->value : 0)
               << ' ' << std::quoted(expense.merchant) << ' ' << std::quoted(expense.note) << '\n';
    }
    output << "NOTES " << snapshot.notes.size() << '\n';
    for (const auto& note : snapshot.notes)
        output << "NOTE " << note.id.value << ' ' << std::quoted(note.text) << '\n';
    output << "END\n";
    if (!output) throw std::runtime_error("Failed to write Moneta expense file");
}

ExpenseTree storage_detail::read_tree_record(Reader& reader, LoadLimits limits) {
    reader.expect("MONETA_EXPENSE_TREE");
    if (reader.number<unsigned>() != 1)
        throw std::runtime_error("Unsupported Moneta expense file version");
    reader.expect("CURRENCY");
    auto code = reader.quoted();
    const auto scale = reader.number<unsigned>();
    if (scale > 9) invalid_file();
    TreeSnapshot snapshot{Currency{std::move(code), static_cast<std::uint8_t>(scale)},
        0, 0, {}, {}, {}, {}};
    reader.expect("COUNTERS");
    snapshot.next_node_id = reader.number<std::uint64_t>();
    snapshot.next_tag_id = reader.number<std::uint64_t>();
    reader.expect("TAGS");
    const auto tag_count = reader.count(limits.max_tags);
    snapshot.tags.reserve(tag_count);
    for (std::size_t index = 0; index < tag_count; ++index) {
        reader.expect("TAG");
        const auto id = reader.number<std::uint64_t>();
        auto name = reader.quoted();
        auto key = reader.quoted();
        snapshot.tags.push_back({TagId{id}, std::move(name), std::move(key)});
    }
    reader.expect("NODES");
    const auto node_count = reader.count(limits.max_nodes);
    snapshot.nodes.reserve(node_count);
    for (std::size_t index = 0; index < node_count; ++index) {
        reader.expect("NODE");
        const auto id = reader.number<std::uint64_t>();
        const auto kind = reader.number<unsigned>();
        if (kind > static_cast<unsigned>(NodeKind::item)) invalid_file();
        auto name = reader.quoted();
        const auto parent = reader.number<std::uint64_t>();
        const auto amount = reader.number<std::int64_t>();
        const auto transaction = reader.number<std::uint64_t>();
        ExpenseNode node{NodeId{id}, static_cast<NodeKind>(kind), std::move(name),
            NodeId{parent}, {}, {}, amount, NodeId{transaction}};
        const auto children = reader.references(node_count);
        node.children.reserve(children);
        for (std::size_t child = 0; child < children; ++child)
            node.children.push_back(NodeId{reader.number<std::uint64_t>()});
        const auto tags = reader.references(tag_count);
        node.tags.reserve(tags);
        for (std::size_t tag = 0; tag < tags; ++tag)
            node.tags.push_back(TagId{reader.number<std::uint64_t>()});
        snapshot.nodes.push_back(std::move(node));
    }
    reader.expect("EXPENSES");
    const auto expense_count = reader.count(node_count);
    snapshot.expenses.reserve(expense_count);
    for (std::size_t index = 0; index < expense_count; ++index) {
        reader.expect("EXPENSE");
        const auto id = reader.number<std::uint64_t>();
        const auto amount = reader.number<std::int64_t>();
        const auto year = reader.number<int>();
        const auto month = reader.number<unsigned>();
        const auto day = reader.number<unsigned>();
        if (year < -32767 || year > 32767 || month < 1 || month > 12 || day < 1 || day > 31)
            invalid_file();
        const auto account = reader.number<std::uint64_t>();
        const auto ledger = reader.number<std::uint64_t>();
        const auto location = reader.number<std::uint64_t>();
        auto merchant = reader.quoted();
        auto note = reader.quoted();
        Expense expense{Money{amount, snapshot.currency},
            Date{std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}},
            AccountId{account}, ledger ? std::optional{LedgerEntryId{ledger}} : std::nullopt,
            location ? std::optional{LocationId{location}} : std::nullopt,
            std::move(merchant), std::move(note)};
        snapshot.expenses.push_back({NodeId{id}, std::move(expense)});
    }
    reader.expect("NOTES");
    const auto note_count = reader.count(node_count);
    snapshot.notes.reserve(note_count);
    for (std::size_t index = 0; index < note_count; ++index) {
        reader.expect("NOTE");
        const auto id = reader.number<std::uint64_t>();
        snapshot.notes.push_back({NodeId{id}, reader.quoted()});
    }
    reader.expect("END");
    return ExpenseTree::from_snapshot(std::move(snapshot));
}

ExpenseTree load_tree(std::istream& input, LoadLimits limits) {
    StreamFormat formatting{input};
    Reader reader{input, limits};
    auto tree = read_tree_record(reader, limits);
    input >> std::ws;
    if (input.bad() || input.peek() != std::char_traits<char>::eof()) invalid_file();
    return tree;
}

void save_tree_file(const std::filesystem::path& path, const ExpenseTree& tree) {
    atomic_write(path, [&](std::ostream& out) { save_tree(out, tree); });
}

ExpenseTree load_tree_file(const std::filesystem::path& path, LoadLimits limits) {
    std::ifstream input{path, std::ios::binary};
    if (!input) throw std::runtime_error("Cannot open Moneta expense file");
    return load_tree(input, limits);
}

void export_expenses_csv(std::ostream& output, const ExpenseTree& tree,
    NodeId subtree, const QueryFilter& filter) {
    StreamFormat formatting{output};
    const auto leaves = tree.matching_leaves(subtree, filter);
    output << "transaction_id,leaf_id,amount_minor_units,currency,decimal_places,date,"
        "account_id,location_id,ledger_entry_id,merchant,transaction_name,leaf_name,"
        "transaction_note,leaf_note,explicit_tags_length_prefixed,effective_tags_length_prefixed\r\n";
    for (const auto id : leaves) {
        const auto& node = tree.node(id);
        const auto& expense = tree.expense(node.transaction_id);
        output << node.transaction_id.value << ',' << node.id.value << ','
               << node.amount_minor_units << ',';
        csv_text(output, tree.currency().code());
        output << ',' << static_cast<unsigned>(tree.currency().decimal_places()) << ',';
        csv_text(output, date_text(expense.date));
        output << ',' << expense.account_id.value << ',';
        if (expense.location_id) output << expense.location_id->value;
        output << ',';
        if (expense.ledger_entry_id) output << expense.ledger_entry_id->value;
        const auto explicit_tags = tag_names(tree, node.tags);
        const auto effective_tags = tag_names(tree, tree.effective_tags(id));
        for (const std::string_view value : {std::string_view{expense.merchant},
                std::string_view{tree.node(node.transaction_id).name},
                std::string_view{node.name}, std::string_view{expense.note}, tree.note(id),
                std::string_view{explicit_tags}, std::string_view{effective_tags}}) {
            output << ',';
            csv_text(output, value);
        }
        output << "\r\n";
    }
    if (!output) throw std::runtime_error("Failed to write Moneta expense CSV");
}

} // namespace moneta
