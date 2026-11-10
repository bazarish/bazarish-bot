// Bazarish project (c) 2026
#include "Transcript.hpp"
#include "TestUtil.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using bazarish::bot::Event;
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
    const fs::path dir = testutil::freshDir("bot-transcript");
    const fs::path file = dir / "account.db";
    const std::string self = "selffingerprint";

    {
        Transcript transcript(file, self, true);
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

        const std::vector<bazarish::bot::Event> after = transcript.since(1);
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
        transcript.setStatus("carol", "n1", "failed: refused");
        CHECK(transcript.conversation("carol", 0, 10).front().status == "failed");
        CHECK(transcript.since(transcript.lastEventId() - 1).back().text == "failed: refused");
        transcript.setStatus("carol", "no-such-id", "delivered");

        const std::vector<bazarish::bot::Event> feed = transcript.since(0);
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
        bazarish::client::IncomingMessage receipt;
        receipt.contentType = bazarish::client::kTypeReceipt;
        receipt.fromFingerprint = "dave";
        receipt.e2eId = "rc1";
        receipt.refId = "d2";
        const Transcript::Taken read = transcript.takeIn(receipt);
        CHECK(read.wrote && !read.added);
        CHECK(transcript.since(transcript.lastEventId() - 1).back().kind == "receipt");
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
        Transcript transcript(file, self, true);
        CHECK(transcript.latestPerPeer().size() == 4);
        CHECK(transcript.conversation("bob", 0, 10).size() == 1);
        CHECK(!transcript.menu("bob"));
        transcript.setMenu("bob", {7, {{"Баланс", "balance"}}});
        transcript.setMenu("bob", {9, {{"Help", "help"}, {"Prices", "prices"}}});
    }

    {
        Transcript transcript(file, self, true);
        const std::optional<bazarish::client::BotMenu> menu = transcript.menu("bob");
        CHECK(menu && menu->id == 9 && menu->items.size() == 2);
        CHECK(menu->items.back().command == "prices");
        CHECK(!transcript.menu("carol"));
    }

    {
        Transcript transcript(file, self, true);
        const std::int64_t before = transcript.lastEventId();
        Line offered = lineFor("erin", "report.pdf", "f1", false, 600);
        offered.type = "file";
        offered.file = bazarish::bot::FileInfo{"report.pdf", 3000, "application/pdf", "ab12", {}};
        CHECK(transcript.append(offered) != 0);
        CHECK(transcript.append(lineFor("erin", "thanks", "f2", true, 601)) != 0);
        const std::vector<Line> lines = transcript.conversation("erin", 0, 10);
        CHECK(lines.size() == 2);
        CHECK(!lines[0].file.has_value());
        CHECK(lines[1].file && lines[1].file->size == 3000 && lines[1].file->sha256 == "ab12");
        CHECK(lines[1].file->path.empty());

        transcript.keepFileAt("erin", "f1", "/srv/in/report.pdf");
        transcript.noteTransfer("erin", "f1", "done");
        CHECK(transcript.find("erin", "f1")->file->path == "/srv/in/report.pdf");
        const std::vector<bazarish::bot::Event> events = transcript.since(before);
        CHECK(events.back().kind == "transfer" && events.back().text == "done");
        CHECK(events.back().messageId == "f1");

        CHECK(transcript.applyDelete("erin", "f1", false));
        CHECK(transcript.append(lineFor("erin", "again", "f1", false, 602)) != 0);
        CHECK(!transcript.find("erin", "f1")->file.has_value());

        offered.e2eId = "f3";
        CHECK(transcript.append(offered) != 0);
        transcript.clearPeer("erin");
        CHECK(transcript.conversation("erin", 0, 10).empty());
        CHECK(transcript.append(lineFor("erin", "after", "f3", false, 603)) != 0);
        CHECK(!transcript.find("erin", "f3")->file.has_value());
    }

    {
        Transcript transcript(file, self, true);
        bazarish::client::IncomingMessage text;
        text.contentType = bazarish::client::kTypeText;
        text.fromFingerprint = "frank";
        text.e2eId = "i1";
        text.text = "hello";
        text.sentAt = 700;
        const Transcript::Taken taken = transcript.takeIn(text);
        CHECK(taken.added && taken.wrote);
        CHECK(!transcript.takeIn(text).added);
        CHECK(transcript.since(transcript.lastEventId() - 1).back().kind == "message");
        CHECK(transcript.since(transcript.lastEventId() - 1).back().actor == "frank");

        bazarish::client::IncomingMessage gone;
        gone.contentType = bazarish::client::kTypeDelete;
        gone.fromFingerprint = "frank";
        gone.e2eId = "i2";
        gone.refId = "i1";
        CHECK(transcript.takeIn(gone).wrote);
        CHECK(transcript.conversation("frank", 0, 10).empty());
        CHECK(transcript.since(transcript.lastEventId() - 1).back().kind == "delete");
        CHECK(transcript.since(transcript.lastEventId() - 1).back().messageId == "i1");

        CHECK(transcript.takeIn(text).added);
        bazarish::client::IncomingMessage cleared;
        cleared.contentType = bazarish::client::kTypeChatClear;
        cleared.fromFingerprint = "frank";
        cleared.e2eId = "i3";
        CHECK(transcript.takeIn(cleared).wrote);
        const std::vector<Line> left = transcript.conversation("frank", 0, 10);
        CHECK(left.size() == 1 && left.front().type == bazarish::client::kTypeSystem);
        CHECK(transcript.since(transcript.lastEventId() - 1).back().kind == "chat-cleared");
    }

    {
        Transcript transcript(dir / "forgetful.db", self, false);
        bazarish::client::IncomingMessage text;
        text.contentType = bazarish::client::kTypeText;
        text.fromFingerprint = "gina";
        text.e2eId = "g1";
        text.text = "/start";
        CHECK(transcript.takeIn(text).added);
        CHECK(transcript.conversation("gina", 0, 10).empty());
        const bazarish::bot::Event noted = transcript.since(transcript.lastEventId() - 1).back();
        CHECK(noted.kind == "message" && noted.type == "text" && noted.text == "/start");
        const std::int64_t seen = transcript.lastEventId();
        CHECK(!transcript.takeIn(text).added);
        CHECK(transcript.lastEventId() == seen);

        bazarish::client::IncomingMessage press;
        press.contentType = bazarish::client::kTypeBotCallback;
        press.fromFingerprint = "gina";
        press.e2eId = "g2";
        press.refId = "r0";
        press.callbackData = "lang:en";
        CHECK(transcript.takeIn(press).added);
        const bazarish::bot::Event pressed = transcript.since(transcript.lastEventId() - 1).back();
        CHECK(pressed.type == "bot.callback" && pressed.text == "lang:en" && pressed.actor == "gina");

        bazarish::client::IncomingMessage request;
        request.contentType = bazarish::client::kTypeContactRequest;
        request.fromFingerprint = "hank";
        request.e2eId = "h1";
        CHECK(transcript.takeIn(request).added);
        const bazarish::bot::Event asked = transcript.since(transcript.lastEventId() - 1).back();
        CHECK(asked.type == bazarish::client::kTypeContactRequest && asked.actor == "hank");

        CHECK(transcript.append(lineFor("gina", "Choose a language.", "r1", true, 800)) != 0);
        CHECK(transcript.conversation("gina", 0, 10).empty());
        CHECK(transcript.since(transcript.lastEventId() - 1).back().text == "Choose a language.");
        CHECK(transcript.latestPerPeer().empty());
    }

    {
        constexpr std::int64_t kHour = 3600;
        constexpr std::int64_t kMsPerSecond = 1000;
        constexpr std::int64_t kSentAt = 1000;
        Transcript transcript(dir / "timer.db", self, true);

        bazarish::client::IncomingMessage timer;
        timer.contentType = bazarish::client::kTypeChatTimer;
        timer.fromFingerprint = "hana";
        timer.e2eId = "t1";
        timer.timerSeconds = kHour;
        CHECK(transcript.takeIn(timer).wrote);
        const std::vector<Event> set = transcript.since(0);
        CHECK(set.size() == 1);
        CHECK(set.back().kind == "timer" && set.back().text == std::to_string(kHour)
            && set.back().actor == "hana");
        CHECK(transcript.timer("hana") == kHour);
        CHECK(transcript.conversation("hana", 0, 10).size() == 1);

        CHECK(transcript.append(lineFor("hana", "gone in an hour", "o1", true, kSentAt)) != 0);
        CHECK(!transcript.sweepExpired(kSentAt + kHour * kMsPerSecond - 1));
        CHECK(transcript.sweepExpired(kSentAt + kHour * kMsPerSecond));
        const Event gone = transcript.since(transcript.lastEventId() - 1).back();
        CHECK(gone.kind == "delete" && gone.messageId == "o1");
        CHECK(!transcript.find("hana", "o1").has_value());

        CHECK(transcript.append(lineFor("hana", "read me", "i1", false, kSentAt)) != 0);
        transcript.markRead("hana", "i1");
        CHECK(!transcript.sweepExpired(kSentAt + kHour * kMsPerSecond));
        CHECK(transcript.find("hana", "i1").has_value());

        transcript.setTimer("hana", 0);
        const Event off = transcript.since(transcript.lastEventId() - 1).back();
        CHECK(off.kind == "timer" && off.text == "0" && off.actor.empty());
        CHECK(transcript.timer("hana") == 0);
    }

    fs::remove_all(dir);
    std::fprintf(stderr, "TestTranscript passed\n");
    return 0;
}
