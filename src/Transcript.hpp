// Bazarish project (c) 2026
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace bazarish::daemon {

// One line of a conversation, as the interface and the API both see it.
struct Line {
    std::int64_t id = 0;
    std::string peer;
    bool outgoing = false;
    // The end-to-end content type: "text", "file", "picture", "voice", ...
    std::string type;
    std::string e2eId;
    std::string text;
    // The name of what was attached, for the types that carry one.
    std::string attachmentName;
    // The inline keyboard as its wire JSON, empty when the message carries none.
    std::string keyboard;
    // The protocol id of the message this one answers, empty when it answers none.
    std::string replyTo;
    // The sender's send time in unix milliseconds - what the conversation is
    // ordered by, because the receive time is not what happened.
    std::int64_t sentAt = 0;
};

// What the daemon remembers of its conversations. The client core keeps no
// transcript - it hands each item over once and forgets it - so a service that
// is asked "what did they say" has to write it down itself.
//
// The account database is beside this one and is encrypted; this one holds the
// same conversations and is encrypted with the same key.
class Transcript {
public:
    Transcript(const std::filesystem::path& file, const std::string& passphrase);
    ~Transcript();

    Transcript(const Transcript&) = delete;
    Transcript& operator=(const Transcript&) = delete;

    // Appends a line and returns its id. A line whose e2eId is already there is
    // not appended twice: the mailbox may hand the same item over again.
    std::int64_t append(const Line& line);
    // Newest first, at most `limit`, older than `before` when that is non-zero.
    std::vector<Line> conversation(
        const std::string& peer, std::int64_t before, int limit) const;
    // Every peer this account has a line with, most recent first.
    std::vector<std::string> peers() const;
    // The last line of a conversation, for the list of chats.
    std::vector<Line> latestPerPeer() const;
    // Lines after `id`, oldest first: what a long poll hands back.
    std::vector<Line> since(std::int64_t id, int limit) const;
    // The id of the newest line, 0 when there is none.
    std::int64_t lastId() const;

private:
    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

}  // namespace bazarish::daemon
