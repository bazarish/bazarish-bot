// Bazarish project (c) 2026
#include "GroupEngine.hpp"
#include "TestUtil.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Crypto.hpp>
#include <bazarish/Descriptor.hpp>
#include <bazarish/KeyedSeal.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace bazarish;
using namespace bazarish::bot;

namespace {

constexpr const char* kDest = "dlkbeyqjykssca6o7qlbwgq4fr2hry7kw2ursn2sh3lt3acox6gqaaaa.b32.i2p";
constexpr int kPumpRounds = 8;
constexpr const char* kAskName = "Choose your name in this chat.";
// The minute the engine once waited before it sent a refused relay again.
constexpr std::int64_t kOldRetryMs = 60 * 1000;
constexpr std::chrono::seconds kRelayWait{10};
constexpr std::chrono::milliseconds kSettle{200};
// Longer than the second a started engine once slept between looks at its clock.
constexpr std::chrono::milliseconds kIdleWatch{1500};

struct Sent {
    std::string kind;
    std::string peer;
    std::string id;
    std::string text;
    std::string author;
    client::InlineKeyboard keyboard;
    nlohmann::json reactions;
    bool fromHost = false;
    std::string after;
};

struct Fake {
    std::vector<Sent> sent;
    std::vector<std::string> accepted;
    std::vector<std::string> forgotten;
    std::vector<std::string> states;
    std::map<std::string, std::string> refuse;
    // Members whose server does not answer: their relays wait in the outbox.
    std::set<std::string> away;
    std::map<std::string, client::DeliveryWatch> parked;
    std::vector<std::string> withdrawn;
    std::map<std::string, std::string> names;
    std::map<std::string, int> relayCalls;
    // Members whose relays fail on this device before they leave it.
    std::set<std::string> broken;
    std::size_t batchCap = std::numeric_limits<std::size_t>::max();
    std::optional<Bytes> mediaBytes;
    int told = 0;

    client::OutboundCourier::Outcome outcomeFor(const std::string& peer) const
    {
        client::OutboundCourier::Outcome outcome;
        const auto found = refuse.find(peer);
        outcome.stored = found == refuse.end();
        if (!outcome.stored) {
            outcome.errorCode = found->second;
        }
        return outcome;
    }

    std::vector<Sent> take(const std::string& kind, const std::string& peer)
    {
        std::vector<Sent> matched;
        std::vector<Sent> rest;
        for (Sent& item : sent) {
            (item.kind == kind && item.peer == peer ? matched : rest).push_back(item);
        }
        sent = rest;
        return matched;
    }

    void forget() { sent.clear(); }
};

GroupLink linkFor(Fake& fake)
{
    GroupLink link;
    link.accept = [&fake](const std::string& peer) { fake.accepted.push_back(peer); };
    link.tell = [&fake](const std::string& peer, const std::string& text,
                    const client::InlineKeyboard& keyboard, const GroupLink::Outcome& done) {
        const std::string id = "told-" + std::to_string(++fake.told);
        fake.sent.push_back({"tell", peer, id, text, {}, keyboard, {}, false, {}});
        done(fake.outcomeFor(peer));
        return id;
    };
    link.retell = [&fake](const std::string& peer, const std::string& id, const std::string& text,
                      const client::InlineKeyboard& keyboard) {
        fake.sent.push_back({"retell", peer, id, text, {}, keyboard, {}, false, {}});
    };
    link.relay = [&fake](const std::string& peer, const std::vector<client::RelayedPost>& posts,
                     const client::DeliveryWatch& watch) {
        ++fake.relayCalls[peer];
        if (fake.broken.count(peer) != 0) {
            throw std::runtime_error("no delivery pass for this contact yet: " + peer);
        }
        for (const client::RelayedPost& post : posts) {
            switch (post.kind) {
            case client::RelayKind::ePost:
                fake.sent.push_back({"relay", peer, post.id, post.text, post.author, {}, {},
                    post.fromHost, post.after});
                break;
            case client::RelayKind::eEdit:
                fake.sent.push_back({"edit", peer, post.ref, post.text, {}, {}, {}, false, {}});
                break;
            case client::RelayKind::eDelete:
                fake.sent.push_back({"delete", peer, post.ref, {}, {}, {}, {}, false, {}});
                break;
            case client::RelayKind::eReactions:
                fake.sent.push_back({"reactions", peer, {}, {}, {}, {}, post.reactions, false, {}});
                break;
            case client::RelayKind::eMembers:
                fake.sent.push_back({"members", peer, {}, {}, {}, {},
                    client::memberListToJson(post.members), false, {}});
                break;
            }
        }
        if (fake.away.count(peer) != 0) {
            fake.parked[peer] = watch;
            watch.onPhase(client::kPhaseWaiting);
            return;
        }
        watch.onOutcome(fake.outcomeFor(peer));
    };
    link.state = [&fake](const std::string& peer, const GroupLink::Outcome& done) {
        fake.states.push_back(peer);
        done(fake.outcomeFor(peer));
    };
    link.receipt = [&fake](const std::string& peer, const std::string& id) {
        fake.sent.push_back({"receipt", peer, id, {}, {}, {}, {}, false, {}});
    };
    link.descriptor = [](const std::string& peer) {
        return encodeDescriptor(Descriptor{peer, kDest, newLinkKey(), "theirs"});
    };
    link.name = [&fake](const std::string& peer) {
        const auto found = fake.names.find(peer);
        return found == fake.names.end() ? std::string() : found->second;
    };
    link.forget = [&fake](const std::string& peer) { fake.forgotten.push_back(peer); };
    link.withdraw = [&fake](const std::string& peer) { fake.withdrawn.push_back(peer); };
    link.fitting = [&fake](const std::vector<client::RelayedPost>& posts) {
        std::size_t count = 0;
        while (count < posts.size() && count < fake.batchCap && posts[count].text != "huge") {
            ++count;
        }
        return count;
    };
    link.media = [&fake](const std::string&, const std::string&) { return fake.mediaBytes; };
    return link;
}

client::IncomingMessage incoming(const std::string& from, const std::string& type)
{
    client::IncomingMessage message;
    message.fromFingerprint = from;
    message.contentType = type;
    return message;
}

client::IncomingMessage textFrom(
    const std::string& from, const std::string& text, const std::string& id = {})
{
    client::IncomingMessage message = incoming(from, "text");
    message.text = text;
    message.e2eId = id;
    return message;
}

client::IncomingMessage commandFrom(
    const std::string& from, const std::string& name, const std::string& args)
{
    client::IncomingMessage message = incoming(from, "bot.command");
    message.commandName = name;
    message.commandArgs = args;
    return message;
}

void pump(GroupEngine& engine)
{
    for (int round = 0; round < kPumpRounds; ++round) {
        engine.pumpOnce();
    }
}

void join(GroupEngine& engine, Fake& fake, const std::string& peer, const std::string& name)
{
    fake.names[peer] = name;
    engine.onIncoming(incoming(peer, "contact.request"));
}

std::string member()
{
    return Identity::generate().fingerprint();
}

constexpr std::size_t kCrowdMembers = client::kMaxMemberNamesPerLetter + 1;
constexpr int kCrowdNameDigits = 4;

nlohmann::json onlyLetter(Fake& fake, const std::string& peer)
{
    const std::vector<Sent> letters = fake.take("members", peer);
    CHECK(letters.size() == 1);
    return letters.at(0).reactions;
}

std::vector<std::string> namesIn(const nlohmann::json& letter)
{
    return letter.at("names").get<std::vector<std::string>>();
}

void checkMemberList(const fs::path& dir)
{
    std::int64_t now = 1000;
    Fake fake;
    GroupEngine engine(dir / "roster.db", linkFor(fake), [&now]() { return now; });
    const std::string ann = "fp-ann";
    const std::string bea = "fp-bea";
    const std::string cal = "fp-cal";

    join(engine, fake, ann, "Ann");
    pump(engine);
    nlohmann::json letter = onlyLetter(fake, ann);
    CHECK(letter.at("op") == "full");
    CHECK(letter.at("part") == 1 && letter.at("parts") == 1 && letter.at("total") == 1);
    CHECK(namesIn(letter) == std::vector<std::string>{"Ann"});
    CHECK(letter.at("you") == "Ann");

    join(engine, fake, bea, "Anna Maria");
    pump(engine);
    letter = onlyLetter(fake, ann);
    CHECK(letter.at("op") == "added");
    CHECK(namesIn(letter) == std::vector<std::string>{"Anna Maria"});
    CHECK(letter.at("total") == 2);
    CHECK(!letter.contains("you"));
    letter = onlyLetter(fake, bea);
    CHECK(letter.at("op") == "full" && letter.at("total") == 2);
    CHECK(letter.at("you") == "Anna Maria");

    join(engine, fake, cal, "anna_maria");
    CHECK(fake.take("tell", cal).back().text == "The name anna_maria is taken. Choose another.");
    engine.onIncoming(textFrom(cal, "Cal"));
    pump(engine);
    CHECK(onlyLetter(fake, ann).at("total") == 3);
    CHECK(onlyLetter(fake, bea).at("total") == 3);
    fake.forget();

    engine.onIncoming(commandFrom(bea, "rename", ""));
    engine.onIncoming(textFrom(bea, "Bea"));
    pump(engine);
    std::vector<Sent> renamed = fake.take("members", ann);
    CHECK(renamed.size() == 2);
    CHECK(renamed[0].reactions.at("op") == "removed");
    CHECK(namesIn(renamed[0].reactions) == std::vector<std::string>{"Anna Maria"});
    CHECK(renamed[0].reactions.at("total") == 2);
    CHECK(renamed[1].reactions.at("op") == "added");
    CHECK(renamed[1].reactions.at("total") == 3);
    CHECK(!renamed[1].reactions.contains("you"));
    renamed = fake.take("members", bea);
    CHECK(renamed.size() == 2);
    CHECK(renamed[1].reactions.at("you") == "Bea");

    engine.onIncoming(textFrom(ann, "@Bea look", "m1"));
    pump(engine);
    const std::vector<Sent> relayed = fake.take("relay", bea);
    CHECK(relayed.size() == 1 && relayed[0].text == "@Bea look");
    engine.onIncoming(textFrom(ann, "and this", "m2"));
    pump(engine);
    const std::vector<Sent> next = fake.take("relay", bea);
    CHECK(next.size() == 1 && next[0].after == "m1");
    fake.forget();

    engine.onIncoming(incoming(ann, "group.members.ask"));
    pump(engine);
    letter = onlyLetter(fake, ann);
    CHECK(letter.at("op") == "full" && letter.at("total") == 3);
    std::vector<std::string> everyone = namesIn(letter);
    std::sort(everyone.begin(), everyone.end());
    CHECK(everyone == (std::vector<std::string>{"Ann", "Bea", "Cal"}));
    CHECK(letter.at("you") == "Ann");
    CHECK(fake.take("members", bea).empty());

    engine.exclude(cal);
    pump(engine);
    letter = onlyLetter(fake, ann);
    CHECK(letter.at("op") == "removed" && letter.at("total") == 2);
    CHECK(fake.take("members", cal).empty());
    engine.onIncoming(incoming(cal, "group.members.ask"));
    pump(engine);
    CHECK(fake.take("members", cal).empty());
    fake.forget();

    GroupConfig config = engine.config();
    config.kind = client::GroupKind::eChannel;
    engine.setConfig(config);
    pump(engine);
    CHECK(onlyLetter(fake, ann).at("op") == "none");
    CHECK(onlyLetter(fake, bea).at("op") == "none");
    engine.onIncoming(commandFrom(bea, "rename", ""));
    engine.onIncoming(textFrom(bea, "Bee"));
    engine.onIncoming(incoming(ann, "group.members.ask"));
    pump(engine);
    letter = onlyLetter(fake, ann);
    CHECK(letter.at("op") == "none");
    CHECK(!letter.contains("names"));
    CHECK(fake.take("members", bea).empty());
}

std::string crowdName(const std::size_t number)
{
    std::string digits = std::to_string(number);
    digits.insert(0, static_cast<std::size_t>(kCrowdNameDigits) - digits.size(), '0');
    std::string name = "M";
    name += digits;
    return name;
}

void checkListInParts(const fs::path& dir)
{
    std::int64_t now = 1000;
    Fake fake;
    GroupEngine engine(dir / "parts.db", linkFor(fake), [&now]() { return now; });
    const std::string first = "a-first";
    join(engine, fake, first, "First");
    for (std::size_t number = 1; number < kCrowdMembers; ++number) {
        std::string peer = "m-";
        peer += crowdName(number);
        join(engine, fake, peer, crowdName(number));
    }
    engine.onIncoming(incoming(first, "group.members.ask"));
    engine.pumpOnce();
    const std::vector<Sent> parts = fake.take("members", first);
    CHECK(parts.size() == 2);
    std::size_t names = 0;
    bool you = false;
    for (std::size_t at = 0; at < parts.size(); ++at) {
        const nlohmann::json& part = parts[at].reactions;
        CHECK(part.at("op") == "full");
        CHECK(part.at("part") == static_cast<std::int64_t>(at + 1));
        CHECK(part.at("parts") == 2);
        CHECK(part.at("total") == static_cast<std::int64_t>(kCrowdMembers));
        CHECK(namesIn(part).size() <= client::kMaxMemberNamesPerLetter);
        names += namesIn(part).size();
        you = you || (part.contains("you") && part.at("you") == "First");
    }
    CHECK(names == kCrowdMembers);
    CHECK(you);
}

}  // namespace

int main()
try {
    const testutil::ScratchDir scratch("group");
    const fs::path& dir = scratch.path();
    std::int64_t now = 1000;
    Fake fake;
    const std::string ann = Identity::generate().fingerprint();
    const std::string bob = Identity::generate().fingerprint();
    const std::string cid = Identity::generate().fingerprint();
    const std::string dan = Identity::generate().fingerprint();
    const std::string eve = Identity::generate().fingerprint();
    const std::string ivy = Identity::generate().fingerprint();
    const std::string kIvyIntro = "Hello, I am Ivy.\nA friend sent me.";

    {
        GroupEngine engine(dir / "group.db", linkFor(fake), [&now]() { return now; });

        engine.onIncoming(incoming(ann, "contact.request"));
        CHECK(fake.accepted == std::vector<std::string>{ann});
        const std::vector<Sent> named = fake.take("tell", ann);
        CHECK(named.size() == 1);
        CHECK(named[0].text == kAskName);
        engine.onIncoming(incoming(ann, "contact.request"));
        CHECK(fake.take("tell", ann).empty());
        engine.onIncoming(incoming(ann, "receipt"));
        CHECK(fake.take("tell", ann).empty());
        CHECK(engine.blockFor(ann).write);
        engine.onIncoming(textFrom(ann, "Ann"));
        CHECK(fake.take("tell", ann).empty());
        CHECK(engine.blockFor(ann).write);

        engine.onIncoming(commandFrom(ann, "settings", ""));
        const std::vector<Sent> settings = fake.take("tell", ann);
        CHECK(settings.size() == 1);
        CHECK(settings[0].text == "Your name in this group: Ann.");
        CHECK(settings[0].keyboard.size() == 2);
        CHECK(settings[0].keyboard.at(0).at(0).data == "contactable off");
        CHECK(settings[0].keyboard.at(1).at(0).data == "rename");

        const auto press = [&](const std::string& data) {
            client::IncomingMessage callback = incoming(ann, "bot.callback");
            callback.callbackData = data;
            callback.refId = settings[0].id;
            engine.onIncoming(callback);
            const std::vector<Sent> retold = fake.take("retell", ann);
            CHECK(retold.size() == 1);
            CHECK(retold[0].id == settings[0].id);
            CHECK(retold[0].text == settings[0].text);
            CHECK(retold[0].keyboard.size() == 2);
            CHECK(retold[0].keyboard.at(1).at(0).data == "rename");
            return retold[0].keyboard.at(0).at(0).data;
        };
        CHECK(press("contactable off") == "contactable on");
        CHECK(press("contactable on") == "contactable off");

        engine.onIncoming(textFrom(ann, " /settings\n", "p-settings"));
        CHECK(fake.take("tell", ann).empty());
        engine.onIncoming(commandFrom(ann, "settings", ""));
        const std::vector<Sent> offered = fake.take("tell", ann);
        CHECK(offered.size() == 1);
        CHECK(offered[0].text == "Your name in this group: Ann.");
        CHECK(offered[0].keyboard.at(0).at(0).data == "contactable off");

        join(engine, fake, bob, "ann");
        CHECK(fake.take("tell", bob).back().text == "The name ann is taken. Choose another.");
        CHECK(engine.blockFor(bob).write);
        engine.onIncoming(textFrom(bob, "Bob"));
        CHECK(fake.take("tell", bob).empty());
        join(engine, fake, cid, "Ace");
        CHECK(fake.take("tell", cid).empty());
        join(engine, fake, dan, "Асе");
        CHECK(fake.take("tell", dan).back().text == "The name Асе is taken. Choose another.");
        engine.onIncoming(textFrom(dan, "Dan"));
        fake.forget();

        engine.onIncoming(textFrom(ann, "first", "p1"));
        const std::vector<Sent> receipts = fake.take("receipt", ann);
        CHECK(receipts.size() == 1);
        CHECK(receipts[0].id == "p1");
        pump(engine);
        CHECK(fake.take("relay", ann).empty());
        const std::vector<Sent> toBob = fake.take("relay", bob);
        CHECK(toBob.size() == 2);
        CHECK(toBob[0].id == "p-settings");
        CHECK(toBob[1].id == "p1");
        CHECK(toBob[1].author == "Ann");
        CHECK(fake.take("relay", cid).size() == 2);
        fake.accepted.clear();
        engine.onIncoming(incoming(ann, "contact.request"));
        CHECK(fake.accepted == std::vector<std::string>{ann});
        CHECK(fake.take("tell", ann).empty());
        client::IncomingMessage readded = incoming(ann, "contact.request");
        readded.e2eId = "a-new-request";
        engine.onIncoming(readded);
        CHECK(fake.take("tell", ann).empty());
        engine.onIncoming(readded);
        CHECK(fake.take("tell", ann).empty());

        engine.onIncoming(textFrom(bob, "again", "p1"));
        pump(engine);
        CHECK(fake.take("relay", ann).empty());

        engine.onIncoming(textFrom(bob, "huge", "p-huge"));
        CHECK(fake.take("tell", bob).back().text == "Too large to relay.");

        client::IncomingMessage carried = incoming(bob, client::kTypeFile);
        carried.e2eId = "p-file";
        carried.attachmentName = "notes.txt";
        fake.mediaBytes = Bytes{'t', 'x', 't'};
        engine.onIncoming(carried);
        pump(engine);
        CHECK(fake.take("relay", ann).size() == 1);
        CHECK(fake.take("relay", cid).size() == 1);
        client::IncomingMessage announced = carried;
        announced.e2eId = "p-announced";
        fake.mediaBytes.reset();
        engine.onIncoming(announced);
        pump(engine);
        CHECK(fake.take("tell", bob).back().text == "Too large to relay.");
        CHECK(fake.take("relay", ann).empty());
        CHECK(fake.take("relay", cid).empty());

        engine.hostPost("h1", "from the group");
        pump(engine);
        for (const std::string& member : {ann, bob, cid}) {
            const std::vector<Sent> hosted = fake.take("relay", member);
            CHECK(hosted.size() == 1);
            CHECK(hosted[0].id == "h1");
            CHECK(hosted[0].author.empty());
            CHECK(hosted[0].fromHost);
        }
        CHECK(!toBob[0].fromHost);
        for (const std::string& words : {std::string("huge"), std::string()}) {
            bool refused = false;
            try {
                engine.hostPost("h2", words);
            } catch (const std::invalid_argument&) {
                refused = true;
            }
            CHECK(refused);
        }
        pump(engine);
        CHECK(fake.take("relay", bob).empty());

        client::IncomingMessage edit = incoming(ann, "edit");
        edit.refId = "p1";
        edit.text = "first, edited";
        engine.onIncoming(edit);
        client::IncomingMessage foreignEdit = incoming(bob, "edit");
        foreignEdit.refId = "p1";
        foreignEdit.text = "not yours";
        engine.onIncoming(foreignEdit);
        pump(engine);
        const std::vector<Sent> edits = fake.take("edit", bob);
        CHECK(edits.size() == 1);
        CHECK(edits[0].text == "first, edited");
        CHECK(fake.take("edit", ann).empty());

        client::IncomingMessage reaction = incoming(bob, "reaction");
        reaction.refId = "p1";
        reaction.text = "+";
        engine.onIncoming(reaction);
        reaction.fromFingerprint = ann;
        engine.onIncoming(reaction);
        pump(engine);
        const std::vector<Sent> annCounts = fake.take("reactions", ann);
        CHECK(annCounts.size() == 1);
        CHECK(annCounts[0].reactions.size() == 1);
        CHECK(annCounts[0].reactions.at(0).at("counts").at("+") == 1);
        const std::vector<Sent> bobCounts = fake.take("reactions", bob);
        CHECK(bobCounts.size() == 1);
        CHECK(bobCounts[0].reactions.size() == 1);
        CHECK(bobCounts[0].reactions.at(0).at("counts").at("+") == 1);

        engine.onIncoming(textFrom(bob, "second", "p2"));
        pump(engine);
        fake.forget();
        GroupConfig config = engine.config();
        config.historySize = 1;
        engine.setConfig(config);
        join(engine, fake, eve, "Eve");
        pump(engine);
        const std::vector<Sent> history = fake.take("relay", eve);
        CHECK(history.size() == 1);
        CHECK(history[0].id == "p2");

        const std::string zed = member();
        config.historySize = 0;
        engine.setConfig(config);
        join(engine, fake, zed, "Zed");
        pump(engine);
        CHECK(fake.take("relay", zed).empty());

        CHECK(engine.blockFor(ann).members == 6);

        config.kind = client::GroupKind::eChannel;
        engine.setConfig(config);
        engine.refreshMemberCount();
        CHECK(fake.states.size() == 6);
        CHECK(!engine.blockFor(bob).write);
        engine.onIncoming(textFrom(bob, "closed", "p3"));
        CHECK(fake.take("tell", bob).back().text == "Only admins post here.");
        const std::string hal = member();
        join(engine, fake, hal, "Hal");
        CHECK(fake.take("tell", hal).empty());
        CHECK(engine.blockFor(ann).members == 6);
        engine.exclude(hal);
        engine.setAdmin(bob, true);
        CHECK(engine.blockFor(bob).admin);
        CHECK(engine.blockFor(bob).write);
        engine.onIncoming(textFrom(bob, "from an admin", "p4"));
        pump(engine);
        CHECK(fake.take("relay", ann).size() == 1);
        fake.forget();

        engine.onIncoming(commandFrom(bob, "delete", "p1"));
        pump(engine);
        CHECK(fake.take("delete", bob).size() == 1);
        CHECK(fake.take("delete", ann).empty());
        CHECK(fake.take("delete", cid).size() == 1);
        engine.onIncoming(commandFrom(ann, "delete", "p4"));
        pump(engine);
        CHECK(fake.take("delete", cid).empty());

        engine.onIncoming(commandFrom(bob, "contact", "p2"));
        CHECK(fake.take("tell", bob).empty());
        engine.onIncoming(commandFrom(ann, "contact", "p2"));
        const std::vector<Sent> asked = fake.take("tell", bob);
        CHECK(asked.size() == 1);
        CHECK(asked[0].text.find("Ann asks you to write.") == 0);
        CHECK(asked[0].text.find("https://bazari.sh/#invite?") != std::string::npos);
        CHECK(fake.take("tell", ann).back().text == "Your contact request was sent to Bob.");
        engine.onIncoming(commandFrom(ann, "contact", "p2"));
        CHECK(fake.take("tell", bob).empty());
        now += kContactAskEveryMs - 1;
        engine.onIncoming(commandFrom(ann, "contact", "p2"));
        CHECK(fake.take("tell", bob).empty());
        now += 1;
        engine.onIncoming(commandFrom(ann, "contact", "p2"));
        const std::vector<Sent> askedAgain = fake.take("tell", bob);
        CHECK(askedAgain.size() == 1);
        CHECK(askedAgain[0].text.find("Ann asks you to write.") == 0);

        config.kind = client::GroupKind::eGroup;
        engine.setConfig(config);
        engine.onIncoming(textFrom(cid, "by cid", "p5"));
        pump(engine);
        fake.forget();
        engine.onIncoming(commandFrom(cid, "contactable", "off"));
        fake.forget();
        engine.onIncoming(commandFrom(ann, "contact", "p5"));
        CHECK(fake.take("tell", cid).empty());
        const std::vector<Sent> notAccepting = fake.take("tell", ann);
        CHECK(notAccepting.size() == 1
            && notAccepting[0].text == "Ace does not accept contact requests.");
        const auto contactableOf = [&](const std::string& peer) {
            for (const Member& held : engine.members()) {
                if (held.fingerprint == peer) {
                    return held.contactable;
                }
            }
            return false;
        };
        CHECK(!contactableOf(cid));
        engine.setContactable(cid, true);
        CHECK(contactableOf(cid));
        engine.setContactable(cid, false);
        CHECK(!contactableOf(cid));
        CHECK_THROWS(engine.setContactable(Identity::generate().fingerprint(), false));
        fake.forget();
        engine.onIncoming(commandFrom(ann, "contact", "p5"));
        CHECK(fake.take("tell", cid).empty());
        const std::vector<Sent> refusedByHost = fake.take("tell", ann);
        CHECK(refusedByHost.size() == 1
            && refusedByHost[0].text == "Ace does not accept contact requests.");
        const auto stateOf = [&](const std::string& peer) {
            for (const Member& held : engine.members()) {
                if (held.fingerprint == peer) {
                    return held.state;
                }
            }
            return MemberState::eNaming;
        };
        engine.onIncoming(commandFrom(bob, "exclude", "p5"));
        CHECK(stateOf(cid) == MemberState::eLeft);
        CHECK(fake.forgotten.empty());
        CHECK(fake.take("tell", cid).back().text == "You were excluded from the group.");
        engine.onIncoming(textFrom(cid, "back in", "p5b"));
        CHECK(stateOf(cid) == MemberState::eMember);

        engine.onIncoming(commandFrom(bob, "block", "p5"));
        CHECK(stateOf(cid) == MemberState::eBanned);
        CHECK(fake.forgotten == std::vector<std::string>{cid});
        CHECK(fake.take("tell", cid).back().text == "You were blocked in the group.");
        fake.accepted.clear();
        engine.onIncoming(incoming(cid, "contact.request"));
        CHECK(fake.accepted.empty());

        config.joinMode = JoinMode::eApproval;
        engine.setConfig(config);
        const std::string fay = member();
        join(engine, fake, fay, "Fay");
        CHECK(fake.take("tell", fay).back().text == "Your request is waiting for an admin.");
        CHECK(!engine.blockFor(fay).write);
        const std::vector<Sent> card = fake.take("tell", bob);
        CHECK(card.size() == 1);
        CHECK(card[0].text.find("Fay wants to join.") == 0);
        CHECK(card[0].text.find("\n\n") == std::string::npos);
        CHECK(card[0].keyboard.at(0).size() == 2);
        CHECK(engine.requests().size() == 1);
        client::IncomingMessage approve = incoming(bob, "bot.callback");
        approve.callbackData = card[0].keyboard.at(0).at(0).data;
        engine.onIncoming(approve);
        const std::vector<Sent> settled = fake.take("retell", bob);
        CHECK(settled.size() == 1);
        CHECK(settled[0].text == "Fay: allowed by Bob.");
        CHECK(fake.take("tell", fay).empty());
        CHECK(engine.requests().empty());

        client::IncomingMessage ivyAsks = incoming(ivy, "contact.request");
        ivyAsks.text = kIvyIntro;
        engine.onIncoming(ivyAsks);
        CHECK(fake.take("tell", ivy).back().text == "Choose your name in this chat.");
        CHECK(fake.take("tell", bob).empty());

        fake.forget();
        fake.refuse[dan] = "DELIVERY_REJECTED";
        engine.onIncoming(textFrom(ann, "to the gone", "p6"));
        pump(engine);
        const auto state = [&](const std::string& peer) {
            for (const Member& held : engine.members()) {
                if (held.fingerprint == peer) {
                    return held.state;
                }
            }
            return MemberState::eNaming;
        };
        CHECK(state(dan) == MemberState::eLeft);
        CHECK(engine.blockFor(dan).write);
        fake.refuse.clear();
        engine.onIncoming(textFrom(dan, "back again", "p7"));
        CHECK(state(dan) == MemberState::ePending);

        engine.exclude(eve);
        CHECK(state(eve) == MemberState::eLeft);
        engine.onIncoming(incoming(eve, "receipt"));
        CHECK(state(eve) == MemberState::eLeft);
        engine.onIncoming(incoming(eve, "contact.request"));
        CHECK(state(eve) == MemberState::ePending);
        fake.refuse[eve] = "DELIVERY_REJECTED";
        engine.onIncoming(textFrom(eve, "anyone?", "p-eve"));
        CHECK(state(eve) == MemberState::eLeft);
        fake.refuse.clear();

        client::IncomingMessage rename = incoming(ann, "bot.callback");
        rename.callbackData = "rename";
        engine.onIncoming(rename);
        CHECK(fake.take("tell", ann).back().text == "Send your new name.");
        engine.onIncoming(commandFrom(ann, "settings", ""));
        CHECK(fake.take("tell", ann).back().text == "Your name in this group: Ann.");
        engine.onIncoming(textFrom(ann, "Anna", "p-anna"));
        CHECK(fake.take("tell", ann).back().text == "Your name is now Anna.");
        engine.onIncoming(textFrom(ann, "after", "p-after"));
        CHECK(fake.take("tell", ann).empty());
        engine.onIncoming(rename);
        engine.onIncoming(textFrom(ann, "Ann", "p-ann-again"));
        CHECK(fake.take("tell", ann).back().text == "Your name is now Ann.");
        engine.onIncoming(rename);
        CHECK(fake.take("tell", ann).size() == 1);
        engine.onIncoming(textFrom(ann, "Bob", "p-bob"));
        const std::vector<Sent> refused = fake.take("tell", ann);
        CHECK(refused.size() == 1);
        CHECK(refused[0].text == "That name is taken. Choose another.");
        engine.onIncoming(textFrom(ann, "Ann", "p-ann-third"));
        const std::vector<Sent> renamed = fake.take("tell", ann);
        CHECK(renamed.size() == 1);
        CHECK(renamed[0].text == "Your name is now Ann.");
    }

    {
        GroupEngine engine(dir / "group.db", linkFor(fake), [&now]() { return now; });
        CHECK(engine.config().joinMode == JoinMode::eApproval);
        CHECK(engine.blockFor(ann).members == 4);

        fake.forget();
        engine.onIncoming(textFrom(ivy, "Ivy", "p-ivy"));
        const auto ivyRequest = [&engine, &ivy]() {
            for (const JoinRequest& asked : engine.requests()) {
                if (asked.fingerprint == ivy) {
                    return std::optional<JoinRequest>(asked);
                }
            }
            return std::optional<JoinRequest>();
        };
        const std::vector<Sent> ivyCard = fake.take("tell", bob);
        CHECK(ivyCard.size() == 1);
        CHECK(ivyCard[0].text.find("Ivy wants to join.") == 0);
        CHECK(ivyCard[0].text.size() > kIvyIntro.size());
        CHECK(ivyCard[0].text.compare(ivyCard[0].text.size() - kIvyIntro.size(), kIvyIntro.size(),
                  kIvyIntro) == 0);
        CHECK(ivyRequest() && ivyRequest()->intro == kIvyIntro);
        engine.decide(ivyRequest()->number, false);
        CHECK(!ivyRequest());
        fake.forget();
        engine.onIncoming(textFrom(ivy, "again", "p-ivy-again"));
        CHECK(ivyRequest() && ivyRequest()->intro.empty());
        const std::vector<Sent> bareCard = fake.take("tell", bob);
        CHECK(bareCard.size() == 1);
        CHECK(bareCard[0].text.find(kIvyIntro) == std::string::npos);
        engine.decide(ivyRequest()->number, false);

        GroupConfig open = engine.config();
        open.joinMode = JoinMode::eOpen;
        engine.setConfig(open);
        const std::string gus = member();
        join(engine, fake, gus, "Аce");
        CHECK(fake.take("tell", gus).back().text == "The name Аce is not allowed. Choose another.");
        engine.onIncoming(textFrom(gus, "Gus"));
        CHECK(fake.take("tell", gus).empty());
        CHECK(engine.blockFor(gus).write);
        for (const Member& held : engine.members()) {
            if (held.fingerprint == ann) {
                CHECK(held.name == "Ann");
            }
        }
    }

    {
        Fake batched;
        GroupEngine engine(dir / "batched.db", linkFor(batched), [&now]() { return now; });
        const std::string host = member();
        join(engine, batched, host, "Hal");
        for (const char* const id : {"b1", "b2", "b3", "b4"}) {
            engine.onIncoming(textFrom(host, id, id));
        }
        client::IncomingMessage edit = incoming(host, "edit");
        edit.refId = "b2";
        edit.text = "b2, edited";
        engine.onIncoming(edit);
        client::IncomingMessage liked = incoming(host, "reaction");
        liked.refId = "b3";
        liked.text = "+";
        engine.onIncoming(liked);
        engine.onIncoming(textFrom(host, "b5", "b5"));
        pump(engine);
        GroupConfig config = engine.config();
        config.historySize = 5;
        engine.setConfig(config);

        const std::string uma = member();
        join(engine, batched, uma, "Uma");
        pump(engine);
        const std::vector<Sent> history = batched.take("relay", uma);
        CHECK(history.size() == 5);
        for (std::size_t at = 0; at < history.size(); ++at) {
            CHECK(history[at].id == "b" + std::to_string(at + 1));
        }
        CHECK(history[1].text == "b2, edited");
        CHECK(batched.relayCalls[uma] == 1);
        CHECK(batched.take("edit", uma).empty());
        const std::vector<Sent> umaCounts = batched.take("reactions", uma);
        CHECK(umaCounts.size() == 1);
        CHECK(umaCounts[0].reactions.at(0).at("ref") == "b3");

        batched.batchCap = 2;
        const std::string vic = member();
        join(engine, batched, vic, "Vic");
        pump(engine);
        CHECK(batched.take("relay", vic).size() == 5);
        CHECK(batched.relayCalls[vic] == 4);
    }

    {
        Fake dead;
        GroupEngine engine(dir / "dead.db", linkFor(dead), [&now]() { return now; });
        const auto stateIn = [&engine](const std::string& peer) {
            for (const Member& held : engine.members()) {
                if (held.fingerprint == peer) {
                    return held.state;
                }
            }
            return MemberState::eNaming;
        };
        const std::string host = member();
        join(engine, dead, host, "Hal");
        const std::string gus = member();
        join(engine, dead, gus, "Gus");
        const std::string ida = member();
        join(engine, dead, ida, "Ida");
        dead.away = {gus, ida};
        engine.onIncoming(textFrom(host, "first", "d1"));
        pump(engine);
        CHECK(dead.parked.count(gus) == 1 && dead.parked.count(ida) == 1);

        now += kDeadAfterMs / 2;
        dead.away.erase(ida);
        dead.parked.at(ida).onOutcome(dead.outcomeFor(ida));
        dead.parked.erase(ida);
        dead.away.insert(ida);
        engine.onIncoming(textFrom(host, "second", "d2"));
        pump(engine);
        CHECK(dead.parked.count(ida) == 1);
        CHECK(stateIn(gus) == MemberState::eMember);

        now += kDeadAfterMs / 2 + 1;
        dead.parked.at(gus).onPhase(client::kPhaseWaiting);
        dead.parked.at(ida).onPhase(client::kPhaseWaiting);
        CHECK(stateIn(gus) == MemberState::eLeft);
        CHECK(stateIn(ida) == MemberState::eMember);
        CHECK(dead.withdrawn == std::vector<std::string>{gus});
        dead.parked.at(gus).onOutcome(
            client::OutboundCourier::Outcome{false, {}, client::kWithdrawnMessage});
        dead.parked.erase(gus);
        CHECK(stateIn(gus) == MemberState::eLeft);

        dead.away.erase(gus);
        dead.forget();
        engine.onIncoming(textFrom(gus, "back", "d3"));
        pump(engine);
        CHECK(stateIn(gus) == MemberState::eMember);
        CHECK(!dead.take("relay", gus).empty());
    }

    {
        Fake kept;
        GroupEngine engine(dir / "kept.db", linkFor(kept), [&now]() { return now; });
        const std::string host = member();
        join(engine, kept, host, "Hal");
        const std::string late = member();
        join(engine, kept, late, "Lea");
        GroupConfig narrow = engine.config();
        narrow.historySize = 2;
        engine.setConfig(narrow);
        const auto shot = [&host](const std::string& id) {
            client::IncomingMessage picture = incoming(host, client::kTypeImage);
            picture.e2eId = id;
            picture.attachmentName = id + ".jpg";
            picture.attachmentMime = "image/jpeg";
            return picture;
        };
        const auto postIds = [&engine]() {
            std::vector<std::string> ids;
            for (const Post& post : engine.posts(2 * kLeastPostsKept)) {
                ids.push_back(post.id);
            }
            return ids;
        };
        const auto mediaOf = [&engine](const std::string& id) {
            for (const Post& post : engine.posts(2 * kLeastPostsKept)) {
                if (post.id == id) {
                    return post.media;
                }
            }
            return Bytes();
        };
        kept.mediaBytes = Bytes{'j', 'p', 'g'};
        kept.away = {late};
        engine.onIncoming(shot("p0"));
        for (int number = 1; number <= 10; ++number) {
            const std::string id = "p" + std::to_string(number);
            engine.onIncoming(textFrom(host, id, id));
        }
        engine.onIncoming(shot("p11"));
        pump(engine);
        CHECK(kept.parked.count(late) == 1);

        engine.setKeepsHistory(false);
        engine.dropSpent();
        CHECK(postIds().size() == 12);
        CHECK(!mediaOf("p0").empty());

        kept.away.erase(late);
        kept.parked.at(late).onOutcome(kept.outcomeFor(late));
        kept.parked.erase(late);
        pump(engine);
        engine.dropSpent();
        CHECK(postIds().size() == static_cast<std::size_t>(kLeastPostsKept));
        CHECK(postIds().front() == "p11" && postIds().back() == "p2");
        CHECK(!mediaOf("p11").empty());
        CHECK(mediaOf("p9").empty());

        GroupConfig none = engine.config();
        none.historySize = 0;
        engine.setConfig(none);
        engine.onIncoming(shot("p12"));
        pump(engine);
        engine.dropSpent();
        CHECK(postIds().size() == static_cast<std::size_t>(kLeastPostsKept));
        CHECK(postIds().front() == "p12" && postIds().back() == "p3");
        CHECK(mediaOf("p12").empty());

        const std::string fresh = member();
        join(engine, kept, fresh, "Fay");
        kept.forget();
        pump(engine);
        CHECK(kept.take("relay", fresh).empty());

        engine.setKeepsHistory(true);
        for (const char* const id : {"p13", "p14"}) {
            engine.onIncoming(textFrom(host, id, id));
        }
        pump(engine);
        engine.dropSpent();
        CHECK(postIds().size() == static_cast<std::size_t>(kLeastPostsKept) + 2);
    }

    {
        Fake crowd;
        GroupEngine engine(dir / "crowd.db", linkFor(crowd), [&now]() { return now; });
        const std::string host = member();
        join(engine, crowd, host, "Hub");
        for (std::size_t i = 0; i <= kMaxRelaysInFlight; ++i) {
            const std::string gone = member();
            join(engine, crowd, gone, "Away " + std::to_string(i));
            crowd.away.insert(gone);
        }
        const std::string present = member();
        join(engine, crowd, present, "Here");
        crowd.forget();
        engine.onIncoming(textFrom(host, "to everyone", "c1"));
        pump(engine);
        CHECK(crowd.parked.size() == kMaxRelaysInFlight + 1);
        CHECK(crowd.take("relay", present).size() == 1);
    }

    {
        Fake refused;
        GroupEngine engine(dir / "refused.db", linkFor(refused), [&now]() { return now; });
        const std::string host = member();
        join(engine, refused, host, "Hoy");
        const std::string full = member();
        join(engine, refused, full, "Jam");
        const std::string large = member();
        join(engine, refused, large, "Kit");
        const std::string local = member();
        join(engine, refused, local, "Lou");
        refused.refuse[full] = "STORAGE_FULL";
        refused.refuse[large] = "MESSAGE_TOO_LARGE";
        refused.broken.insert(local);
        refused.forget();
        refused.relayCalls.clear();
        engine.onIncoming(textFrom(host, "first", "r1"));
        pump(engine);
        CHECK(refused.relayCalls[full] == 1);
        CHECK(refused.relayCalls[large] == 1);
        CHECK(refused.relayCalls[local] == 1);

        now += kOldRetryMs;
        pump(engine);
        CHECK(refused.relayCalls[full] == 1);
        CHECK(refused.relayCalls[large] == 1);
        CHECK(refused.relayCalls[local] == 1);

        refused.refuse.erase(large);
        refused.forget();
        engine.onIncoming(textFrom(host, "second", "r2"));
        pump(engine);
        const std::vector<Sent> after = refused.take("relay", large);
        CHECK(after.size() == 1);
        CHECK(after[0].id == "r2");
        CHECK(refused.relayCalls[full] == 1);
        CHECK(refused.relayCalls[local] == 1);

        refused.refuse.erase(full);
        refused.broken.erase(local);
        refused.forget();
        engine.onIncoming(incoming(full, "receipt"));
        engine.onIncoming(incoming(local, "receipt"));
        pump(engine);
        for (const std::string& heard : {full, local}) {
            const std::vector<Sent> caught = refused.take("relay", heard);
            CHECK(caught.size() == 2);
            CHECK(caught[0].id == "r1");
            CHECK(caught[1].id == "r2");
        }
    }

    {
        Fake started;
        std::mutex guard;
        std::condition_variable relayed;
        std::atomic<int> clockReads{0};
        GroupLink link = linkFor(started);
        const auto locked = [&guard](auto call) {
            return [&guard, call](auto&&... args) {
                const std::lock_guard<std::mutex> lock(guard);
                return call(std::forward<decltype(args)>(args)...);
            };
        };
        link.accept = locked(link.accept);
        link.tell = locked(link.tell);
        link.retell = locked(link.retell);
        link.state = locked(link.state);
        link.receipt = locked(link.receipt);
        link.name = locked(link.name);
        link.relay = [&guard, &relayed, relay = link.relay](const std::string& peer,
                         const std::vector<client::RelayedPost>& posts,
                         const client::DeliveryWatch& watch) {
            {
                const std::lock_guard<std::mutex> lock(guard);
                relay(peer, posts, watch);
            }
            relayed.notify_all();
        };
        GroupEngine engine(dir / "started.db", std::move(link), [&clockReads, &now]() {
            ++clockReads;
            return now;
        });
        const std::string ray = member();
        join(engine, started, ray, "Ray");
        engine.start();
        engine.hostPost("h1", "posted while the engine runs");
        {
            std::unique_lock<std::mutex> lock(guard);
            CHECK(relayed.wait_for(lock, kRelayWait, [&started, &ray]() {
                return std::any_of(
                    started.sent.begin(), started.sent.end(), [&ray](const Sent& sent) {
                        return sent.kind == "relay" && sent.peer == ray && sent.id == "h1";
                    });
            }));
        }
        std::this_thread::sleep_for(kSettle);
        const int settled = clockReads.load();
        std::this_thread::sleep_for(kIdleWatch);
        CHECK(clockReads.load() == settled);
        engine.stop();
    }

    checkMemberList(dir);
    checkListInParts(dir);
    std::printf("TestGroupEngine passed\n");
    return 0;
} catch (...) {
    return testutil::failed();
}
