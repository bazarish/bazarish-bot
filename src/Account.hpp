// Bazarish project (c) 2026
#pragma once

#include "GroupEngine.hpp"
#include "ServerSide.hpp"
#include "Settings.hpp"
#include "Transcript.hpp"

#include <LoginSigner.hpp>
#include <Session.hpp>

#include <bazarish/Bytes.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace bazarish::bot {

class Account {
public:
    using ServerSideFn = std::function<ServerSide&()>;

    Account(std::string id, std::filesystem::path accountFile, std::filesystem::path transcriptFile,
        std::filesystem::path groupFile, Settings& settings, ServerSideFn serverSide);
    ~Account();

    Account(const Account&) = delete;
    Account& operator=(const Account&) = delete;

    const std::string& id() const { return id_; }
    bool running() const;
    std::string status() const;

    void open();
    void close();
    void closeOnServer();

    Settings::AccountKind kind() const;
    GroupEngine& group() const;

    Bytes exportBundle(const std::string& bundlePassphrase);

    struct Pairing {
        bool active = false;
        std::string uri;
        std::string code;
        std::string stage;
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        int wrongCodes = 0;
        std::string error;
    };
    Pairing startPairing();
    void stopPairing();
    Pairing pairing() const;

    void rename(const std::string& name);
    void newAddress();

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
    std::string addByAlias(const std::string& alias, const std::string& text);

    std::string signLogin(const std::string& challenge) const;

    struct Alias {
        std::string alias;
        std::int64_t notAfter = 0;
        bool autoRenew = true;
        bool resolving = false;
        bool live = false;
    };
    struct AliasCheck {
        bool ok = false;
        std::string note;
    };
    struct Aliases {
        std::vector<Alias> held;
        bool depositCovers = true;
        bool checking = false;
        std::optional<AliasCheck> checked;
    };
    Aliases aliases() const;
    void checkAliases();
    void setAliasBinding(const std::string& alias, bool on);

    struct PendingRouting {
        std::vector<std::string> contacts;
        bool registry = false;
    };
    PendingRouting pendingRouting() const;
    void retryRouting(const std::string& peer);

    std::vector<Event> await(std::int64_t sinceId, int seconds) const;
    std::int64_t lastEventId() const;

private:
    void maintain();
    void waitForEvents(const client::Session::ContactFetchContext& context);
    void drainMailbox();
    void bringUp();
    client::Session& openSession() const;
    void applyKind(client::Session& session) const;
    GroupLink groupLink();
    void noteAliases(const client::Session& session);
    void startAliasErrand(client::Session::AliasErrandContext context, bool byHand);
    void runAliasErrand(const client::Session::AliasErrandContext& context);

    const std::string id_;
    const std::filesystem::path accountFile_;
    const std::filesystem::path transcriptFile_;
    const std::filesystem::path groupFile_;
    Settings& settings_;
    const ServerSideFn serverSide_;

    mutable std::mutex sessionMutex_;
    ServerSide* mailbox_ = nullptr;
    std::unique_ptr<client::Session> session_;
    std::unique_ptr<Transcript> transcript_;
    std::unique_ptr<GroupEngine> group_;

    mutable std::mutex stateMutex_;
    std::string fingerprint_;
    std::string name_;
    std::string status_;
    std::shared_ptr<client::LoginSigner> signer_;
    Aliases aliases_;

    struct PairingSlot {
        std::mutex mutex;
        Pairing state;
    };
    const std::shared_ptr<PairingSlot> pairing_ = std::make_shared<PairingSlot>();

    std::thread starter_;
    std::thread maintainer_;
    std::thread eventWaiter_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> cardCarriesDestination_{false};
    std::atomic<bool> announceRoutingWhenCardIsOut_{false};
    std::int64_t lastAliasErrand_ = 0;
    std::mutex errandMutex_;
    std::thread aliasErrand_;
    bool aliasErrandRunning_ = false;
    bool aliasErrandByHand_ = false;
    std::atomic<bool> aliasesAnswered_{false};
    mutable std::mutex waitMutex_;
    mutable std::condition_variable waiters_;
};

}  // namespace bazarish::bot
