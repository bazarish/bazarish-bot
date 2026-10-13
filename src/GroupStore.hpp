// Bazarish project (c) 2026
#pragma once

#include "Database.hpp"

#include <Session.hpp>

#include <bazarish/Bytes.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bazarish::bot {

inline constexpr std::int64_t kDefaultHistorySize = 50;
inline constexpr const char* kDefaultGroupLanguage = "en";

enum class JoinMode {
    eOpen,
    eApproval,
};

enum class MemberState {
    eNaming,
    ePending,
    eMember,
    eGone,
    eLeft,
    eBanned,
};

enum class LogKind {
    ePost,
    eEdit,
    eDelete,
    eReactions,
};

struct GroupConfig {
    client::GroupKind kind = client::GroupKind::eGroup;
    JoinMode joinMode = JoinMode::eOpen;
    std::int64_t historySize = kDefaultHistorySize;
    bool contactRequests = true;
    std::string language = kDefaultGroupLanguage;
};

struct Member {
    std::string fingerprint;
    std::string name;
    std::string nameKey;
    MemberState state = MemberState::eNaming;
    bool admin = false;
    bool contactable = true;
    std::int64_t cursor = 0;
    std::int64_t historyFrom = 0;
    std::int64_t joinedAt = 0;
    std::string welcomeId;
};

struct Post {
    std::string id;
    std::string author;
    std::string type;
    std::string text;
    std::string mediaName;
    std::string mime;
    Bytes media;
    std::int64_t durationMs = 0;
    std::string replyTo;
    std::int64_t sentAt = 0;
    bool edited = false;
    bool deleted = false;
    std::int64_t seq = 0;
};

struct LogEntry {
    std::int64_t seq = 0;
    LogKind kind = LogKind::ePost;
    std::string postId;
};

struct JoinRequest {
    std::int64_t number = 0;
    std::string fingerprint;
    std::int64_t at = 0;
};

struct RequestCard {
    std::string admin;
    std::string messageId;
};

std::string joinModeName(JoinMode mode);
JoinMode joinModeFromName(const std::string& name);
std::string memberStateName(MemberState state);
std::string groupKindName(client::GroupKind kind);
client::GroupKind groupKindFromName(const std::string& name);

class GroupStore {
public:
    GroupStore(const std::filesystem::path& file, const std::string& passphrase);

    GroupConfig config() const;
    void setConfig(const GroupConfig& config);

    std::vector<Member> members() const;
    void putMember(const Member& member);

    std::optional<Post> post(const std::string& id) const;
    std::int64_t addPost(Post post);
    void updatePost(const Post& post);
    std::vector<Post> recentPosts(std::int64_t limit) const;
    void dropMediaBefore(std::int64_t seq);

    std::int64_t appendLog(LogKind kind, const std::string& postId);
    std::vector<LogEntry> logAfter(std::int64_t seq, std::int64_t limit) const;
    std::int64_t lastSeq() const;
    std::int64_t seqBeforeLastPosts(std::int64_t count) const;

    void setReaction(const std::string& postId, const std::string& member, const std::string& emoji);
    std::map<std::string, std::int64_t> reactionCounts(
        const std::string& postId, const std::string& leavingOut) const;

    std::int64_t addRequest(const std::string& fingerprint, std::int64_t at);
    std::optional<JoinRequest> request(std::int64_t number) const;
    std::optional<JoinRequest> requestOf(const std::string& fingerprint) const;
    std::vector<JoinRequest> requests() const;
    void removeRequest(std::int64_t number);
    void addCard(std::int64_t number, const std::string& admin, const std::string& messageId);
    std::vector<RequestCard> cards(std::int64_t number) const;

    bool noteContactAsk(const std::string& requester, const std::string& author);

private:
    Database database_;
};

}  // namespace bazarish::bot
