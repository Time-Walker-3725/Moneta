#pragma once

#include <sqlite3.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace moneta::storage_detail {
inline void sql_check(sqlite3* db, int rc) {
    if (rc != SQLITE_OK) throw std::runtime_error(sqlite3_errmsg(db));
}
using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)>;

inline Connection connect(const std::filesystem::path& path, int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
    sqlite3* raw = nullptr;
    const auto rc = sqlite3_open_v2(path.string().c_str(), &raw,
        flags, nullptr);
    Connection db{raw, sqlite3_close_v2};
    sql_check(raw, rc);
    sql_check(raw, sqlite3_busy_timeout(raw, 3000));
    return db;
}

inline void execute(sqlite3* db, const char* sql) {
    sql_check(db, sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        sql_check(db, sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr));
    }
    ~Statement() { sqlite3_finalize(stmt_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bind(int col, std::int64_t v) { sql_check(db_, sqlite3_bind_int64(stmt_, col, v)); }
    void bind(int col, const std::string& v) {
        sql_check(db_, sqlite3_bind_text64(stmt_, col, v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
    }
    void optional_id(int col, std::int64_t id) {
        if (id) bind(col, id);
        else sql_check(db_, sqlite3_bind_null(stmt_, col));
    }
    bool step() {
        const int rc = sqlite3_step(stmt_);
        if (rc == SQLITE_ROW) return true;
        if (rc != SQLITE_DONE) sql_check(db_, rc);
        return false;
    }
    void run() {
        if (step()) throw std::runtime_error("Unexpected SQLite result");
        sql_check(db_, sqlite3_reset(stmt_));
    }
    std::int64_t number(int col) const { return sqlite3_column_int64(stmt_, col); }
    std::string text(int col) const {
        const auto* value = sqlite3_column_text(stmt_, col);
        if (!value) throw std::runtime_error("Missing SQLite text value");
        return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(sqlite3_column_bytes(stmt_, col))};
    }
private:
    sqlite3* db_;
    sqlite3_stmt* stmt_{};
};

class Transaction {
public:
    Transaction(sqlite3* db, bool write) : db_(db) { execute(db_, write ? "BEGIN IMMEDIATE" : "BEGIN"); }
    ~Transaction() { if (db_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() { execute(db_, "COMMIT"); db_ = nullptr; }
private:
    sqlite3* db_;
};

inline std::int64_t scalar(sqlite3* db, const char* sql, std::int64_t id = 0) {
    Statement q{db, sql};
    if (id) q.bind(1, id);
    return q.step() ? q.number(0) : 0;
}
} // namespace moneta::storage_detail
