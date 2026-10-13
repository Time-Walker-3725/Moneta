#include "moneta/expenses/expense_history.hpp"
#include "moneta/storage/expense_store.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace moneta;
using namespace std::chrono;

namespace {

const Currency inr{"INR", 2};
const Date day = year{2026} / October / 13;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Action> void rejects(Action action, const std::string& message) {
    try { action(); }
    catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

Expense purchase(std::int64_t amount) {
    return Expense{Money{amount, inr}, day, AccountId{7}, LedgerEntryId{8},
        LocationId{9}, "Merchant, \"quoted\"\n日本", "Note\\with\nnewlines"};
}

std::string saved(const ExpenseTree& tree) {
    std::ostringstream output;
    save_tree(output, tree);
    return output.str();
}

ExpenseTree loaded(const std::string& bytes, LoadLimits limits = {}) {
    std::istringstream input{bytes};
    return load_tree(input, limits);
}

void roundtrip_and_validation() {
    ExpenseTree tree{inr, "Expenses\\\"\n日本"};
    const auto recurring = tree.create_tag("Recurring, \"purchase\"\n日本");
    const auto unused = tree.create_tag("Unused");
    tree.erase_tag(unused);
    const auto group = tree.add_group(tree.root_id(), "Home", {recurring});
    const auto transaction = tree.add_expense(group, "Receipt,\"\n日本", purchase(12345));
    const auto items = tree.split(transaction, {
        {"Meat", Money{7000, inr}, "Chicken\nBeef", {}},
        {"Other", Money{5345, inr}, "", {}}});
    tree.split(items[0], {{"Chicken", Money{5000, inr}, "", {}},
        {"Beef", Money{2000, inr}, "\"Local\", 日本", {}}});
    tree.group_items(transaction, items, "Unprocessed");
    tree.set_note(group, "Category\\\"note\n日本");
    auto optional = purchase(0);
    optional.ledger_entry_id.reset();
    optional.location_id.reset();
    const auto temporary = tree.add_expense(group, "Removed", optional);
    tree.erase_subtree(temporary);
    const auto zero = tree.add_expense(group, "Free", optional);

    const auto bytes = saved(tree);
    const auto revision = tree.revision();
    auto bad_counter = tree.snapshot();
    bad_counter.next_node_id = 0;
    rejects([&] { tree.restore(bad_counter); }, "Current node high-water must not conceal a corrupt snapshot counter");
    bad_counter = tree.snapshot();
    bad_counter.next_tag_id = 0;
    rejects([&] { tree.restore(bad_counter); }, "Current tag high-water must not conceal a corrupt snapshot counter");
    require(saved(tree) == bytes && tree.revision() == revision, "Rejected restore leaves data and revision untouched");
    auto restored = loaded(bytes);
    require(saved(restored) == bytes, "Save/load must preserve all authoritative records deterministically");
    require(restored.node(transaction).children == tree.node(transaction).children,
        "Child ordering and node IDs must survive loading");
    require(restored.total_by_tag(restored.root_id(), "Recurring, \"purchase\"\n日本") == Money{12345, inr},
        "Inherited tag queries must work after loading");
    require(!restored.expense(zero).location_id && !restored.expense(zero).ledger_entry_id,
        "Absent optional references must stay absent");
    const auto next_node = tree.snapshot().next_node_id;
    const auto next_tag = tree.snapshot().next_tag_id;
    require(restored.add_group(restored.root_id(), "New").value == next_node,
        "Deleted node IDs must remain retired after loading");
    require(restored.create_tag("New").value == next_tag,
        "Deleted tag IDs must remain retired after loading");

    // Caller formatting must not change canonical persisted numeric values.
    std::ostringstream formatted;
    formatted << std::hex << std::showpos;
    save_tree(formatted, tree);
    require(formatted.str() == bytes, "Persistence must use deterministic decimal numbers");
    require((formatted.flags() & std::ios::basefield) == std::ios::hex,
        "Persistence must restore caller formatting");

    for (const auto& currency : {Currency{"JPY", 0}, Currency{"KWD", 3}}) {
        ExpenseTree other{currency};
        other.add_expense(other.root_id(), "Different scale", Expense{Money{123, currency},
            day, AccountId{1}, std::nullopt, std::nullopt, "", ""});
        const auto other_bytes = saved(other);
        require(saved(loaded(other_bytes)) == other_bytes,
            "Persistence must preserve zero and three-decimal currency scales");
    }

    for (const auto& corrupt : {bytes.substr(0, bytes.size() / 2), bytes + "junk",
            std::string{"MONETA_EXPENSE_TREE 2\n"}})
        rejects([&] { (void)loaded(corrupt); }, "Reject truncated, extra, or unsupported data");
    auto limit = LoadLimits{};
    limit.max_nodes = tree.size() - 1;
    rejects([&] { (void)loaded(bytes, limit); }, "Enforce node limits before allocation");
    limit = {};
    limit.max_tags = 0;
    rejects([&] { (void)loaded(bytes, limit); }, "Enforce tag limits");
    limit = {};
    limit.max_references = 0;
    rejects([&] { (void)loaded(bytes, limit); }, "Enforce total reference limits");
    limit = {};
    limit.max_text_bytes = 1;
    rejects([&] { (void)loaded(bytes, limit); }, "Bound quoted text while reading");

    auto invalid_date = bytes;
    const auto date_position = invalid_date.find("2026 10 13");
    require(date_position != std::string::npos, "Fixture must contain date");
    invalid_date.replace(date_position, std::string{"2026 10 13"}.size(), "2026 2 30");
    rejects([&] { (void)loaded(invalid_date); }, "Reject invalid calendar dates");
    ExpenseTree empty{inr};
    auto invalid_graph = saved(empty);
    const std::string root_record{"NODE 1 0 \"Expenses\" 0 0 0 0 0"};
    const auto root_position = invalid_graph.find(root_record);
    require(root_position != std::string::npos, "Fixture must contain root record");
    invalid_graph.replace(root_position, root_record.size(), "NODE 1 0 \"Expenses\" 1 0 0 0 0");
    rejects([&] { (void)loaded(invalid_graph); }, "Loader must propagate graph validation");
    auto negative_count = saved(empty);
    const auto count_position = negative_count.find("NODES 1");
    negative_count.replace(count_position, std::string{"NODES 1"}.size(), "NODES -1");
    rejects([&] { (void)loaded(negative_count); }, "Reject negative unsigned counts");
    std::ostringstream broken;
    broken.setstate(std::ios::badbit);
    rejects([&] { save_tree(broken, tree); }, "Report output failures");
}

std::vector<std::vector<std::string>> parse_csv(const std::string& bytes) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto character = bytes[index];
        if (character == '"') {
            if (quoted && index + 1 < bytes.size() && bytes[index + 1] == '"') {
                field += '"';
                ++index;
            } else quoted = !quoted;
        } else if (!quoted && character == ',') {
            row.push_back(std::move(field));
            field.clear();
        } else if (!quoted && character == '\r') {
            require(index + 1 < bytes.size() && bytes[++index] == '\n', "CSV must use CRLF rows");
            row.push_back(std::move(field));
            field.clear();
            rows.push_back(std::move(row));
            row.clear();
        } else field += character;
    }
    require(!quoted && row.empty() && field.empty(), "CSV must have complete quoted fields and rows");
    return rows;
}

void csv_export() {
    ExpenseTree tree{inr};
    const auto tag = tree.create_tag("Recurring");
    const auto group = tree.add_group(tree.root_id(), "Category", {tag});
    const auto transaction = tree.add_expense(group, "Receipt,\"\n日本", purchase(100));
    const auto items = tree.split(transaction, {
        {"First,\"item\"", Money{40, inr}, "Note\n\"quoted\"", {}},
        {"Second", Money{60, inr}, "", {}}});
    const auto direct = tree.create_tag("Direct; tag");
    tree.add_tag(items[0], direct);
    tree.add_expense(tree.root_id(), "Unrelated", purchase(500));
    QueryFilter filter;
    filter.tags = {tag};
    filter.dates = DateRange{day, day};
    std::ostringstream output;
    export_expenses_csv(output, tree, tree.root_id(), filter);
    const auto rows = parse_csv(output.str());
    require(rows.size() == 3 && rows[0].size() == 16, "CSV must contain header and exactly matching leaves");
    std::int64_t total = 0;
    for (std::size_t index = 1; index < rows.size(); ++index) {
        const auto& row = rows[index];
        require(row.size() == 16, "CSV column count must survive embedded commas/newlines");
        require(row[0] == std::to_string(transaction.value), "Leaf must identify its owning transaction");
        require(row[3] == "INR" && row[4] == "2", "CSV must specify minor-unit currency scale");
        require(row[5] == "2026-10-13" && row[6] == "7" && row[7] == "9" && row[8] == "8",
            "CSV must preserve dates and accounting references");
        require(row[9] == purchase(1).merchant && row[10] == tree.node(transaction).name,
            "CSV escaping must preserve merchant and receipt name");
        require(row[12] == purchase(1).note, "CSV escaping must preserve transaction notes");
        require(row[15].find("9:Recurring") != std::string::npos,
            "Leaf CSV must include inherited tags");
        total += std::stoll(row[2]);
    }
    require(total == 100, "CSV must not count both purchase and split amounts");
    require(rows[1][11] == "First,\"item\"" && rows[1][13] == "Note\n\"quoted\"",
        "CSV must preserve leaf names and notes");
    require(rows[1][14] == "11:Direct; tag" && rows[2][14].empty(),
        "CSV must distinguish explicit leaf tags from inherited tags");
    std::ostringstream broken;
    broken.setstate(std::ios::badbit);
    rejects([&] { export_expenses_csv(broken, tree, tree.root_id()); }, "CSV output failures must throw");
}

void undo_redo() {
    ExpenseTree tree{inr};
    ExpenseHistory history{2};
    require(!history.undo(tree) && !history.redo(tree), "Empty history must be harmless");
    history.checkpoint(tree);
    const auto first = tree.add_expense(tree.root_id(), "First", purchase(100));
    const auto first_tag = tree.create_tag("First tag");
    tree.add_tag(first, first_tag);
    history.checkpoint(tree);
    tree.update_expense(first, purchase(200));
    const auto revision = tree.revision();
    require(history.undo(tree) && tree.total(tree.root_id()) == Money{100, inr}, "Undo must restore expense amount");
    require(tree.revision() > revision, "Undo must advance source revision");
    require(history.redo(tree) && tree.total(tree.root_id()) == Money{200, inr}, "Redo must restore edited amount");
    require(history.undo(tree) && history.undo(tree) && tree.size() == 1,
        "Two undos must restore the original tree");
    history.checkpoint(tree);
    const auto branch = tree.add_expense(tree.root_id(), "Branch", purchase(300));
    const auto branch_tag = tree.create_tag("Branch tag");
    require(branch.value > first.value && branch_tag.value > first_tag.value,
        "Undo followed by a branch must not reuse retired node/tag IDs");
    require(!history.can_redo(), "A new checkpoint must clear abandoned redo states");

    history.clear();
    for (int index = 0; index < 3; ++index) {
        history.checkpoint(tree);
        tree.rename(branch, std::to_string(index));
    }
    require(history.undo_size() == 2, "History must retain only configured capacity");
    require(history.undo(tree) && tree.node(branch).name == "1", "Newest checkpoint must be retained");
    require(history.undo(tree) && tree.node(branch).name == "0", "Second newest checkpoint must be retained");
    require(!history.undo(tree), "Oldest checkpoint must be evicted");
    history.set_capacity(0);
    history.checkpoint(tree);
    require(!history.can_undo() && !history.can_redo(), "Zero capacity must disable and release history");
}

void file_save() {
    const auto temporary = std::filesystem::temp_directory_path() /
        ("moneta-store-test-" + std::to_string(steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(temporary);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
    } cleanup{temporary};
    ExpenseTree tree{inr};
    tree.add_expense(tree.root_id(), "Purchase", purchase(100));
    const auto destination = temporary / "expenses.moneta";
    save_tree_file(destination, tree);
    require(saved(load_tree_file(destination)) == saved(tree), "File roundtrip must preserve authoritative tree");
    tree.rename(tree.root_id(), "Updated");
    save_tree_file(destination, tree);
    require(load_tree_file(destination).node(NodeId{1}).name == "Updated", "Atomic save must replace existing file");
    const auto protected_directory = temporary / "existing-directory";
    std::filesystem::create_directory(protected_directory);
    { std::ofstream marker{protected_directory / "marker"}; marker << "untouched"; }
    rejects([&] { save_tree_file(protected_directory, tree); }, "Rename failure must throw");
    require(std::filesystem::exists(protected_directory / "marker"),
        "Failed save must leave existing destination contents untouched");
    for (const auto& entry : std::filesystem::directory_iterator(temporary))
        require(entry.path().filename().string().find(".moneta-save-") != 0,
            "Successful and failed saves must remove temporary files");
    rejects([&] { save_tree_file(temporary / "missing-parent" / "file", tree); },
        "Missing destination directory must report failure");
    rejects([&] { (void)load_tree_file(temporary / "missing-file"); }, "Missing file must report failure");
}

} // namespace

int main() {
    try {
        roundtrip_and_validation();
        csv_export();
        undo_redo();
        file_save();
        std::cout << "Passed all 4 expense-store/history test suites\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "Expense-store test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
