// Bazarish project (c) 2026
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace bazarish::daemon {

struct Reaction {
    std::string messageId;
    std::string reactor;
    std::string emoji;
};

struct Line {
    std::int64_t id = 0;
    std::string peer;
    bool outgoing = false;
    std::string type;
    std::string e2eId;
    std::string text;
    std::string keyboard;
    std::string replyTo;
    std::int64_t sentAt = 0;
    bool edited = false;
    std::vector<Reaction> reactions;
    std::string status;
};

struct Event {
    std::int64_t id = 0;
    std::string kind;
    std::string peer;
    std::string messageId;
    std::string text;
    std::string actor;
    std::int64_t at = 0;
};

class Transcript {
public:
    Transcript(const std::filesystem::path& file, const std::string& passphrase);
    ~Transcript();

    Transcript(const Transcript&) = delete;
    Transcript& operator=(const Transcript&) = delete;

    std::int64_t append(const Line& line);
    bool applyEdit(const std::string& peer, const std::string& messageId,
        const std::string& text, const std::string& keyboard, bool outgoing);
    bool applyDelete(const std::string& peer, const std::string& messageId, bool outgoing);
    void applyReaction(const std::string& peer, const std::string& messageId,
        const std::string& reactor, const std::string& emoji);
    void applyReceipt(const std::string& peer, const std::string& messageId);
    int clearPeer(const std::string& peer);
    void setStatus(
        const std::string& peer, const std::string& messageId, const std::string& status);

    std::vector<Line> conversation(
        const std::string& peer, std::int64_t before, int limit) const;
    std::vector<Line> latestPerPeer() const;
    std::vector<Event> since(std::int64_t id, int limit) const;
    std::int64_t lastEventId() const;

private:
    std::int64_t noteLocked(const std::string& kind, const std::string& peer,
        const std::string& messageId, const std::string& text, const std::string& actor,
        std::int64_t at);
    void loadReactionsLocked(std::vector<Line>& lines) const;

    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

}  // namespace bazarish::daemon
