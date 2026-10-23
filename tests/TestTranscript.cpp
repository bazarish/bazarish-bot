// Bazarish project (c) 2026
#include "Transcript.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
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
using bazarish::bot::Line;
using bazarish::bot::Transcript;

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
    const fs::path dir = fs::temp_directory_path() / "bz-bot-transcript";
    fs::remove_all(dir);
    const fs::path file = dir / "transcript.db";

    {
        Transcript transcript(file, "pw");
        CHECK(transcript.lastEventId() == 0);
        CHECK(transcript.latestPerPeer().empty());

        const std::int64_t first = transcript.append(lineFor("alice", "hello", "m1", false, 10));
        CHECK(first > 0);
        CHECK(transcript.append(lineFor("alice", "hi", "m2", true, 20)) > first);
        CHECK(transcript.append(lineFor("bob", "hey", "m3", false, 30)) > 0);

        CHECK(transcript.append(lineFor("alice", "hello", "m1", false, 10)) == 0);

        const std::vector<Line> withAlice = transcript.conversation("alice", 0, 50);
        CHECK(withAlice.size() == 2);
        CHECK(withAlice.front().text == "hi");
        CHECK(withAlice.front().outgoing);
        CHECK(withAlice.back().text == "hello");
        CHECK(withAlice.back().sentAt == 10);

        const std::vector<Line> older
            = transcript.conversation("alice", withAlice.front().id, 50);
        CHECK(older.size() == 1);
        CHECK(older.front().text == "hello");

        const std::vector<Line> chats = transcript.latestPerPeer();
        CHECK(chats.size() == 2);
        CHECK(chats.front().peer == "bob");

        const std::vector<bazarish::bot::Event> after = transcript.since(1, 50);
        CHECK(after.size() == 2);
        CHECK(after.front().messageId == "m2");
        CHECK(after.front().kind == "message");
        CHECK(transcript.lastEventId() == after.back().id);

        CHECK(transcript.applyEdit("alice", "m2", "hi there", {}, true));
        CHECK(!transcript.applyEdit("alice", "m2", "wrong side", {}, false));
        CHECK(!transcript.applyEdit("alice", "no-such-id", "nothing", {}, true));
        transcript.applyReaction("alice", "m2", "alice", "\xF0\x9F\x91\x8D");
        transcript.applyReaction("alice", "m2", "me", "\xE2\x9D\xA4");

        const std::vector<Line> edited = transcript.conversation("alice", 0, 50);
        CHECK(edited.front().text == "hi there");
        CHECK(edited.front().edited);
        CHECK(edited.front().reactions.size() == 2);

        transcript.applyReaction("alice", "m2", "alice", "\xF0\x9F\x8E\x89");
        CHECK(transcript.conversation("alice", 0, 50).front().reactions.size() == 2);
        transcript.applyReaction("alice", "m2", "alice", "");
        CHECK(transcript.conversation("alice", 0, 50).front().reactions.size() == 1);

        CHECK(transcript.applyDelete("alice", "m2", true));
        CHECK(!transcript.applyDelete("alice", "m2", true));
        CHECK(transcript.conversation("alice", 0, 50).size() == 1);

        transcript.append(lineFor("carol", "notice", "n1", true, 50));
        transcript.setStatus("carol", "n1", "delivered");
        CHECK(transcript.conversation("carol", 0, 10).front().status == "delivered");
        transcript.setStatus("carol", "n1", "failed: no tokens");
        CHECK(transcript.conversation("carol", 0, 10).front().status == "failed: no tokens");
        transcript.setStatus("carol", "no-such-id", "delivered");

        const std::vector<bazarish::bot::Event> feed = transcript.since(0, 100);
        std::string kinds;
        for (const bazarish::bot::Event& event : feed) {
            kinds += event.kind + " ";
        }
        CHECK(kinds
            == "message message message edit reaction reaction reaction reaction delete "
               "message status status ");

        for (const char* const id : {"d1", "d2", "d3"}) {
            transcript.append(lineFor("dave", id, id, true, 60));
        }
        transcript.setStatus("dave", "d1", "delivered");
        transcript.setStatus("dave", "d2", "delivered");
        transcript.setStatus("dave", "d3", "sending");
        transcript.applyReceipt("dave", "d2");
        transcript.setStatus("dave", "d1", "delivered");
        std::map<std::string, std::string> states;
        for (const Line& line : transcript.conversation("dave", 0, 10)) {
            states[line.e2eId] = line.status;
        }
        CHECK(states["d1"] == "read");
        CHECK(states["d2"] == "read");
        CHECK(states["d3"] == "sending");
    }

    {
        Transcript transcript(file, "pw");
        CHECK(transcript.latestPerPeer().size() == 4);
        CHECK(transcript.conversation("bob", 0, 10).size() == 1);
        CHECK(!transcript.menu("bob"));
        transcript.setMenu("bob", {7, {{"Баланс", "balance"}}});
        transcript.setMenu("bob", {9, {{"Help", "help"}, {"Prices", "prices"}}});
    }

    {
        Transcript transcript(file, "pw");
        const std::optional<bazarish::client::BotMenu> menu = transcript.menu("bob");
        CHECK(menu && menu->id == 9 && menu->items.size() == 2);
        CHECK(menu->items.back().command == "prices");
        CHECK(!transcript.menu("carol"));
    }

    fs::remove_all(dir);
    std::fprintf(stderr, "TestTranscript passed\n");
    return 0;
}
