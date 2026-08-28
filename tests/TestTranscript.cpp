// Bazarish project (c) 2026
#include "Transcript.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

namespace fs = std::filesystem;
using bazarish::daemon::Line;
using bazarish::daemon::Transcript;

namespace {

Line lineFor(const std::string& peer, const std::string& text, const std::string& e2eId,
    const bool outgoing, const std::int64_t sentAt)
{
    Line line;
    line.peer = peer;
    line.outgoing = outgoing;
    line.type = "text";
    line.e2eId = e2eId;
    line.text = text;
    line.sentAt = sentAt;
    return line;
}

}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / "bz-daemon-transcript";
    fs::remove_all(dir);
    const fs::path file = dir / "transcript.db";

    {
        Transcript transcript(file, "pw");
        CHECK(transcript.lastId() == 0);
        CHECK(transcript.peers().empty());

        const std::int64_t first = transcript.append(lineFor("alice", "hello", "m1", false, 10));
        CHECK(first > 0);
        CHECK(transcript.append(lineFor("alice", "hi", "m2", true, 20)) > first);
        CHECK(transcript.append(lineFor("bob", "hey", "m3", false, 30)) > 0);

        // The mailbox may hand the same item over again; that is one message.
        CHECK(transcript.append(lineFor("alice", "hello", "m1", false, 10)) == 0);

        const std::vector<Line> withAlice = transcript.conversation("alice", 0, 50);
        CHECK(withAlice.size() == 2);
        CHECK(withAlice.front().text == "hi");      // newest first
        CHECK(withAlice.front().outgoing);
        CHECK(withAlice.back().text == "hello");
        CHECK(withAlice.back().sentAt == 10);

        // A page older than what is held.
        const std::vector<Line> older
            = transcript.conversation("alice", withAlice.front().id, 50);
        CHECK(older.size() == 1);
        CHECK(older.front().text == "hello");

        const std::vector<std::string> peers = transcript.peers();
        CHECK(peers.size() == 2);
        CHECK(peers.front() == "bob");  // most recent first

        const std::vector<Line> chats = transcript.latestPerPeer();
        CHECK(chats.size() == 2);
        CHECK(chats.front().peer == "bob");

        const std::vector<Line> after = transcript.since(first, 50);
        CHECK(after.size() == 2);
        CHECK(after.front().e2eId == "m2");  // oldest first
        CHECK(transcript.lastId() == after.back().id);
    }

    // What was written is there on the next open, and only with the passphrase.
    {
        Transcript transcript(file, "pw");
        CHECK(transcript.peers().size() == 2);
        CHECK(transcript.conversation("bob", 0, 10).size() == 1);
    }

    fs::remove_all(dir);
    std::fprintf(stderr, "TestTranscript passed\n");
    return 0;
}
