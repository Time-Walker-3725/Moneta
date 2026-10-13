#include "moneta/expenses/tag_registry.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace moneta;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Exception, typename Action>
void rejects(Action action, const std::string& message) {
    try {
        action();
    } catch (const Exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void normalization() {
    require(TagRegistry::normalize(" \t# Subscription\r\n") == "subscription",
        "Normalize whitespace, prefix and ASCII case");
    require(TagRegistry::normalize("##Work") == "#work", "Remove only one prefix");
    require(TagRegistry::normalize("#") == "", "A bare prefix is blank");
    require(TagRegistry::normalize("Travel  Abroad") == "travel  abroad",
        "Internal whitespace is preserved");
    require(TagRegistry::normalize("CAF\xC3\x89") == "caf\xC3\x89",
        "Non-ASCII bytes are preserved");

    TagRegistry tags;
    const auto subscription = tags.create(" \t#Subscription\n");
    require(subscription == TagId{1}, "IDs begin at one");
    require(tags.get(subscription).name == "Subscription", "Preserve display case");
    require(tags.get(subscription).key == "subscription", "Store normalized lookup key");
    require(tags.create("#SUBSCRIPTION") == subscription, "Equivalent names keep their ID");
    require(tags.size() == 1 && tags.next_id() == 2, "Duplicates do not allocate IDs");
    require(tags.find(" subscription ") == subscription, "Find accepts normalized aliases");
    require(!tags.find("missing") && !tags.find(" # \n"), "Absent and blank names do not match");
    rejects<std::invalid_argument>([&] { tags.create("\t # \n"); }, "Reject blank creation");
    rejects<std::invalid_argument>([&] { tags.create("##Work"); }, "Reject ambiguous repeated prefix");
    rejects<std::invalid_argument>([&] { tags.create("# #Work"); }, "Reject spaced repeated prefix");
    rejects<std::out_of_range>([&] { (void)tags.get(TagId{}); }, "Zero is invalid");
}

void stable_identity() {
    TagRegistry tags;
    const auto work = tags.create("Work");
    const auto holiday = tags.create("Holiday");
    tags.rename(work, "#Business");
    require(tags.find("business") == work && !tags.find("work"), "Rename keeps ID and replaces lookup");
    require(tags.get(work).name == "Business", "Rename changes display name");
    tags.rename(work, "BUSINESS");
    require(tags.get(work).name == "BUSINESS", "Same-key rename changes display case");
    rejects<std::invalid_argument>([&] { tags.rename(work, "HOLIDAY"); }, "Reject rename collision");
    rejects<std::invalid_argument>([&] { tags.rename(work, " # "); }, "Reject blank rename");
    rejects<std::invalid_argument>([&] { tags.rename(work, "##Work"); }, "Reject ambiguous renamed prefix");
    require(tags.get(work).name == "BUSINESS" && tags.find("Holiday") == holiday,
        "Rejected rename preserves both definitions");
    rejects<std::out_of_range>([&] { tags.rename(TagId{99}, "Unknown"); }, "Reject unknown rename");

    tags.erase(work);
    require(!tags.find("business"), "Erasing removes name lookup");
    rejects<std::out_of_range>([&] { (void)tags.get(work); }, "Erased ID is invalid");
    rejects<std::out_of_range>([&] { tags.erase(work); }, "Reject repeated erase");
    const auto replacement = tags.create("Business");
    require(replacement.value > holiday.value, "Erased IDs are not reused");
    const auto definitions = tags.definitions();
    require(definitions.size() == 2 && definitions[0].id == holiday &&
        definitions[1].id == replacement, "Export definitions in ID order");
}

void restore_validation() {
    TagRegistry tags;
    const auto original = tags.create("Original");
    const auto unchanged = [&] {
        require(tags.size() == 1 && tags.next_id() == 2 && tags.find("original") == original,
            "Failed restore must preserve the old registry");
    };
    const auto invalid_restore = [&](std::vector<TagDefinition> definitions,
                                     std::uint64_t next_id) {
        rejects<std::invalid_argument>([&] { tags.restore(std::move(definitions), next_id); },
            "Reject invalid restored registry");
        unchanged();
    };

    invalid_restore({}, 0);
    invalid_restore({{TagId{}, "Work", "work"}}, 2);
    invalid_restore({{TagId{2}, "Work", "work"}}, 2);
    invalid_restore({{TagId{1}, "Work", "work"}, {TagId{1}, "Travel", "travel"}}, 3);
    invalid_restore({{TagId{1}, "Work", "work"}, {TagId{2}, "WORK", "work"}}, 3);
    invalid_restore({{TagId{1}, "Work", "WORK"}}, 3);
    invalid_restore({{TagId{1}, " # ", ""}}, 3);
    invalid_restore({{TagId{1}, "#Work", "work"}}, 3);
    invalid_restore({{TagId{1}, " Work ", "work"}}, 3);

    tags.restore({{TagId{9}, "Holiday", "holiday"}, {TagId{3}, "Work", "work"}}, 12);
    require(tags.size() == 2 && !tags.find("original"), "Successful restore replaces registry");
    require(tags.find("#WORK") == TagId{3}, "Restore rebuilds lookup index");
    require(tags.definitions()[0].id == TagId{3}, "Restored exports remain ordered");
    require(tags.create("New") == TagId{12}, "Restore preserves the next available ID");

    TagRegistry copy;
    copy.restore(tags.definitions(), tags.next_id());
    require(copy.find("new") == TagId{12} && copy.next_id() == 13,
        "Definitions and next ID round-trip");
}

void id_exhaustion() {
    TagRegistry tags;
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    tags.restore({{TagId{maximum - 1}, "Last", "last"}}, maximum);
    require(tags.create("LAST") == TagId{maximum - 1}, "Existing lookup works at exhaustion");
    rejects<std::overflow_error>([&] { tags.create("Another"); }, "IDs cannot wrap around");
    require(tags.size() == 1 && tags.next_id() == maximum, "Overflow preserves registry");
    tags.erase(TagId{maximum - 1});
    rejects<std::overflow_error>([&] { tags.create("Last"); }, "Deletion does not reset exhausted IDs");
}

} // namespace

int main() {
    try {
        normalization();
        stable_identity();
        restore_validation();
        id_exhaustion();
        std::cout << "All four tag registry test suites passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
