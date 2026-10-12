// Bazarish project (c) 2026
#pragma once

#include "Account.hpp"
#include "Config.hpp"
#include "ServerSide.hpp"
#include "Settings.hpp"

#include <AccountManager.hpp>

#include <bazarish/Bytes.hpp>
#include <bazarish/I2p.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bazarish::bot {

class Runtime {
public:
    explicit Runtime(Config config);
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    const Config& config() const { return config_; }
    Settings& settings() { return settings_; }

    struct Summary {
        std::string id;
        std::string name;
        std::string fingerprint;
        Settings::AccountKind kind = Settings::AccountKind::eBot;
        bool running = false;
        std::string status;
    };
    std::vector<Summary> accounts() const;

    std::shared_ptr<Account> account(const std::string& id);
    std::shared_ptr<Account> accountForToken(const std::string& secret);

    std::string createAccount(
        const std::string& name, const std::string& passphrase, Settings::AccountKind kind);
    std::string importAccount(const Bytes& bundle, const std::string& bundlePassphrase,
        const std::string& passphrase, Settings::AccountKind kind);
    void removeAccount(const std::string& id);

    struct PairingImport {
        bool active = false;
        bool needsCode = false;
        std::string stage;
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        int triesLeft = 0;
        std::string error;
        std::string account;
    };
    void startPairingImport(
        const std::string& link, const std::string& passphrase, Settings::AccountKind kind);
    void submitPairCode(const std::string& code);
    void cancelPairingImport();
    PairingImport pairingImport() const;

    void openAll();
    void stop();

private:
    ServerSide& serverSide();
    i2p::Router& router();
    std::string adopt(const client::AccountInfo& info, const std::string& passphrase,
        Settings::AccountKind kind);
    std::filesystem::path transcriptFileFor(const std::string& id) const;
    void runPairWorker(std::function<void()> work);

    Config config_;
    Settings settings_;
    client::AccountManager manager_;

    std::mutex hostMutex_;
    std::unique_ptr<ServerSide> mailbox_;

    mutable std::mutex accountsMutex_;
    std::map<std::string, std::shared_ptr<Account>> accounts_;

    struct PairingSlot {
        std::mutex mutex;
        PairingImport state;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<i2p::Endpoint> endpoint;
        std::string dest;
        std::string passphrase;
        Settings::AccountKind kind = Settings::AccountKind::eBot;
    };
    const std::shared_ptr<PairingSlot> pairing_ = std::make_shared<PairingSlot>();
    std::mutex pairWorkerMutex_;
    std::thread pairWorker_;
};

}  // namespace bazarish::bot
