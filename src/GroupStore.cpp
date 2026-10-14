// Bazarish project (c) 2026
#include "GroupStore.hpp"

#include <stdexcept>
#include <utility>

namespace bazarish::bot {

namespace {

constexpr std::int64_t kConfigRow = 1;

constexpr const char* kMemberColumns = "fingerprint, name, nameKey, state, admin, contactable,"
                                       " cursor, historyFrom, joinedAt, welcomeId";
constexpr const char* kPostColumns = "id, author, type, text, mediaName, mime, media, durationMs,"
                                     " replyTo, sentAt, edited, deleted, seq";

struct Named {
    MemberState state;
    const char* name;
};

constexpr Named kStateNames[] = {
    {MemberState::eNaming, "naming"},
    {MemberState::ePending, "pending"},
    {MemberState::eMember, "member"},
    {MemberState::eLeft, "left"},
    {MemberState::eBanned, "banned"},
};

constexpr const char* kLogKindNames[] = {"post", "edit", "delete", "reactions"};

MemberState memberStateFromName(const std::string& name)
{
    for (const Named& entry : kStateNames) {
        if (name == entry.name) {
            return entry.state;
        }
    }
    throw std::runtime_error("group store: unknown member state " + name);
}

LogKind logKindFromName(const std::string& name)
{
    for (std::size_t index = 0; index < std::size(kLogKindNames); ++index) {
        if (name == kLogKindNames[index]) {
            return static_cast<LogKind>(index);
        }
    }
    throw std::runtime_error("group store: unknown log entry " + name);
}

std::int64_t flag(const bool value)
{
    return value ? 1 : 0;
}

Member readMember(const Statement& row)
{
    Member member;
    member.fingerprint = row.text(0);
    member.name = row.text(1);
    member.nameKey = row.text(2);
    member.state = memberStateFromName(row.text(3));
    member.admin = row.number(4) != 0;
    member.contactable = row.number(5) != 0;
    member.cursor = row.number(6);
    member.historyFrom = row.number(7);
    member.joinedAt = row.number(8);
    member.welcomeId = row.text(9);
    return member;
}

Post readPost(const Statement& row)
{
    Post post;
    post.id = row.text(0);
    post.author = row.text(1);
    post.type = row.text(2);
    post.text = row.text(3);
    post.mediaName = row.text(4);
    post.mime = row.text(5);
    post.media = row.blob(6);
    post.durationMs = row.number(7);
    post.replyTo = row.text(8);
    post.sentAt = row.number(9);
    post.edited = row.number(10) != 0;
    post.deleted = row.number(11) != 0;
    post.seq = row.number(12);
    return post;
}

}  // namespace

std::string joinModeName(const JoinMode mode)
{
    return mode == JoinMode::eApproval ? "approval" : "open";
}

JoinMode joinModeFromName(const std::string& name)
{
    if (name == "approval") {
        return JoinMode::eApproval;
    }
    if (name == "open") {
        return JoinMode::eOpen;
    }
    throw std::runtime_error("the join mode is open or approval");
}

std::string memberStateName(const MemberState state)
{
    for (const Named& entry : kStateNames) {
        if (entry.state == state) {
            return entry.name;
        }
    }
    throw std::runtime_error("group store: a member state without a name");
}

std::string groupKindName(const client::GroupKind kind)
{
    return kind == client::GroupKind::eChannel ? "channel" : "group";
}

client::GroupKind groupKindFromName(const std::string& name)
{
    if (name == "channel") {
        return client::GroupKind::eChannel;
    }
    if (name == "group") {
        return client::GroupKind::eGroup;
    }
    throw std::runtime_error("the group kind is group or channel");
}

GroupStore::GroupStore(const std::filesystem::path& file, const std::string& passphrase)
    : database_(file, passphrase, "group store")
{
    database_.run("CREATE TABLE IF NOT EXISTS config (row INTEGER PRIMARY KEY, kind TEXT NOT NULL,"
                  " joinMode TEXT NOT NULL, historySize INTEGER NOT NULL,"
                  " contactRequests INTEGER NOT NULL, language TEXT NOT NULL,"
                  " greeting TEXT NOT NULL, welcome TEXT NOT NULL)");
    database_.run("CREATE TABLE IF NOT EXISTS members (fingerprint TEXT PRIMARY KEY,"
                  " name TEXT NOT NULL, nameKey TEXT NOT NULL, state TEXT NOT NULL,"
                  " admin INTEGER NOT NULL, contactable INTEGER NOT NULL, cursor INTEGER NOT NULL,"
                  " historyFrom INTEGER NOT NULL, joinedAt INTEGER NOT NULL, welcomeId TEXT NOT NULL)");
    database_.run("CREATE UNIQUE INDEX IF NOT EXISTS members_name ON members(nameKey)"
                  " WHERE nameKey <> ''");
    database_.run("CREATE TABLE IF NOT EXISTS posts (id TEXT PRIMARY KEY, author TEXT NOT NULL,"
                  " type TEXT NOT NULL, text TEXT NOT NULL, mediaName TEXT NOT NULL,"
                  " mime TEXT NOT NULL, media BLOB, durationMs INTEGER NOT NULL,"
                  " replyTo TEXT NOT NULL, sentAt INTEGER NOT NULL, edited INTEGER NOT NULL,"
                  " deleted INTEGER NOT NULL, seq INTEGER NOT NULL)");
    database_.run("CREATE TABLE IF NOT EXISTS log (seq INTEGER PRIMARY KEY AUTOINCREMENT,"
                  " kind TEXT NOT NULL, postId TEXT NOT NULL)");
    database_.run("CREATE TABLE IF NOT EXISTS reactions (postId TEXT NOT NULL, member TEXT NOT NULL,"
                  " emoji TEXT NOT NULL, PRIMARY KEY (postId, member))");
    database_.run("CREATE TABLE IF NOT EXISTS requests (number INTEGER PRIMARY KEY AUTOINCREMENT,"
                  " fingerprint TEXT NOT NULL UNIQUE, at INTEGER NOT NULL)");
    database_.run("CREATE TABLE IF NOT EXISTS cards (number INTEGER NOT NULL, admin TEXT NOT NULL,"
                  " messageId TEXT NOT NULL, PRIMARY KEY (number, admin))");
    database_.run("CREATE TABLE IF NOT EXISTS asks (requester TEXT NOT NULL, author TEXT NOT NULL,"
                  " PRIMARY KEY (requester, author))");

    Statement seed(database_, "INSERT OR IGNORE INTO config (row, kind, joinMode, historySize,"
                              " contactRequests, language, greeting, welcome)"
                              " VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
    const GroupConfig defaults;
    seed.bind(1, kConfigRow);
    seed.bind(2, groupKindName(defaults.kind));
    seed.bind(3, joinModeName(defaults.joinMode));
    seed.bind(4, defaults.historySize);
    seed.bind(5, flag(defaults.contactRequests));
    seed.bind(6, defaults.language);
    seed.bind(7, defaults.greeting);
    seed.bind(8, defaults.welcome);
    (void)seed.step();
}

GroupConfig GroupStore::config() const
{
    Statement query(database_, "SELECT kind, joinMode, historySize, contactRequests, language,"
                               " greeting, welcome FROM config WHERE row = ?");
    query.bind(1, kConfigRow);
    if (!query.step()) {
        throw std::runtime_error("group store: the configuration row is missing");
    }
    GroupConfig config;
    config.kind = groupKindFromName(query.text(0));
    config.joinMode = joinModeFromName(query.text(1));
    config.historySize = query.number(2);
    config.contactRequests = query.number(3) != 0;
    config.language = query.text(4);
    config.greeting = query.text(5);
    config.welcome = query.text(6);
    return config;
}

void GroupStore::setConfig(const GroupConfig& config)
{
    Statement update(database_, "UPDATE config SET kind = ?, joinMode = ?, historySize = ?,"
                                " contactRequests = ?, language = ?, greeting = ?, welcome = ?"
                                " WHERE row = ?");
    update.bind(1, groupKindName(config.kind));
    update.bind(2, joinModeName(config.joinMode));
    update.bind(3, config.historySize);
    update.bind(4, flag(config.contactRequests));
    update.bind(5, config.language);
    update.bind(6, config.greeting);
    update.bind(7, config.welcome);
    update.bind(8, kConfigRow);
    (void)update.step();
}

std::vector<Member> GroupStore::members() const
{
    Statement query(database_, std::string("SELECT ") + kMemberColumns + " FROM members");
    std::vector<Member> members;
    while (query.step()) {
        members.push_back(readMember(query));
    }
    return members;
}

void GroupStore::putMember(const Member& member)
{
    Statement upsert(database_, std::string("INSERT OR REPLACE INTO members (") + kMemberColumns
            + ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    upsert.bind(1, member.fingerprint);
    upsert.bind(2, member.name);
    upsert.bind(3, member.nameKey);
    upsert.bind(4, memberStateName(member.state));
    upsert.bind(5, flag(member.admin));
    upsert.bind(6, flag(member.contactable));
    upsert.bind(7, member.cursor);
    upsert.bind(8, member.historyFrom);
    upsert.bind(9, member.joinedAt);
    upsert.bind(10, member.welcomeId);
    (void)upsert.step();
}

std::optional<Post> GroupStore::post(const std::string& id) const
{
    Statement query(database_, std::string("SELECT ") + kPostColumns + " FROM posts WHERE id = ?");
    query.bind(1, id);
    if (!query.step()) {
        return std::nullopt;
    }
    return readPost(query);
}

std::int64_t GroupStore::addPost(Post post)
{
    post.seq = appendLog(LogKind::ePost, post.id);
    Statement insert(database_, std::string("INSERT INTO posts (") + kPostColumns
            + ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert.bind(1, post.id);
    insert.bind(2, post.author);
    insert.bind(3, post.type);
    insert.bind(4, post.text);
    insert.bind(5, post.mediaName);
    insert.bind(6, post.mime);
    insert.bind(7, post.media);
    insert.bind(8, post.durationMs);
    insert.bind(9, post.replyTo);
    insert.bind(10, post.sentAt);
    insert.bind(11, flag(post.edited));
    insert.bind(12, flag(post.deleted));
    insert.bind(13, post.seq);
    (void)insert.step();
    return post.seq;
}

void GroupStore::updatePost(const Post& post)
{
    Statement update(database_, "UPDATE posts SET text = ?, edited = ?, deleted = ? WHERE id = ?");
    update.bind(1, post.text);
    update.bind(2, flag(post.edited));
    update.bind(3, flag(post.deleted));
    update.bind(4, post.id);
    (void)update.step();
}

std::vector<Post> GroupStore::recentPosts(const std::int64_t limit) const
{
    Statement query(database_, std::string("SELECT ") + kPostColumns
            + " FROM posts ORDER BY seq DESC LIMIT ?");
    query.bind(1, limit);
    std::vector<Post> posts;
    while (query.step()) {
        posts.push_back(readPost(query));
    }
    return posts;
}

void GroupStore::dropMediaBefore(const std::int64_t seq)
{
    Statement update(database_, "UPDATE posts SET media = NULL WHERE seq <= ? AND media IS NOT NULL");
    update.bind(1, seq);
    (void)update.step();
}

std::int64_t GroupStore::appendLog(const LogKind kind, const std::string& postId)
{
    Statement insert(database_, "INSERT INTO log (kind, postId) VALUES (?, ?)");
    insert.bind(1, std::string(kLogKindNames[static_cast<std::size_t>(kind)]));
    insert.bind(2, postId);
    (void)insert.step();
    return database_.lastInsertId();
}

std::vector<LogEntry> GroupStore::logAfter(const std::int64_t seq, const std::int64_t limit) const
{
    Statement query(database_, "SELECT seq, kind, postId FROM log WHERE seq > ? ORDER BY seq LIMIT ?");
    query.bind(1, seq);
    query.bind(2, limit);
    std::vector<LogEntry> entries;
    while (query.step()) {
        entries.push_back({query.number(0), logKindFromName(query.text(1)), query.text(2)});
    }
    return entries;
}

std::int64_t GroupStore::lastSeq() const
{
    Statement query(database_, "SELECT COALESCE(MAX(seq), 0) FROM log");
    if (!query.step()) {
        return 0;
    }
    return query.number(0);
}

std::int64_t GroupStore::seqBeforeLastPosts(const std::int64_t count) const
{
    if (count <= 0) {
        return lastSeq();
    }
    Statement query(database_, "SELECT seq FROM posts WHERE deleted = 0 ORDER BY seq DESC"
                               " LIMIT 1 OFFSET ?");
    query.bind(1, count - 1);
    if (!query.step()) {
        return 0;
    }
    return query.number(0) - 1;
}

void GroupStore::setReaction(
    const std::string& postId, const std::string& member, const std::string& emoji)
{
    if (emoji.empty()) {
        Statement remove(database_, "DELETE FROM reactions WHERE postId = ? AND member = ?");
        remove.bind(1, postId);
        remove.bind(2, member);
        (void)remove.step();
        return;
    }
    Statement upsert(database_, "INSERT OR REPLACE INTO reactions (postId, member, emoji)"
                                " VALUES (?, ?, ?)");
    upsert.bind(1, postId);
    upsert.bind(2, member);
    upsert.bind(3, emoji);
    (void)upsert.step();
}

std::map<std::string, std::int64_t> GroupStore::reactionCounts(
    const std::string& postId, const std::string& leavingOut) const
{
    Statement query(database_, "SELECT emoji, COUNT(*) FROM reactions"
                               " WHERE postId = ? AND member <> ? GROUP BY emoji");
    query.bind(1, postId);
    query.bind(2, leavingOut);
    std::map<std::string, std::int64_t> counts;
    while (query.step()) {
        counts[query.text(0)] = query.number(1);
    }
    return counts;
}

std::int64_t GroupStore::addRequest(const std::string& fingerprint, const std::int64_t at)
{
    Statement insert(database_, "INSERT OR IGNORE INTO requests (fingerprint, at) VALUES (?, ?)");
    insert.bind(1, fingerprint);
    insert.bind(2, at);
    (void)insert.step();
    const std::optional<JoinRequest> held = requestOf(fingerprint);
    if (!held) {
        throw std::runtime_error("group store: a request was not recorded");
    }
    return held->number;
}

std::optional<JoinRequest> GroupStore::request(const std::int64_t number) const
{
    Statement query(database_, "SELECT number, fingerprint, at FROM requests WHERE number = ?");
    query.bind(1, number);
    if (!query.step()) {
        return std::nullopt;
    }
    return JoinRequest{query.number(0), query.text(1), query.number(2)};
}

std::optional<JoinRequest> GroupStore::requestOf(const std::string& fingerprint) const
{
    Statement query(database_, "SELECT number, fingerprint, at FROM requests WHERE fingerprint = ?");
    query.bind(1, fingerprint);
    if (!query.step()) {
        return std::nullopt;
    }
    return JoinRequest{query.number(0), query.text(1), query.number(2)};
}

std::vector<JoinRequest> GroupStore::requests() const
{
    Statement query(database_, "SELECT number, fingerprint, at FROM requests ORDER BY number");
    std::vector<JoinRequest> requests;
    while (query.step()) {
        requests.push_back({query.number(0), query.text(1), query.number(2)});
    }
    return requests;
}

void GroupStore::removeRequest(const std::int64_t number)
{
    Statement remove(database_, "DELETE FROM requests WHERE number = ?");
    remove.bind(1, number);
    (void)remove.step();
    Statement cards(database_, "DELETE FROM cards WHERE number = ?");
    cards.bind(1, number);
    (void)cards.step();
}

void GroupStore::addCard(
    const std::int64_t number, const std::string& admin, const std::string& messageId)
{
    Statement insert(database_, "INSERT OR REPLACE INTO cards (number, admin, messageId)"
                                " VALUES (?, ?, ?)");
    insert.bind(1, number);
    insert.bind(2, admin);
    insert.bind(3, messageId);
    (void)insert.step();
}

std::vector<RequestCard> GroupStore::cards(const std::int64_t number) const
{
    Statement query(database_, "SELECT admin, messageId FROM cards WHERE number = ?");
    query.bind(1, number);
    std::vector<RequestCard> cards;
    while (query.step()) {
        cards.push_back({query.text(0), query.text(1)});
    }
    return cards;
}

bool GroupStore::noteContactAsk(const std::string& requester, const std::string& author)
{
    Statement insert(database_, "INSERT OR IGNORE INTO asks (requester, author) VALUES (?, ?)");
    insert.bind(1, requester);
    insert.bind(2, author);
    (void)insert.step();
    return database_.changes() > 0;
}

}  // namespace bazarish::bot
