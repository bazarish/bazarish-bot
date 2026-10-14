// Bazarish project (c) 2026
#pragma once

#include "GroupCopy.hpp"
#include "GroupStore.hpp"

#include <Session.hpp>

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bazarish::bot {

inline constexpr std::int64_t kMemberCountPeriodMs = 60 * 60 * 1000;
inline constexpr std::int64_t kReactionFlushMs = 30 * 1000;
inline constexpr std::int64_t kRelayRetryMs = 60 * 1000;
inline constexpr std::size_t kMaxRelaysInFlight = 16;

struct GroupLink {
    using Outcome = std::function<void(const client::OutboundCourier::Outcome&)>;

    std::function<void(const std::string& peer)> accept;
    std::function<std::string(const std::string& peer, const std::string& text,
        const client::InlineKeyboard& keyboard)>
        tell;
    std::function<void(const std::string& peer, const std::string& messageId,
        const std::string& text, const client::InlineKeyboard& keyboard)>
        retell;
    std::function<void(const std::string& peer, const client::RelayedPost& post, Outcome done)>
        relay;
    std::function<void(
        const std::string& peer, const std::string& ref, const std::string& text, Outcome done)>
        edit;
    std::function<void(const std::string& peer, const std::string& ref, Outcome done)> remove;
    std::function<void(const std::string& peer, const nlohmann::json& reactions, Outcome done)>
        reactions;
    std::function<void(const std::string& peer)> state;
    std::function<void(const std::string& peer, const std::string& messageId)> receipt;
    std::function<std::string(const std::string& peer)> descriptor;
    std::function<void(const std::string& peer)> forget;
    std::function<bool(const client::RelayedPost& post)> fits;
    std::function<std::optional<Bytes>(const std::string& id, const std::string& type)> media;
};

class GroupEngine {
public:
    using Clock = std::function<std::int64_t()>;

    GroupEngine(const std::filesystem::path& file, const std::string& passphrase, GroupLink link,
        Clock clock);
    ~GroupEngine();

    GroupEngine(const GroupEngine&) = delete;
    GroupEngine& operator=(const GroupEngine&) = delete;

    void start();
    void stop();

    void onIncoming(const client::IncomingMessage& message);
    client::GroupBlock blockFor(const std::string& peer) const;

    GroupConfig config() const;
    void setConfig(const GroupConfig& config);
    std::vector<Member> members() const;
    void setAdmin(const std::string& fingerprint, bool admin);
    void exclude(const std::string& fingerprint);
    void block(const std::string& fingerprint);
    void unblock(const std::string& fingerprint);
    std::vector<JoinRequest> requests() const;
    void decide(std::int64_t number, bool allow);
    std::vector<Post> posts(std::int64_t limit) const;
    std::map<std::string, std::int64_t> reactionCounts(const std::string& postId) const;
    void deletePost(const std::string& id);
    std::int64_t memberCount() const;

    void pumpOnce();
    void flushReactions();
    void refreshMemberCount();

private:
    using Actions = std::vector<std::function<void()>>;

    struct Prefetched {
        std::optional<Bytes> media;
        bool fits = false;
    };

    void receiptLocked(const client::IncomingMessage& message, Actions& actions);
    void handleLocked(
        const client::IncomingMessage& message, const Prefetched& prefetched, Actions& actions);
    void joinLocked(Member& member, Actions& actions);
    void admitLocked(Member& member, Actions& actions);
    void decideLocked(std::int64_t number, bool allow, const std::string& byName, Actions& actions);
    void postLocked(Member& member, const client::IncomingMessage& message,
        const Prefetched& prefetched, Actions& actions);
    void commandLocked(Member& member, const std::string& line, Actions& actions);
    void excludeLocked(const std::string& fingerprint, Actions& actions);
    void blockLocked(const std::string& fingerprint, Actions& actions);
    void deletePostLocked(const std::string& id);
    void tellLocked(const std::string& peer, Copy key, const std::vector<std::string>& values,
        Actions& actions, const client::InlineKeyboard& keyboard = {});
    void sayLocked(const std::string& peer, const std::string& words, Actions& actions,
        const client::InlineKeyboard& keyboard = {});
    void askNameLocked(const std::string& peer, Actions& actions);
    std::string welcomeTextLocked(const Member& member) const;
    void offerSettingsLocked(const Member& member, Actions& actions);
    void showSettingsLocked(const Member& member, const std::string& ref, Actions& actions);
    client::InlineKeyboard settingsButtonLocked() const;
    client::InlineKeyboard settingsKeyboardLocked(const Member& member) const;
    std::string chatDescriptor(const Member& member) const;
    std::string text(Copy key, const std::vector<std::string>& values = {}) const;
    bool writesLocked(const Member& member) const;
    void saveLocked(const Member& member);
    void stateChangedLocked(const std::string& fingerprint, Actions& actions);
    void deliveredLocked(const std::string& fingerprint, std::int64_t seq,
        const client::OutboundCourier::Outcome& outcome);
    std::optional<std::function<void()>> nextRelayLocked(Member& member);
    void run(Actions& actions);
    void nudge();
    void loop();

    GroupStore store_;
    GroupLink link_;
    Clock clock_;
    GroupConfig config_;
    std::map<std::string, Member> members_;
    std::map<std::string, std::int64_t> retryAt_;
    std::map<std::string, bool> inFlight_;
    std::vector<std::string> dirtyReactions_;
    std::int64_t memberCount_ = 0;
    std::int64_t countedAt_ = 0;
    std::int64_t flushedAt_ = 0;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    bool nudged_ = false;
    std::thread pump_;
};

}  // namespace bazarish::bot
