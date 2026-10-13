#include "moneta/expenses/tag_registry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace moneta {
namespace {

bool ascii_space(char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' ||
        value == '\r' || value == '\f' || value == '\v';
}

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && ascii_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && ascii_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::string_view display_name(std::string_view name) noexcept {
    name = trim(name);
    if (!name.empty() && name.front() == '#') {
        name.remove_prefix(1);
    }
    return trim(name);
}

} // namespace

std::string TagRegistry::normalize(std::string_view name) {
    std::string key{display_name(name)};
    for (char& value : key) {
        if (value >= 'A' && value <= 'Z') {
            value = static_cast<char>(value - 'A' + 'a');
        }
    }
    return key;
}

TagId TagRegistry::create(std::string_view name) {
    auto key = normalize(name);
    if (key.empty()) {
        throw std::invalid_argument("A tag name must not be blank");
    }
    if (key.front() == '#') {
        throw std::invalid_argument("Use a single optional leading # on a tag name");
    }
    if (const auto found = ids_by_key_.find(key); found != ids_by_key_.end()) {
        return found->second;
    }
    // Keep a representable next ID strictly greater than every allocated ID.
    if (next_id_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("Tag IDs are exhausted");
    }

    const TagId id{next_id_};
    auto [entry, inserted] = definitions_.emplace(id.value,
        TagDefinition{id, std::string{display_name(name)}, std::move(key)});
    (void)inserted; // Monotonic IDs ensure this ID is absent.
    try {
        ids_by_key_.emplace(entry->second.key, id);
    } catch (...) {
        definitions_.erase(entry);
        throw;
    }
    ++next_id_;
    return id;
}

std::optional<TagId> TagRegistry::find(std::string_view name) const {
    const auto key = normalize(name);
    if (const auto found = ids_by_key_.find(key); found != ids_by_key_.end()) {
        return found->second;
    }
    return std::nullopt;
}

const TagDefinition& TagRegistry::get(TagId id) const {
    return definitions_.at(id.value);
}

void TagRegistry::rename(TagId id, std::string_view name) {
    auto& definition = definitions_.at(id.value);
    auto key = normalize(name);
    if (key.empty()) {
        throw std::invalid_argument("A tag name must not be blank");
    }
    if (key.front() == '#') {
        throw std::invalid_argument("Use a single optional leading # on a tag name");
    }
    const auto found = ids_by_key_.find(key);
    if (found != ids_by_key_.end() && found->second != id) {
        throw std::invalid_argument("A tag with this name already exists");
    }

    std::string display{display_name(name)};
    if (key == definition.key) {
        definition.name.swap(display);
        return;
    }

    // Allocate the replacement key before modifying the current definition.
    ids_by_key_.emplace(key, id);
    definition.name.swap(display);
    definition.key.swap(key); // key now holds the old lookup name.
    ids_by_key_.erase(key);
}

void TagRegistry::erase(TagId id) {
    const auto found = definitions_.find(id.value);
    if (found == definitions_.end()) {
        throw std::out_of_range("Unknown tag ID");
    }
    ids_by_key_.erase(found->second.key);
    definitions_.erase(found);
}

std::vector<TagDefinition> TagRegistry::definitions() const {
    std::vector<TagDefinition> result;
    result.reserve(definitions_.size());
    for (const auto& [id, definition] : definitions_) {
        (void)id;
        result.push_back(definition);
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id < right.id;
    });
    return result;
}

void TagRegistry::restore(std::vector<TagDefinition> definitions, std::uint64_t next_id) {
    if (next_id == 0) {
        throw std::invalid_argument("The next tag ID must be nonzero");
    }

    TagRegistry replacement;
    replacement.definitions_.reserve(definitions.size());
    replacement.ids_by_key_.reserve(definitions.size());
    for (auto& definition : definitions) {
        if (definition.id.value == 0 || definition.id.value >= next_id) {
            throw std::invalid_argument("Restored tag IDs must be nonzero and below the next ID");
        }
        if (definition.name.empty() || display_name(definition.name) != definition.name ||
            definition.name.front() == '#' || definition.key.empty() ||
            normalize(definition.name) != definition.key) {
            throw std::invalid_argument("A restored tag key must match its normalized name");
        }
        const TagId id = definition.id;
        if (!replacement.ids_by_key_.emplace(definition.key, id).second ||
            !replacement.definitions_.emplace(id.value, std::move(definition)).second) {
            throw std::invalid_argument("Restored tag IDs and keys must be unique");
        }
    }

    definitions_.swap(replacement.definitions_);
    ids_by_key_.swap(replacement.ids_by_key_);
    next_id_ = next_id;
}

} // namespace moneta
