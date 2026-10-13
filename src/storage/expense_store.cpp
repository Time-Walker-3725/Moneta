#include "moneta/storage/expense_store.hpp"

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

[[noreturn]] void invalid_file() {
    throw std::runtime_error("Invalid or truncated Moneta expense file");
}

// The on-disk format is independent of a caller's numeric locale/flags.
class StreamFormat {
public:
    explicit StreamFormat(std::ios& stream)
        : stream_(stream), flags_(stream.flags()), locale_(stream.getloc()),
          width_(stream.width()) {
        stream_.imbue(std::locale::classic());
        stream_.flags(std::ios::dec | std::ios::skipws);
        stream_.width(0);
    }
    ~StreamFormat() {
        try {
            stream_.flags(flags_);
            stream_.imbue(locale_);
            stream_.width(width_);
        } catch (...) { /* Destruction cannot mask an I/O failure. */ }
    }
private:
    std::ios& stream_;
    std::ios::fmtflags flags_;
    std::locale locale_;
    std::streamsize width_;
};

class Reader {
public:
    Reader(std::istream& input, LoadLimits limits) : input_(input), limits_(limits) {}

    void expect(std::string_view word) {
        input_ >> std::ws;
        for (const char character : word) {
            if (input_.get() != character) invalid_file();
        }
        const auto next = input_.peek();
        if (next != std::char_traits<char>::eof() &&
            next != ' ' && next != '\t' && next != '\r' && next != '\n') invalid_file();
    }

    template <typename Value> Value number() {
        input_ >> std::ws;
        if constexpr (std::is_unsigned_v<Value>) {
            if (input_.peek() == '-') invalid_file();
        }
        Value value{};
        if (!(input_ >> value)) invalid_file();
        return value;
    }

    std::size_t count(std::size_t maximum) {
        const auto result = number<std::uint64_t>();
        if (result > maximum || result > std::numeric_limits<std::size_t>::max())
            throw std::runtime_error("Moneta expense file exceeds load limits");
        return static_cast<std::size_t>(result);
    }

    std::size_t references(std::size_t maximum) {
        const auto result = count(std::min(maximum, limits_.max_references - references_));
        references_ += result;
        return result;
    }

    std::string quoted() {
        input_ >> std::ws;
        if (input_.get() != '"') invalid_file();
        std::string value;
        // Same delimiter/escape rules as std::quoted; bounded before appending
        // each byte rather than allocating an unbounded hostile string first.
        for (;;) {
            auto character = input_.get();
            if (character == std::char_traits<char>::eof()) invalid_file();
            if (character == '"') break;
            if (character == '\\') {
                character = input_.get();
                if (character == std::char_traits<char>::eof()) invalid_file();
            }
            if (text_bytes_ == limits_.max_text_bytes)
                throw std::runtime_error("Moneta expense file exceeds text limit");
            ++text_bytes_;
            value.push_back(static_cast<char>(character));
        }
        return value;
    }

    void finish() {
        expect("END");
        input_ >> std::ws;
        if (input_.bad() || input_.peek() != std::char_traits<char>::eof()) invalid_file();
    }
private:
    std::istream& input_;
    LoadLimits limits_;
    std::size_t references_{};
    std::size_t text_bytes_{};
};

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

class TemporaryDirectory {
public:
    explicit TemporaryDirectory(const std::filesystem::path& destination) {
        static std::atomic<std::uint64_t> serial{};
        const auto parent = destination.has_parent_path()
            ? destination.parent_path() : std::filesystem::path{"."};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
            path_ = parent / (".moneta-save-" + std::to_string(timestamp) + "-" +
                std::to_string(serial.fetch_add(1, std::memory_order_relaxed)));
            if (std::filesystem::create_directory(path_)) return;
        }
        throw std::runtime_error("Cannot create a unique temporary save directory");
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    std::filesystem::path file() const { return path_ / "tree.tmp"; }
private:
    std::filesystem::path path_;
};

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

ExpenseTree load_tree(std::istream& input, LoadLimits limits) {
    StreamFormat formatting{input};
    Reader reader{input, limits};
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
    reader.finish();
    return ExpenseTree::from_snapshot(std::move(snapshot));
}

void save_tree_file(const std::filesystem::path& path, const ExpenseTree& tree) {
    if (path.empty() || path.filename().empty())
        throw std::invalid_argument("Expense file requires a filename");
    TemporaryDirectory temporary{path};
    const auto file = temporary.file();
    std::ofstream output{file, std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error("Cannot open temporary Moneta expense file");
    save_tree(output, tree);
    output.flush();
    if (!output) throw std::runtime_error("Cannot flush temporary Moneta expense file");
    output.close();
    if (!output) throw std::runtime_error("Cannot close temporary Moneta expense file");
    std::filesystem::rename(file, path);
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
