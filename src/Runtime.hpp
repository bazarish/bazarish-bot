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

class Runtime {
public:
    explicit Runtime(Config config);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    const Config& config() const { return config_; }
    Settings& settings() { return settings_; }

    bool hasAccount() const;
    bool running() const;
    std::string status() const;

    void createAccount(const std::string& name, const std::string& passphrase);
    void importAccount(const Bytes& bundle, const std::string& bundlePassphrase,
        const std::string& passphrase);
    Bytes exportAccount(const std::string& bundlePassphrase);

    void rename(const std::string& name);
    void newAddress();

    void start(const std::string& passphrase);
    void stop();

    std::string destination() const;
    std::string fingerprint() const;
    std::string displayName() const;
    std::string inviteUri() const;

    struct Peer {
        std::string fingerprint;
        std::string name;
        bool pending = false;
        bool writable = false;
    };
    std::vector<Peer> contacts() const;
    std::vector<Line> conversation(const std::string& peer, std::int64_t before, int limit) const;
    std::vector<Line> chats() const;

    struct Sent {
        std::string messageId;
        bool confirmed = false;
    };

    Sent sendText(const std::string& peer, const std::string& text);
    Sent sendKeyboard(
        const std::string& peer, const std::string& text, const std::string& keyboardJson);
    void editMessage(
        const std::string& peer, const std::string& messageId, const std::string& text);
    void deleteMessage(const std::string& peer, const std::string& messageId);
    void react(
        const std::string& peer, const std::string& messageId, const std::string& emoji);
    void confirmReceipt(const std::string& peer, const std::string& messageId);

    void clearChat(const std::string& peer);

    void acceptContact(const std::string& peer);
    std::string addByInvite(const std::string& inviteUri, const std::string& text);

    std::vector<Event> await(std::int64_t sinceId, int seconds) const;
    std::int64_t lastEventId() const;

private:
    void poll();
    void bringUpAsync(const std::string& passphrase);
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
    std::atomic<bool> cardCarriesDestination_{false};
    std::atomic<bool> aliasesAsked_{false};
    mutable std::mutex waitMutex_;
    mutable std::condition_variable waiters_;
};

}  // namespace bazarish::daemon
