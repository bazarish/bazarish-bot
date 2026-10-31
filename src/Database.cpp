// Bazarish project (c) 2026
#include "Database.hpp"

#include <AccountKey.hpp>

#include <sqlcipher/sqlite3.h>

#include <stdexcept>
#include <utility>

namespace bazarish::bot {

namespace {

constexpr int kBusyTimeoutMs = 5000;

std::string rawKeyLiteral(const Bytes& key)
{
    return "x'" + toHex(key) + "'";
}

}  // namespace

Database::Database(const std::filesystem::path& file, std::string label)
    : label_(std::move(label))
{
    std::filesystem::create_directories(file.parent_path());
    const Bytes key = client::accountkey::keyFor(file, std::string());
    if (sqlite3_open(file.string().c_str(), &db_) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error(label_ + ": cannot open " + file.string());
    }
    sqlite3_exec(db_, "PRAGMA cipher_log_level = NONE", nullptr, nullptr, nullptr);
    run("PRAGMA key = \"" + rawKeyLiteral(key) + "\"");
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
}

Database::~Database()
{
    sqlite3_close(db_);
}

void Database::run(const std::string& sql) const
{
    char* message = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string text = message == nullptr ? "unknown" : message;
        sqlite3_free(message);
        throw std::runtime_error(label_ + ": " + text);
    }
}

std::int64_t Database::lastInsertId() const
{
    return sqlite3_last_insert_rowid(db_);
}

int Database::changes() const
{
    return sqlite3_changes(db_);
}

Statement::Statement(const Database& database, const std::string& sql)
    : label_(database.label())
{
    if (sqlite3_prepare_v2(database.handle(), sql.c_str(), -1, &statement_, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error(label_ + ": cannot prepare a statement: "
            + sqlite3_errmsg(database.handle()));
    }
}

Statement::~Statement()
{
    sqlite3_finalize(statement_);
}

void Statement::bind(const int index, const std::string& text)
{
    if (sqlite3_bind_text(statement_, index, text.data(), static_cast<int>(text.size()),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        throw std::runtime_error(label_ + ": cannot bind a value");
    }
}

void Statement::bind(const int index, const std::int64_t value)
{
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
        throw std::runtime_error(label_ + ": cannot bind a value");
    }
}

void Statement::bind(const int index, const Bytes& blob)
{
    if (sqlite3_bind_blob64(statement_, index, blob.data(), blob.size(), SQLITE_TRANSIENT)
        != SQLITE_OK) {
        throw std::runtime_error(label_ + ": cannot bind a value");
    }
}

bool Statement::step()
{
    const int outcome = sqlite3_step(statement_);
    if (outcome == SQLITE_ROW) {
        return true;
    }
    if (outcome != SQLITE_DONE) {
        throw std::runtime_error(label_ + ": a statement did not complete");
    }
    return false;
}

std::string Statement::text(const int index) const
{
    const unsigned char* const value = sqlite3_column_text(statement_, index);
    return value == nullptr ? std::string() : reinterpret_cast<const char*>(value);
}

std::int64_t Statement::number(const int index) const
{
    return sqlite3_column_int64(statement_, index);
}

Bytes Statement::blob(const int index) const
{
    const auto* const data = static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_, index));
    const int size = sqlite3_column_bytes(statement_, index);
    return data == nullptr ? Bytes() : Bytes(data, data + size);
}

}  // namespace bazarish::bot
