#pragma once

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

namespace moneta::storage_detail {
[[noreturn]] inline void invalid_file() {
    throw std::runtime_error("Invalid or truncated Moneta file");
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
            throw std::runtime_error("Moneta file exceeds load limits");
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
                throw std::runtime_error("Moneta file exceeds text limit");
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

template<class Writer>
void atomic_write(const std::filesystem::path& path, Writer write) {
    if (path.empty() || path.filename().empty())
        throw std::invalid_argument("Moneta file requires a filename");
    TemporaryDirectory temporary{path};
    std::ofstream output{temporary.file(), std::ios::binary | std::ios::trunc};
    if (!output) throw std::runtime_error("Cannot open temporary Moneta file");
    write(output);
    output.close();
    if (!output) throw std::runtime_error("Cannot close temporary Moneta file");
    std::filesystem::rename(temporary.file(), path);
}

ExpenseTree read_tree_record(Reader& reader, LoadLimits limits);
} // namespace moneta::storage_detail
