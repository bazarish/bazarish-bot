// Bazarish project (c) 2026
#pragma once

#include "Database.hpp"

#include <BotMenu.hpp>
#include <Session.hpp>
#include <TranscriptDb.hpp>
#include <TranscriptIngest.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bazarish::bot {

struct FileInfo {
    std::string name;
    std::uint64_t size = 0;
    std::string mime;
    std::string sha256;
    std::string path;
};

struct Reaction {
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
    std::optional<FileInfo> file;
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

// The conversations sit in the account's database the way the client keeps them; the event feed of
// the API and the menus this account gave out are its own tables in the same file.
class Transcript {
public:
    Transcript(const std::filesystem::path& accountFile, std::string ownFingerprint);
    ~Transcript();

    Transcript(const Transcript&) = delete;
    Transcript& operator=(const Transcript&) = delete;

    struct Taken {
        bool wrote = false;
        bool added = false;
    };
    // Throws when the store refused it, so the message stays in the mailbox.
    Taken takeIn(const client::IncomingMessage& message);

    std::int64_t append(const Line& line);
    bool applyEdit(const std::string& peer, const std::string& messageId,
        const std::string& text, const std::string& keyboard, bool outgoing);
    bool applyDelete(const std::string& peer, const std::string& messageId, bool outgoing);
    void applyReaction(const std::string& peer, const std::string& messageId,
        const std::string& reactor, const std::string& emoji);
    void clearPeer(const std::string& peer);
    // What was still on its way when the account last closed did not leave.
    void failUnfinished();
    void setStatus(
        const std::string& peer, const std::string& messageId, const std::string& status);
    void keepFileAt(
        const std::string& peer, const std::string& messageId, const std::string& path);
    void noteTransfer(
        const std::string& peer, const std::string& messageId, const std::string& state);
    void setMenu(const std::string& peer, const client::BotMenu& menu);
    std::optional<client::BotMenu> menu(const std::string& peer) const;

    std::vector<Line> conversation(
        const std::string& peer, std::int64_t before, int limit) const;
    std::vector<Line> latestPerPeer() const;
    std::optional<Line> find(const std::string& peer, const std::string& messageId) const;
    std::vector<Event> since(std::int64_t id) const;
    std::int64_t lastEventId() const;

    client::DatabaseFootprint footprint() const;
    void rebuild();

private:
    std::int64_t noteLocked(const std::string& kind, const std::string& peer,
        const std::string& messageId, const std::string& text, const std::string& actor,
        std::int64_t at);
    Line lineOf(const client::StoredMessage& row) const;
    void require(bool done) const;

    class Relay;

    std::string own_;
    client::TranscriptStore store_;
    Database ledger_;
    std::unique_ptr<Relay> relay_;
    std::unique_ptr<client::TranscriptIngest> ingest_;
    mutable std::mutex mutex_;
};

}  // namespace bazarish::bot
