// Bazarish project (c) 2026
#include "Transcript.hpp"

#include <AccountKey.hpp>

#include <bazarish/Bytes.hpp>

#include <sqlcipher/sqlite3.h>

#include <stdexcept>

namespace bazarish::daemon {

namespace {

// How long a writer waits for another to finish before giving up.
constexpr int kBusyTimeoutMs = 5000;
// What a conversation query will hand back at most, however much is asked for:
// a page, not a database.
constexpr int kMaxPage = 500;

// A raw key is given to SQLCipher as hex in a blob literal.
std::string rawKeyLiteral(const Bytes& key)
{
    return "x'" + toHex(key) + "'";
}

void run(sqlite3* const db, const std::string& sql)
{
    char* message = nullptr;
    if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &message) != SQLITE_OK) {
        const std::string text = message == nullptr ? "unknown" : message;
        sqlite3_free(message);
        throw std::runtime_error("transcript: " + text);
    }
}

void bindText(sqlite3_stmt* const statement, const int index, const std::string& text)
{
    if (sqlite3_bind_text(statement, index, text.data(), static_cast<int>(text.size()),
            SQLITE_TRANSIENT)
        != SQLITE_OK) {
        throw std::runtime_error("transcript: cannot bind a value");
    }
}

std::string columnText(sqlite3_stmt* const statement, const int index)
{
    const unsigned char* const text = sqlite3_column_text(statement, index);
    return text == nullptr ? std::string() : reinterpret_cast<const char*>(text);
}

Line readLine(sqlite3_stmt* const statement)
{
    Line line;
    line.id = sqlite3_column_int64(statement, 0);
    line.peer = columnText(statement, 1);
    line.outgoing = sqlite3_column_int(statement, 2) != 0;
    line.type = columnText(statement, 3);
    line.e2eId = columnText(statement, 4);
    line.text = columnText(statement, 5);
    line.attachmentName = columnText(statement, 6);
    line.keyboard = columnText(statement, 7);
    line.replyTo = columnText(statement, 8);
    line.sentAt = sqlite3_column_int64(statement, 9);
    return line;
}

constexpr const char* kColumns
    = "id, peer, outgoing, type, e2eId, text, attName, keyboard, replyTo, sentAt";

}  // namespace

Transcript::Transcript(const std::filesystem::path& file, const std::string& passphrase)
{
    std::filesystem::create_directories(file.parent_path());
    // The key record is made before the database file exists: a database with no
    // record beside it is one whose key is gone, and asking for it then is how a
    // wrong passphrase would read as a new account.
    const Bytes key = client::accountkey::keyFor(file, passphrase);
    if (sqlite3_open(file.string().c_str(), &db_) != SQLITE_OK) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("transcript: cannot open " + file.string());
    }
    sqlite3_exec(db_, "PRAGMA cipher_log_level = NONE", nullptr, nullptr, nullptr);
    // The same derivation the account database uses, over this file's own key
    // record: one passphrase opens both, and neither is readable without it.
    run(db_, "PRAGMA key = \"" + rawKeyLiteral(key) + "\"");
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    run(db_,
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "peer TEXT NOT NULL, outgoing INTEGER NOT NULL, type TEXT, e2eId TEXT,"
        "text TEXT, attName TEXT, keyboard TEXT, replyTo TEXT, sentAt INTEGER)");
    // The mailbox may hand the same item over twice; the second one is not a
    // second message.
    run(db_, "CREATE UNIQUE INDEX IF NOT EXISTS messages_e2e ON messages(e2eId)"
             " WHERE e2eId IS NOT NULL AND e2eId <> ''");
    run(db_, "CREATE INDEX IF NOT EXISTS messages_peer ON messages(peer, id)");
}

Transcript::~Transcript()
{
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

std::int64_t Transcript::append(const Line& line)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO messages"
            " (peer, outgoing, type, e2eId, text, attName, keyboard, replyTo, sentAt)"
            " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
            -1, &statement, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("transcript: cannot prepare the insert");
    }
    bindText(statement, 1, line.peer);
    sqlite3_bind_int(statement, 2, line.outgoing ? 1 : 0);
    bindText(statement, 3, line.type);
    bindText(statement, 4, line.e2eId);
    bindText(statement, 5, line.text);
    bindText(statement, 6, line.attachmentName);
    bindText(statement, 7, line.keyboard);
    bindText(statement, 8, line.replyTo);
    sqlite3_bind_int64(statement, 9, line.sentAt);
    const int done = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (done != SQLITE_DONE) {
        throw std::runtime_error("transcript: cannot store a line");
    }
    return sqlite3_changes(db_) == 0 ? 0 : sqlite3_last_insert_rowid(db_);
}

std::vector<Line> Transcript::conversation(
    const std::string& peer, const std::int64_t before, const int limit) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string sql = std::string("SELECT ") + kColumns
        + " FROM messages WHERE peer = ?"
        + (before > 0 ? " AND id < ?" : "") + " ORDER BY id DESC LIMIT ?";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error("transcript: cannot prepare the conversation query");
    }
    int index = 1;
    bindText(statement, index++, peer);
    if (before > 0) {
        sqlite3_bind_int64(statement, index++, before);
    }
    sqlite3_bind_int(statement, index, limit > kMaxPage || limit <= 0 ? kMaxPage : limit);
    std::vector<Line> lines;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        lines.push_back(readLine(statement));
    }
    sqlite3_finalize(statement);
    return lines;
}

std::vector<std::string> Transcript::peers() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT peer, MAX(id) AS last FROM messages GROUP BY peer ORDER BY last DESC", -1,
            &statement, nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("transcript: cannot prepare the peer query");
    }
    std::vector<std::string> peers;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        peers.push_back(columnText(statement, 0));
    }
    sqlite3_finalize(statement);
    return peers;
}

std::vector<Line> Transcript::latestPerPeer() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string sql = std::string("SELECT ") + kColumns
        + " FROM messages WHERE id IN (SELECT MAX(id) FROM messages GROUP BY peer)"
        + " ORDER BY id DESC";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error("transcript: cannot prepare the chat-list query");
    }
    std::vector<Line> lines;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        lines.push_back(readLine(statement));
    }
    sqlite3_finalize(statement);
    return lines;
}

std::vector<Line> Transcript::since(const std::int64_t id, const int limit) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string sql
        = std::string("SELECT ") + kColumns + " FROM messages WHERE id > ? ORDER BY id LIMIT ?";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
        throw std::runtime_error("transcript: cannot prepare the event query");
    }
    sqlite3_bind_int64(statement, 1, id);
    sqlite3_bind_int(statement, 2, limit > kMaxPage || limit <= 0 ? kMaxPage : limit);
    std::vector<Line> lines;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        lines.push_back(readLine(statement));
    }
    sqlite3_finalize(statement);
    return lines;
}

std::int64_t Transcript::lastId() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT IFNULL(MAX(id), 0) FROM messages", -1, &statement,
            nullptr)
        != SQLITE_OK) {
        throw std::runtime_error("transcript: cannot prepare the last-id query");
    }
    std::int64_t last = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        last = sqlite3_column_int64(statement, 0);
    }
    sqlite3_finalize(statement);
    return last;
}

}  // namespace bazarish::daemon
