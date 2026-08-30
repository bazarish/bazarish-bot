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
// What a query will hand back at most, however much is asked for: a page, not a
// database.
constexpr int kMaxPage = 500;

constexpr const char* kLineColumns
    = "id, peer, outgoing, type, e2eId, text, keyboard, replyTo, sentAt, edited, status";

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

// A prepared statement that is always finalised, however the caller leaves.
class Statement {
public:
    Statement(sqlite3* const db, const std::string& sql)
    {
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement_, nullptr) != SQLITE_OK) {
            throw std::runtime_error(
                std::string("transcript: cannot prepare a statement: ") + sqlite3_errmsg(db));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const { return statement_; }

    void bind(const int index, const std::string& text)
    {
        if (sqlite3_bind_text(statement_, index, text.data(), static_cast<int>(text.size()),
                SQLITE_TRANSIENT)
            != SQLITE_OK) {
            throw std::runtime_error("transcript: cannot bind a value");
        }
    }
    void bind(const int index, const std::int64_t value)
    {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw std::runtime_error("transcript: cannot bind a value");
        }
    }
    bool step()
    {
        const int outcome = sqlite3_step(statement_);
        if (outcome == SQLITE_ROW) {
            return true;
        }
        if (outcome != SQLITE_DONE) {
            throw std::runtime_error("transcript: a statement did not complete");
        }
        return false;
    }
    std::string text(const int index) const
    {
        const unsigned char* const value = sqlite3_column_text(statement_, index);
        return value == nullptr ? std::string() : reinterpret_cast<const char*>(value);
    }
    std::int64_t number(const int index) const
    {
        return sqlite3_column_int64(statement_, index);
    }

private:
    sqlite3_stmt* statement_ = nullptr;
};

Line readLine(const Statement& statement)
{
    Line line;
    line.id = statement.number(0);
    line.peer = statement.text(1);
    line.outgoing = statement.number(2) != 0;
    line.type = statement.text(3);
    line.e2eId = statement.text(4);
    line.text = statement.text(5);
    line.keyboard = statement.text(6);
    line.replyTo = statement.text(7);
    line.sentAt = statement.number(8);
    line.edited = statement.number(9) != 0;
    line.status = statement.text(10);
    return line;
}

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
    run(db_, "PRAGMA key = \"" + rawKeyLiteral(key) + "\"");
    sqlite3_busy_timeout(db_, kBusyTimeoutMs);
    run(db_,
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "peer TEXT NOT NULL, outgoing INTEGER NOT NULL, type TEXT, e2eId TEXT,"
        "text TEXT, keyboard TEXT, replyTo TEXT, sentAt INTEGER,"
        "edited INTEGER NOT NULL DEFAULT 0, status TEXT)");
    // The mailbox may hand the same item over twice; the second one is not a
    // second message.
    run(db_, "CREATE UNIQUE INDEX IF NOT EXISTS messages_e2e ON messages(e2eId)"
             " WHERE e2eId IS NOT NULL AND e2eId <> ''");
    run(db_, "CREATE INDEX IF NOT EXISTS messages_peer ON messages(peer, id)");
    // A file written by an older daemon has a table of the same name and other
    // columns, and CREATE TABLE IF NOT EXISTS leaves it exactly as it was. Said
    // here rather than met later as a query that cannot be prepared - and the
    // file is left alone, because deleting somebody's conversations to fix a
    // schema is not this code's decision to make.
    {
        Statement columns(db_, "SELECT COUNT(*) FROM pragma_table_info('messages')"
                               " WHERE name = 'status'");
        if (!columns.step() || columns.number(0) == 0) {
            throw std::runtime_error("the transcript at " + file.string()
                + " was written by an older daemon and cannot be read; remove it and its"
                  " .key file to start a fresh one - the conversations kept in it will be"
                  " lost, and nothing else will");
        }
    }
    // One reaction per reactor per message: a new emoji replaces the old one.
    run(db_,
        "CREATE TABLE IF NOT EXISTS reactions ("
        "peer TEXT NOT NULL, messageId TEXT NOT NULL, reactor TEXT NOT NULL, emoji TEXT,"
        " PRIMARY KEY (peer, messageId, reactor))");
    run(db_,
        "CREATE TABLE IF NOT EXISTS events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "kind TEXT NOT NULL, peer TEXT NOT NULL, messageId TEXT, text TEXT,"
        " actor TEXT, at INTEGER)");
}

Transcript::~Transcript()
{
    if (db_ != nullptr) {
        sqlite3_close(db_);
    }
}

std::int64_t Transcript::noteLocked(const std::string& kind, const std::string& peer,
    const std::string& messageId, const std::string& text, const std::string& actor,
    const std::int64_t at)
{
    Statement insert(db_,
        "INSERT INTO events (kind, peer, messageId, text, actor, at)"
        " VALUES (?, ?, ?, ?, ?, ?)");
    insert.bind(1, kind);
    insert.bind(2, peer);
    insert.bind(3, messageId);
    insert.bind(4, text);
    insert.bind(5, actor);
    insert.bind(6, at);
    (void)insert.step();
    return sqlite3_last_insert_rowid(db_);
}

std::int64_t Transcript::append(const Line& line)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement insert(db_,
        "INSERT OR IGNORE INTO messages"
        " (peer, outgoing, type, e2eId, text, keyboard, replyTo, sentAt, status)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert.bind(1, line.peer);
    insert.bind(2, static_cast<std::int64_t>(line.outgoing ? 1 : 0));
    insert.bind(3, line.type);
    insert.bind(4, line.e2eId);
    insert.bind(5, line.text);
    insert.bind(6, line.keyboard);
    insert.bind(7, line.replyTo);
    insert.bind(8, line.sentAt);
    insert.bind(9, line.status);
    (void)insert.step();
    if (sqlite3_changes(db_) == 0) {
        return 0;
    }
    const std::int64_t id = sqlite3_last_insert_rowid(db_);
    (void)noteLocked("message", line.peer, line.e2eId, line.text, line.outgoing ? "" : line.peer,
        line.sentAt);
    return id;
}

bool Transcript::applyEdit(const std::string& peer, const std::string& messageId,
    const std::string& text, const std::string& keyboard, const bool outgoing)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement update(db_,
        "UPDATE messages SET text = ?, keyboard = ?, edited = 1"
        " WHERE peer = ? AND e2eId = ? AND outgoing = ?");
    update.bind(1, text);
    update.bind(2, keyboard);
    update.bind(3, peer);
    update.bind(4, messageId);
    update.bind(5, static_cast<std::int64_t>(outgoing ? 1 : 0));
    (void)update.step();
    if (sqlite3_changes(db_) == 0) {
        return false;
    }
    (void)noteLocked("edit", peer, messageId, text, outgoing ? "" : peer, 0);
    return true;
}

bool Transcript::applyDelete(
    const std::string& peer, const std::string& messageId, const bool outgoing)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement remove(db_,
        "DELETE FROM messages WHERE peer = ? AND e2eId = ? AND outgoing = ?");
    remove.bind(1, peer);
    remove.bind(2, messageId);
    remove.bind(3, static_cast<std::int64_t>(outgoing ? 1 : 0));
    (void)remove.step();
    if (sqlite3_changes(db_) == 0) {
        return false;
    }
    Statement clear(db_, "DELETE FROM reactions WHERE peer = ? AND messageId = ?");
    clear.bind(1, peer);
    clear.bind(2, messageId);
    (void)clear.step();
    (void)noteLocked("delete", peer, messageId, {}, outgoing ? "" : peer, 0);
    return true;
}

void Transcript::applyReaction(const std::string& peer, const std::string& messageId,
    const std::string& reactor, const std::string& emoji)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (emoji.empty()) {
        Statement remove(db_,
            "DELETE FROM reactions WHERE peer = ? AND messageId = ? AND reactor = ?");
        remove.bind(1, peer);
        remove.bind(2, messageId);
        remove.bind(3, reactor);
        (void)remove.step();
    } else {
        Statement put(db_,
            "INSERT INTO reactions (peer, messageId, reactor, emoji) VALUES (?, ?, ?, ?)"
            " ON CONFLICT(peer, messageId, reactor) DO UPDATE SET emoji = excluded.emoji");
        put.bind(1, peer);
        put.bind(2, messageId);
        put.bind(3, reactor);
        put.bind(4, emoji);
        (void)put.step();
    }
    (void)noteLocked("reaction", peer, messageId, emoji, reactor, 0);
}

void Transcript::setStatus(
    const std::string& peer, const std::string& messageId, const std::string& status)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement update(db_,
        "UPDATE messages SET status = ? WHERE peer = ? AND e2eId = ? AND outgoing = 1");
    update.bind(1, status);
    update.bind(2, peer);
    update.bind(3, messageId);
    (void)update.step();
    if (sqlite3_changes(db_) == 0) {
        return;
    }
    (void)noteLocked("status", peer, messageId, status, {}, 0);
}

void Transcript::applyReceipt(const std::string& peer, const std::string& messageId)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    (void)noteLocked("receipt", peer, messageId, {}, peer, 0);
}

void Transcript::loadReactionsLocked(std::vector<Line>& lines) const
{
    for (Line& line : lines) {
        if (line.e2eId.empty()) {
            continue;
        }
        Statement query(db_,
            "SELECT reactor, emoji FROM reactions WHERE peer = ? AND messageId = ?");
        query.bind(1, line.peer);
        query.bind(2, line.e2eId);
        while (query.step()) {
            line.reactions.push_back({line.e2eId, query.text(0), query.text(1)});
        }
    }
}

std::vector<Line> Transcript::conversation(
    const std::string& peer, const std::int64_t before, const int limit) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string sql = std::string("SELECT ") + kLineColumns
        + " FROM messages WHERE peer = ?" + (before > 0 ? " AND id < ?" : "")
        + " ORDER BY id DESC LIMIT ?";
    Statement query(db_, sql);
    int index = 1;
    query.bind(index++, peer);
    if (before > 0) {
        query.bind(index++, before);
    }
    query.bind(index, static_cast<std::int64_t>(limit > kMaxPage || limit <= 0 ? kMaxPage : limit));
    std::vector<Line> lines;
    while (query.step()) {
        lines.push_back(readLine(query));
    }
    loadReactionsLocked(lines);
    return lines;
}

std::vector<Line> Transcript::latestPerPeer() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::string sql = std::string("SELECT ") + kLineColumns
        + " FROM messages WHERE id IN (SELECT MAX(id) FROM messages GROUP BY peer)"
        + " ORDER BY id DESC";
    Statement query(db_, sql);
    std::vector<Line> lines;
    while (query.step()) {
        lines.push_back(readLine(query));
    }
    return lines;
}

std::vector<Event> Transcript::since(const std::int64_t id, const int limit) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_,
        "SELECT id, kind, peer, messageId, text, actor, at FROM events"
        " WHERE id > ? ORDER BY id LIMIT ?");
    query.bind(1, id);
    query.bind(2, static_cast<std::int64_t>(limit > kMaxPage || limit <= 0 ? kMaxPage : limit));
    std::vector<Event> events;
    while (query.step()) {
        Event event;
        event.id = query.number(0);
        event.kind = query.text(1);
        event.peer = query.text(2);
        event.messageId = query.text(3);
        event.text = query.text(4);
        event.actor = query.text(5);
        event.at = query.number(6);
        events.push_back(std::move(event));
    }
    return events;
}

std::int64_t Transcript::lastEventId() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement query(db_, "SELECT IFNULL(MAX(id), 0) FROM events");
    return query.step() ? query.number(0) : 0;
}

}  // namespace bazarish::daemon
