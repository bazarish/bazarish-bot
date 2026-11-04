// Bazarish project (c) 2026
#include "Transcript.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace bazarish::bot {

namespace {

using client::DeliveryState;

constexpr int kMaxPage = 500;
constexpr const char* kOwnReactor = "me";
constexpr const char* kFileType = "file";
constexpr const char* kFailedStatus = "failed";

int stateOf(const std::string& status)
{
    if (status == "sending") {
        return static_cast<int>(DeliveryState::eDelivering);
    }
    if (status == "delivered") {
        return static_cast<int>(DeliveryState::eAtRecipientServer);
    }
    if (status == "read") {
        return static_cast<int>(DeliveryState::eDelivered);
    }
    if (status.rfind(kFailedStatus, 0) == 0) {
        return static_cast<int>(DeliveryState::eFailed);
    }
    return static_cast<int>(DeliveryState::eReceived);
}

std::string statusOf(const int state)
{
    switch (static_cast<DeliveryState>(state)) {
    case DeliveryState::ePreparing:
    case DeliveryState::eDelivering:
        return "sending";
    case DeliveryState::eAtRecipientServer:
        return "delivered";
    case DeliveryState::eDelivered:
        return "read";
    case DeliveryState::eFailed:
        return kFailedStatus;
    case DeliveryState::eReceived:
        return {};
    }
    return {};
}

client::StoredMessage rowOf(const Line& line)
{
    client::StoredMessage row;
    row.peer = line.peer;
    row.outgoing = line.outgoing;
    row.type = line.type;
    row.e2eId = line.e2eId;
    row.text = line.text;
    row.keyboard = line.keyboard;
    row.replyTo = line.replyTo;
    row.ts = line.sentAt;
    row.status = line.outgoing ? stateOf(line.status) : static_cast<int>(DeliveryState::eReceived);
    if (line.file.has_value()) {
        row.attName = line.file->name;
        row.attSize = static_cast<std::int64_t>(line.file->size);
        row.attMime = line.file->mime;
        row.attRef = line.file->sha256;
        (line.outgoing ? row.attSrcPath : row.savedPath) = line.file->path;
    }
    return row;
}

}  // namespace

Transcript::Transcript(const std::filesystem::path& accountFile, std::string ownFingerprint)
    : own_(std::move(ownFingerprint))
    , ledger_(accountFile, "transcript")
{
    if (!store_.open(accountFile)) {
        throw std::runtime_error("transcript: " + accountFile.string() + " did not open: "
            + store_.lastError());
    }
    ledger_.run(
        "CREATE TABLE IF NOT EXISTS events ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "kind TEXT NOT NULL, peer TEXT NOT NULL, messageId TEXT, text TEXT,"
        " actor TEXT, at INTEGER)");
    ledger_.run("CREATE TABLE IF NOT EXISTS issued_menus ("
                "peer TEXT PRIMARY KEY, menuId INTEGER NOT NULL, items TEXT NOT NULL)");
}

void Transcript::require(const bool done) const
{
    if (!done) {
        throw std::runtime_error("transcript: " + store_.lastError());
    }
}

Line Transcript::lineOf(const client::StoredMessage& row) const
{
    Line line;
    line.id = row.id;
    line.peer = row.peer;
    line.outgoing = row.outgoing;
    line.type = row.type;
    line.e2eId = row.e2eId;
    line.text = row.text;
    line.keyboard = row.keyboard;
    line.replyTo = row.replyTo;
    line.sentAt = row.ts;
    line.edited = row.edited;
    line.status = statusOf(row.status);
    if (row.type == kFileType) {
        line.file = FileInfo{row.attName, static_cast<std::uint64_t>(row.attSize), row.attMime,
            row.attRef, row.outgoing ? row.attSrcPath : row.savedPath};
    }
    for (const client::Reaction& reaction : store_.reactionsFor(row.peer, row.e2eId)) {
        line.reactions.push_back({reaction.reactor == own_ ? kOwnReactor : reaction.reactor,
            reaction.emoji});
    }
    return line;
}

void Transcript::setMenu(const std::string& peer, const client::BotMenu& menu)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement upsert(
        ledger_, "INSERT OR REPLACE INTO issued_menus (peer, menuId, items) VALUES (?, ?, ?)");
    upsert.bind(1, peer);
    upsert.bind(2, menu.id);
    upsert.bind(3, client::botMenuItemsJson(menu.items).dump());
    (void)upsert.step();
}

std::optional<client::BotMenu> Transcript::menu(const std::string& peer) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement query(ledger_, "SELECT menuId, items FROM issued_menus WHERE peer = ?");
    query.bind(1, peer);
    if (!query.step()) {
        return std::nullopt;
    }
    return client::botMenuFromJson(
        {{"menuId", query.number(0)}, {"items", nlohmann::json::parse(query.text(1))}});
}

std::int64_t Transcript::noteLocked(const std::string& kind, const std::string& peer,
    const std::string& messageId, const std::string& text, const std::string& actor,
    const std::int64_t at)
{
    Statement insert(ledger_,
        "INSERT INTO events (kind, peer, messageId, text, actor, at)"
        " VALUES (?, ?, ?, ?, ?, ?)");
    insert.bind(1, kind);
    insert.bind(2, peer);
    insert.bind(3, messageId);
    insert.bind(4, text);
    insert.bind(5, actor);
    insert.bind(6, at);
    (void)insert.step();
    return ledger_.lastInsertId();
}

std::int64_t Transcript::append(const Line& line)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    if (store_.idForKey(line.peer, line.e2eId, line.outgoing) != 0) {
        return 0;
    }
    const std::int64_t id = store_.append(rowOf(line));
    require(id != 0);
    (void)noteLocked("message", line.peer, line.e2eId, line.text, line.outgoing ? "" : line.peer,
        line.sentAt);
    return id;
}

bool Transcript::applyEdit(const std::string& peer, const std::string& messageId,
    const std::string& text, const std::string& keyboard, const bool outgoing)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t id = store_.idForKey(peer, messageId, outgoing);
    if (id == 0) {
        return false;
    }
    require(store_.editContent(id, text, keyboard));
    (void)noteLocked("edit", peer, messageId, text, outgoing ? "" : peer, 0);
    return true;
}

bool Transcript::applyDelete(
    const std::string& peer, const std::string& messageId, const bool outgoing)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t id = store_.idForKey(peer, messageId, outgoing);
    if (id == 0) {
        return false;
    }
    require(store_.removeById(id));
    (void)noteLocked("delete", peer, messageId, {}, outgoing ? "" : peer, 0);
    return true;
}

void Transcript::applyReaction(const std::string& peer, const std::string& messageId,
    const std::string& reactor, const std::string& emoji)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    require(store_.setReaction(peer, messageId, reactor == kOwnReactor ? own_ : reactor, emoji));
    (void)noteLocked("reaction", peer, messageId, emoji, reactor, 0);
}

void Transcript::clearPeer(const std::string& peer)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    require(store_.clearPeer(peer));
    (void)noteLocked("chat-cleared", peer, {}, {}, {}, 0);
}

void Transcript::setStatus(
    const std::string& peer, const std::string& messageId, const std::string& status)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const client::StoredMessage row = store_.messageByE2e(messageId, peer);
    if (row.id == 0 || !row.outgoing
        || row.status == static_cast<int>(DeliveryState::eDelivered)) {
        return;
    }
    require(store_.updateStatus(row.id, stateOf(status)));
    (void)noteLocked("status", peer, messageId, status, {}, 0);
}

void Transcript::applyReceipt(const std::string& peer, const std::string& messageId)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t upTo = store_.idForKey(peer, messageId, true);
    // A read receipt is a high-water mark: it covers every earlier line their server took.
    if (upTo != 0) {
        require(store_.markOutgoingReadUpTo(peer, upTo,
            static_cast<int>(DeliveryState::eDelivered),
            static_cast<int>(DeliveryState::eAtRecipientServer),
            static_cast<int>(DeliveryState::eAtRecipientServer)));
    }
    (void)noteLocked("receipt", peer, messageId, {}, peer, 0);
}

void Transcript::keepFileAt(
    const std::string& peer, const std::string& messageId, const std::string& path)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::int64_t id = store_.idForKey(peer, messageId, false);
    if (id != 0) {
        require(store_.setSavedPath(id, path));
    }
}

void Transcript::noteTransfer(
    const std::string& peer, const std::string& messageId, const std::string& state)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    (void)noteLocked("transfer", peer, messageId, state, {}, 0);
}

std::vector<Line> Transcript::conversation(
    const std::string& peer, const std::int64_t before, const int limit) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const int page = limit > kMaxPage || limit <= 0 ? kMaxPage : limit;
    const std::vector<client::StoredMessage> rows = before > 0
        ? store_.olderMessages(peer, before, page)
        : store_.latestMessages(peer, page);
    std::vector<Line> lines;
    for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
        lines.push_back(lineOf(*row));
    }
    return lines;
}

std::optional<Line> Transcript::find(const std::string& peer, const std::string& messageId) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const client::StoredMessage row = store_.messageByE2e(messageId, peer);
    if (row.id == 0) {
        return std::nullopt;
    }
    return lineOf(row);
}

std::vector<Line> Transcript::latestPerPeer() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Line> lines;
    for (const std::string& peer : store_.conversationPeers()) {
        for (const client::StoredMessage& row : store_.latestMessages(peer, 1)) {
            Line line = lineOf(row);
            lines.push_back(std::move(line));
        }
    }
    std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.id > b.id; });
    return lines;
}

std::vector<Event> Transcript::since(const std::int64_t id) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Statement query(ledger_,
        "SELECT id, kind, peer, messageId, text, actor, at FROM events"
        " WHERE id > ? ORDER BY id LIMIT ?");
    query.bind(1, id);
    query.bind(2, static_cast<std::int64_t>(kMaxPage));
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
    Statement query(ledger_, "SELECT IFNULL(MAX(id), 0) FROM events");
    return query.step() ? query.number(0) : 0;
}

client::DatabaseFootprint Transcript::footprint() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return store_.footprint();
}

void Transcript::rebuild()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string reason;
    if (!store_.rebuild(reason)) {
        throw std::runtime_error("the database was not rewritten: " + reason);
    }
}

}  // namespace bazarish::bot
