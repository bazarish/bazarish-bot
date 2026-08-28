// Bazarish project (c) 2026
#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

namespace bazarish::daemon {

// One reaction: who put it on which message, and what it was.
struct Reaction {
    std::string messageId;
    std::string reactor;
    std::string emoji;
};

// One line of a conversation, as the interface and the API both see it.
struct Line {
    std::int64_t id = 0;
    std::string peer;
    bool outgoing = false;
    // The end-to-end content type: "text", "contact.request", "bot.command", ...
    std::string type;
    std::string e2eId;
    std::string text;
    // The inline keyboard as its wire JSON, empty when the message carries none.
    std::string keyboard;
    // The protocol id of the message this one answers, empty when it answers none.
    std::string replyTo;
    // The sender's send time in unix milliseconds - what the conversation is
    // ordered by, because the receive time is not what happened.
    std::int64_t sentAt = 0;
    // Whether the sender has since revised it, and the reactions it carries.
    bool edited = false;
    std::vector<Reaction> reactions;
};

// Something that happened, in the order it happened. A new message appends a
// line and an event; an edit, a delete, a reaction or a receipt changes a line
// that is already there and appends only an event - so a bot watching the feed
// sees all four, and a page rereading a conversation sees the result.
struct Event {
    std::int64_t id = 0;
    // "message", "edit", "delete", "reaction", "receipt".
    std::string kind;
    std::string peer;
    // The message it is about: the new one for "message", the target otherwise.
    std::string messageId;
    // The text of a message or an edit, the emoji of a reaction, empty otherwise.
    std::string text;
    // Who reacted, for "reaction": the peer, or this account.
    std::string actor;
    std::int64_t at = 0;
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

    // Appends a line and the event that says so, and returns the line's id. A
    // line whose e2eId is already there is not appended twice: the mailbox may
    // hand the same item over again. Returns 0 for such a repeat.
    std::int64_t append(const Line& line);
    // Replaces a message's text and keyboard where the sender has revised it.
    // Returns whether there was such a message here to revise.
    bool applyEdit(const std::string& peer, const std::string& messageId,
        const std::string& text, const std::string& keyboard, bool outgoing);
    // Drops a message the sender took back. Returns whether there was one.
    bool applyDelete(const std::string& peer, const std::string& messageId, bool outgoing);
    // Records a reaction, or removes it when the emoji is empty. One reaction per
    // reactor per message.
    void applyReaction(const std::string& peer, const std::string& messageId,
        const std::string& reactor, const std::string& emoji);
    // Records that the far side has the message: an event and nothing else.
    void applyReceipt(const std::string& peer, const std::string& messageId);

    // Newest first, at most `limit`, older than `before` when that is non-zero.
    std::vector<Line> conversation(
        const std::string& peer, std::int64_t before, int limit) const;
    // The last line of every conversation, most recent first.
    std::vector<Line> latestPerPeer() const;
    // Events after `id`, oldest first: what a long poll hands back.
    std::vector<Event> since(std::int64_t id, int limit) const;
    std::int64_t lastEventId() const;

private:
    // Appends one event; the caller holds the lock.
    std::int64_t noteLocked(const std::string& kind, const std::string& peer,
        const std::string& messageId, const std::string& text, const std::string& actor,
        std::int64_t at);
    void loadReactionsLocked(std::vector<Line>& lines) const;

    sqlite3* db_ = nullptr;
    mutable std::mutex mutex_;
};

}  // namespace bazarish::daemon
