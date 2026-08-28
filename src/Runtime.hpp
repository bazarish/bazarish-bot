// Bazarish project (c) 2026
#pragma once

#include "Config.hpp"
#include "ServerSide.hpp"
#include "Settings.hpp"
#include "Transcript.hpp"

#include <Session.hpp>

#include <bazarish/Bytes.hpp>

#include <bazarish/I2p.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bazarish::daemon {

// One account, running as a service, with the mailbox that serves it in the same
// process. What the interface and the API both talk to.
//
// The account is deliberately single-device: this daemon is its own server, so
// there is nowhere for a second device to synchronise from, and pretending
// otherwise would hand the operator a half-working feature.
class Runtime {
public:
    explicit Runtime(Config config);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    const Config& config() const { return config_; }
    Settings& settings() { return settings_; }

    // Whether an account has been created or imported yet.
    bool hasAccount() const;
    // Whether the account is open and the transport is up.
    bool running() const;
    // What the daemon is doing, for the interface to say: "no account", "closed",
    // "starting", "running", or the failure it stopped at.
    std::string status() const;

    // Creates a fresh account and starts bringing it up. Throws when one already
    // exists; the bring-up itself happens on a thread of its own, because a
    // router that is finding the network takes minutes and the interface has to
    // stay answering while it does.
    void createAccount(const std::string& name, const std::string& passphrase);
    // Takes an exported bundle as this daemon's account and brings it up. The
    // bundle arrives as bytes: an operator with a browser has a file, not a path
    // on the machine the daemon runs on.
    void importAccount(const Bytes& bundle, const std::string& bundlePassphrase,
        const std::string& passphrase);
    // The account as a bundle, which is how it is moved to another daemon - not
    // how it is shared with a second device, because there is no second device.
    Bytes exportAccount(const std::string& bundlePassphrase);

    // The name this account calls itself. Local: a contact keeps their own name
    // for us, and this is not sent to them.
    void rename(const std::string& name);
    // Rotates the key this account's own server seals with and the capability
    // that reads its card, and tells the contacts. The address does not change.
    void rotateServingKey();
    // Mints a fresh destination key: a new address, published in a new card. The
    // old one stops answering, so a contact who has not seen the new card has to
    // be written to first.
    void newAddress();

    // Opens the account and brings the transport, the mailbox and the session up.
    // Throws when the passphrase is wrong or nothing is there to open.
    void start(const std::string& passphrase);
    void stop();

    // The address this daemon is reachable at, empty while it is still building.
    std::string destination() const;
    // This account's own fingerprint and the name it was created under.
    std::string fingerprint() const;
    std::string displayName() const;
    // The invite an operator hands out to be added as a contact.
    std::string inviteUri() const;

    // A contact as the two faces show it.
    struct Peer {
        std::string fingerprint;
        std::string name;
        bool pending = false;
    };
    std::vector<Peer> contacts() const;
    std::vector<Line> conversation(const std::string& peer, std::int64_t before, int limit) const;
    std::vector<Line> chats() const;

    // What a send comes back with: the id it was sent under, and whether the
    // far side confirmed storage before the wait was up. Unconfirmed is not
    // failed - the mailbox took it and keeps trying.
    struct Sent {
        std::string messageId;
        bool confirmed = false;
    };

    // Sends and records in one step: what the transcript holds is what was said,
    // whichever face said it.
    Sent sendText(const std::string& peer, const std::string& text);
    Sent sendKeyboard(
        const std::string& peer, const std::string& text, const std::string& keyboardJson);
    // Revises a message this account sent, in place on both sides.
    void editMessage(
        const std::string& peer, const std::string& messageId, const std::string& text);
    // Takes back a message this account sent, on both sides.
    void deleteMessage(const std::string& peer, const std::string& messageId);
    // Puts a reaction on a message, or removes this account's own with an empty
    // emoji. One reaction per message per reactor.
    void react(
        const std::string& peer, const std::string& messageId, const std::string& emoji);
    // Tells the peer their message arrived, which is what turns their bubble
    // from delivered into read.
    void confirmReceipt(const std::string& peer, const std::string& messageId);

    void acceptContact(const std::string& peer);
    std::string addByInvite(const std::string& inviteUri, const std::string& text);

    // Everything that happened after `sinceId`, waiting up to `seconds` when
    // nothing has yet. This is what a bot polls.
    std::vector<Event> await(std::int64_t sinceId, int seconds) const;
    std::int64_t lastEventId() const;

private:
    // Pulls the mailbox, writes what arrives down, and wakes whoever is waiting.
    void poll();
    // The bring-up, on its own thread: a router that is finding the network takes
    // minutes, and the interface has to stay answering while it does.
    void bringUpAsync(const std::string& passphrase);
    // The session, held under the lock the poller and the two faces share.
    client::Session& openSession() const;
    std::filesystem::path accountFile() const;
    void bringUp(const std::string& passphrase);

    Config config_;
    Settings settings_;

    mutable std::mutex sessionMutex_;
    std::unique_ptr<ServerSide> mailbox_;
    std::unique_ptr<client::Session> session_;
    std::unique_ptr<Transcript> transcript_;
    std::string status_;

    std::thread starter_;
    std::thread poller_;
    std::atomic<bool> stopping_{false};
    // Whether the card a contact fetches names the destination yet.
    std::atomic<bool> cardCarriesDestination_{false};
    mutable std::mutex waitMutex_;
    mutable std::condition_variable waiters_;
};

}  // namespace bazarish::daemon
