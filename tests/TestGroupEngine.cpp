// Bazarish project (c) 2026
#include "GroupEngine.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Crypto.hpp>
#include <bazarish/Descriptor.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

namespace fs = std::filesystem;
using namespace bazarish;
using namespace bazarish::bot;

namespace {

constexpr const char* kDest = "dlkbeyqjykssca6o7qlbwgq4fr2hry7kw2ursn2sh3lt3acox6gq.b32.i2p";
constexpr int kPumpRounds = 8;
constexpr const char* kHint = "Bring this up again with !!/settings!!.";

struct Sent {
    std::string kind;
    std::string peer;
    std::string id;
    std::string text;
    std::string author;
    client::InlineKeyboard keyboard;
    nlohmann::json reactions;
};

struct Fake {
    std::vector<Sent> sent;
    std::vector<std::string> accepted;
    std::vector<std::string> forgotten;
    std::vector<std::string> states;
    std::map<std::string, std::string> refuse;
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
        fake.sent.push_back({"tell", peer, id, text, {}, keyboard, {}});
        done(fake.outcomeFor(peer));
        return id;
    };
    link.retell = [&fake](const std::string& peer, const std::string& id, const std::string& text,
                      const client::InlineKeyboard& keyboard) {
        fake.sent.push_back({"retell", peer, id, text, {}, keyboard, {}});
    };
    link.relay = [&fake](const std::string& peer, const client::RelayedPost& post,
                     const GroupLink::Outcome& done) {
        fake.sent.push_back({"relay", peer, post.id, post.text, post.author, {}, {}});
        done(fake.outcomeFor(peer));
    };
    link.edit = [&fake](const std::string& peer, const std::string& ref, const std::string& text,
                    const GroupLink::Outcome& done) {
        fake.sent.push_back({"edit", peer, ref, text, {}, {}, {}});
        done(fake.outcomeFor(peer));
    };
    link.remove = [&fake](const std::string& peer, const std::string& ref,
                      const GroupLink::Outcome& done) {
        fake.sent.push_back({"delete", peer, ref, {}, {}, {}, {}});
        done(fake.outcomeFor(peer));
    };
    link.reactions = [&fake](const std::string& peer, const nlohmann::json& reactions,
                         const GroupLink::Outcome& done) {
        fake.sent.push_back({"reactions", peer, {}, {}, {}, {}, reactions});
        done(fake.outcomeFor(peer));
    };
    link.state = [&fake](const std::string& peer, const GroupLink::Outcome& done) {
        fake.states.push_back(peer);
        done(fake.outcomeFor(peer));
    };
    link.receipt = [&fake](const std::string& peer, const std::string& id) {
        fake.sent.push_back({"receipt", peer, id, {}, {}, {}, {}});
    };
    link.descriptor = [](const std::string& peer) {
        return encodeDescriptor(Descriptor{peer, kDest, "theirs"});
    };
    link.forget = [&fake](const std::string& peer) { fake.forgotten.push_back(peer); };
    link.fits = [](const client::RelayedPost& post) { return post.text != "huge"; };
    link.media = [](const std::string&, const std::string&) { return std::optional<Bytes>(); };
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

void join(GroupEngine& engine, const std::string& peer, const std::string& name)
{
    engine.onIncoming(incoming(peer, "contact.request"));
    engine.onIncoming(textFrom(peer, name));
}

std::string member()
{
    return Identity::generate().fingerprint();
}

}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / ("bz-group-" + toHex(randomBytes(8)));
    std::int64_t now = 1000;
    Fake fake;
    const std::string ann = Identity::generate().fingerprint();
    const std::string bob = Identity::generate().fingerprint();
    const std::string cid = Identity::generate().fingerprint();
    const std::string dan = Identity::generate().fingerprint();
    const std::string eve = Identity::generate().fingerprint();

    {
        GroupEngine engine(dir / "group.db", {}, linkFor(fake), [&now]() { return now; });

        engine.onIncoming(incoming(ann, "contact.request"));
        CHECK(fake.accepted == std::vector<std::string>{ann});
        CHECK(fake.take("tell", ann).size() == 1);
        engine.onIncoming(incoming(ann, "receipt"));
        CHECK(fake.take("tell", ann).empty());
        CHECK(engine.blockFor(ann).write);
        engine.onIncoming(textFrom(ann, "Ann"));
        const std::vector<Sent> welcome = fake.take("tell", ann);
        CHECK(welcome.size() == 1);
        CHECK(welcome[0].text == std::string("Welcome, Ann.\n\n") + kHint);
        CHECK(welcome[0].keyboard.size() == 1);
        CHECK(welcome[0].keyboard.at(0).at(0).data == "settings");

        const auto press = [&](const std::string& data) {
            client::IncomingMessage callback = incoming(ann, "bot.callback");
            callback.callbackData = data;
            callback.refId = welcome[0].id;
            engine.onIncoming(callback);
            const std::vector<Sent> retold = fake.take("retell", ann);
            CHECK(retold.size() == 1);
            CHECK(retold[0].id == welcome[0].id);
            CHECK(retold[0].text == welcome[0].text);
            CHECK(retold[0].keyboard.size() == 2);
            CHECK(retold[0].keyboard.at(1).at(0).data == "rename");
            return retold[0].keyboard.at(0).at(0).data;
        };
        CHECK(press("settings") == "contactable off");
        CHECK(press("contactable off") == "contactable on");
        CHECK(press("contactable on") == "contactable off");

        engine.onIncoming(textFrom(ann, " /settings\n", "p-settings"));
        const std::vector<Sent> offered = fake.take("tell", ann);
        CHECK(offered.size() == 1);
        CHECK(offered[0].text == kHint);
        CHECK(offered[0].keyboard.at(0).at(0).data == "settings");

        join(engine, bob, "ann");
        CHECK(fake.take("tell", bob).back().text == "That name is taken. Choose another.");
        engine.onIncoming(textFrom(bob, "Bob"));
        join(engine, cid, "Ace");
        join(engine, dan, "Асе");
        CHECK(fake.take("tell", dan).back().text == "That name is taken. Choose another.");
        engine.onIncoming(textFrom(dan, "Dan"));
        fake.forget();

        engine.onIncoming(textFrom(ann, "first", "p1"));
        const std::vector<Sent> receipts = fake.take("receipt", ann);
        CHECK(receipts.size() == 1);
        CHECK(receipts[0].id == "p1");
        pump(engine);
        CHECK(fake.take("relay", ann).empty());
        const std::vector<Sent> toBob = fake.take("relay", bob);
        CHECK(toBob.size() == 1);
        CHECK(toBob[0].id == "p1");
        CHECK(toBob[0].author == "Ann");
        CHECK(fake.take("relay", cid).size() == 1);

        engine.onIncoming(textFrom(bob, "again", "p1"));
        pump(engine);
        CHECK(fake.take("relay", ann).empty());

        engine.onIncoming(textFrom(bob, "huge", "p-huge"));
        CHECK(fake.take("tell", bob).back().text == "Too large to relay.");

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
        engine.flushReactions();
        pump(engine);
        const std::vector<Sent> annCounts = fake.take("reactions", ann);
        CHECK(annCounts.size() == 1);
        CHECK(annCounts[0].reactions.at(0).at("counts").at("+") == 1);
        const std::vector<Sent> bobCounts = fake.take("reactions", bob);
        CHECK(bobCounts.size() == 1);
        CHECK(bobCounts[0].reactions.at(0).at("counts").empty());

        engine.onIncoming(textFrom(bob, "second", "p2"));
        pump(engine);
        fake.forget();
        GroupConfig config = engine.config();
        config.historySize = 1;
        engine.setConfig(config);
        join(engine, eve, "Eve");
        pump(engine);
        const std::vector<Sent> history = fake.take("relay", eve);
        CHECK(history.size() == 1);
        CHECK(history[0].id == "p2");

        const std::string zed = member();
        config.historySize = 0;
        engine.setConfig(config);
        join(engine, zed, "Zed");
        pump(engine);
        CHECK(fake.take("relay", zed).empty());

        engine.refreshMemberCount();
        CHECK(engine.blockFor(ann).members == 6);

        config.kind = client::GroupKind::eChannel;
        engine.setConfig(config);
        CHECK(fake.states.size() == 6);
        CHECK(!engine.blockFor(bob).write);
        engine.onIncoming(textFrom(bob, "closed", "p3"));
        CHECK(fake.take("tell", bob).back().text == "Only admins post here.");
        const std::string hal = member();
        join(engine, hal, "Hal");
        CHECK(fake.take("tell", hal).back().text == std::string("Welcome, Hal.\n\n") + kHint);
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
        CHECK(asked[0].text.find("bazarish://") != std::string::npos);
        CHECK(fake.take("tell", ann).back().text == "Sent.");
        engine.onIncoming(commandFrom(ann, "contact", "p2"));
        CHECK(fake.take("tell", bob).empty());

        config.kind = client::GroupKind::eGroup;
        engine.setConfig(config);
        engine.onIncoming(textFrom(cid, "by cid", "p5"));
        pump(engine);
        fake.forget();
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
        join(engine, fay, "Fay");
        CHECK(fake.take("tell", fay).back().text == "Your request is waiting for an admin.");
        CHECK(!engine.blockFor(fay).write);
        const std::vector<Sent> card = fake.take("tell", bob);
        CHECK(card.size() == 1);
        CHECK(card[0].text.find("Fay wants to join.") == 0);
        CHECK(card[0].keyboard.at(0).size() == 2);
        CHECK(engine.requests().size() == 1);
        client::IncomingMessage approve = incoming(bob, "bot.callback");
        approve.callbackData = card[0].keyboard.at(0).at(0).data;
        engine.onIncoming(approve);
        const std::vector<Sent> settled = fake.take("retell", bob);
        CHECK(settled.size() == 1);
        CHECK(settled[0].text == "Fay: allowed by Bob.");
        CHECK(fake.take("tell", fay).back().text == std::string("Welcome, Fay.\n\n") + kHint);
        CHECK(engine.requests().empty());

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
        engine.onIncoming(textFrom(ann, "/settings", "p-settings-2"));
        CHECK(fake.take("tell", ann).back().text == kHint);
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
        GroupEngine engine(dir / "group.db", {}, linkFor(fake), [&now]() { return now; });
        CHECK(engine.config().joinMode == JoinMode::eApproval);
        CHECK(engine.memberCount() == 4);

        GroupConfig custom = engine.config();
        custom.joinMode = JoinMode::eOpen;
        custom.greeting = "Pick a name.";
        custom.welcome = "Hi, {name}! {name} is in.";
        engine.setConfig(custom);
        const std::string gus = member();
        engine.onIncoming(incoming(gus, "contact.request"));
        CHECK(fake.take("tell", gus).back().text == "Pick a name.");
        engine.onIncoming(textFrom(gus, "Gus"));
        CHECK(fake.take("tell", gus).back().text
            == std::string("Hi, Gus! Gus is in.\n\n") + kHint);
        custom.welcome = std::string(2048, 'w');
        bool refused = false;
        try {
            engine.setConfig(custom);
        } catch (const std::exception&) {
            refused = true;
        }
        CHECK(refused);
        for (const Member& held : engine.members()) {
            if (held.fingerprint == ann) {
                CHECK(held.name == "Ann");
            }
        }
    }

    {
        GroupEngine engine(dir / "group.db", {}, linkFor(fake), [&now]() { return now; });
        CHECK(engine.config().greeting == "Pick a name.");
        CHECK(engine.config().welcome == "Hi, {name}! {name} is in.");
    }

    fs::remove_all(dir);
    std::printf("TestGroupEngine passed\n");
    return 0;
}
