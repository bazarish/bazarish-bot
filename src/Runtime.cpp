// Bazarish project (c) 2026
#include "Runtime.hpp"

#include "Backup.hpp"

#include <BuildVersion.hpp>
#include <I2pRouter.hpp>

#include <bazarish/Clock.hpp>
#include <bazarish/Log.hpp>
#include <bazarish/ServerDescriptor.hpp>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr const char* kAccountsDirectory = "accounts";
constexpr const char* kI2pDirectory = "i2p";

}  // namespace

Runtime::Runtime(Config config)
    : config_(std::move(config))
    , settings_(config_.stateDir / "settings.json")
    , manager_(config_.stateDir / kAccountsDirectory)
{
    bazarish::setSelfHostedFacadeOnLoopback(true);
    manager_.discardInterruptedImports();
    for (const client::AccountInfo& info : manager_.list()) {
        const std::lock_guard<std::mutex> lock(accountsMutex_);
        accounts_[info.id] = std::make_shared<Account>(
            info.id, info.file, settings_, [this]() -> ServerSide& { return serverSide(); });
    }
}

Runtime::~Runtime()
{
    stop();
}

i2p::Router& Runtime::router()
{
    client::setRouterTransit(settings_.transit());
    client::setTunnelPrivacy(settings_.privacy());
    if (settings_.transport() == Settings::Transport::eSam) {
        client::setSamTransport(settings_.samHost(), settings_.samPort());
    }
    return client::sharedI2pRouter(config_.stateDir / kI2pDirectory);
}

ServerSide& Runtime::serverSide()
{
    const std::lock_guard<std::mutex> lock(hostMutex_);
    if (mailbox_ == nullptr) {
        if (settings_.transport() == Settings::Transport::eSam) {
            log::info("transport: SAM at {}:{}", settings_.samHost(), settings_.samPort());
        } else {
            log::info("transport: a router of our own under {}",
                (config_.stateDir / kI2pDirectory).string());
        }
        i2p::Router& shared = router();
        auto mailbox = std::make_unique<ServerSide>(
            config_.stateDir, settings_.privacy(), std::ref(shared));
        mailbox->start();
        mailbox_ = std::move(mailbox);
    }
    return *mailbox_;
}

std::vector<Runtime::Summary> Runtime::accounts() const
{
    std::vector<Summary> summaries;
    const std::lock_guard<std::mutex> lock(accountsMutex_);
    for (const auto& [id, account] : accounts_) {
        Summary summary;
        summary.id = id;
        summary.kind = settings_.kind(id);
        summary.running = account->running();
        summary.status = account->status();
        summary.name = account->displayName();
        summary.fingerprint
            = summary.running ? account->fingerprint() : settings_.fingerprintOf(id);
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

std::shared_ptr<Account> Runtime::account(const std::string& id)
{
    const std::lock_guard<std::mutex> lock(accountsMutex_);
    const auto found = accounts_.find(id);
    if (found == accounts_.end()) {
        throw std::runtime_error("there is no account " + id);
    }
    return found->second;
}

std::shared_ptr<Account> Runtime::accountForToken(const std::string& secret)
{
    const std::optional<ApiToken> token = settings_.tokenFor(secret);
    if (!token) {
        return nullptr;
    }
    const std::lock_guard<std::mutex> lock(accountsMutex_);
    const auto found = accounts_.find(token->account);
    return found == accounts_.end() ? nullptr : found->second;
}

std::string Runtime::adopt(const client::AccountInfo& info, const Settings::AccountKind kind)
{
    const std::string held = settings_.claimFingerprint(info.id, info.fingerprint, kind);
    if (!held.empty()) {
        manager_.remove(info.id);
        throw std::runtime_error("this account is already here as " + held);
    }
    const auto added = std::make_shared<Account>(
        info.id, info.file, settings_, [this]() -> ServerSide& { return serverSide(); });
    {
        const std::lock_guard<std::mutex> lock(accountsMutex_);
        accounts_[info.id] = added;
    }
    log::info("account {} added as a {}", info.id, accountKindName(kind));
    added->open();
    return info.id;
}

std::string Runtime::createAccount(const std::string& name, const Settings::AccountKind kind)
{
    return adopt(manager_.create(name, storedAccountKind(kind)), kind);
}

std::string Runtime::importAccount(
    const Bytes& bundle, const std::string& bundlePassphrase, const Settings::AccountKind kind)
{
    if (client::Session::bundleKind(bundle, bundlePassphrase) != storedAccountKind(kind)) {
        throw std::runtime_error("this account was created as another kind of account");
    }
    return adopt(manager_.import(std::string(), bundle, bundlePassphrase), kind);
}

void Runtime::removeAccount(const std::string& id)
{
    const std::shared_ptr<Account> removed = account(id);
    if (!removed->running()) {
        throw std::runtime_error("open the account first: its server copy is closed from inside it");
    }
    const std::string fingerprint = removed->fingerprint();
    removed->closeOnServer();
    {
        const std::lock_guard<std::mutex> lock(accountsMutex_);
        accounts_.erase(id);
    }
    removed->close();
    serverSide().forget(fingerprint);
    manager_.remove(id);
    settings_.forgetAccount(id);
    log::info("account {} removed", id);
}

void Runtime::openAll()
{
    std::vector<std::shared_ptr<Account>> held;
    {
        const std::lock_guard<std::mutex> lock(accountsMutex_);
        for (const auto& [id, account] : accounts_) {
            held.push_back(account);
        }
    }
    for (const std::shared_ptr<Account>& account : held) {
        if (!settings_.enabled(account->id())) {
            log::info("account {} is switched off", account->id());
            const std::string fingerprint = settings_.fingerprintOf(account->id());
            if (!fingerprint.empty()) {
                serverSide().takeDown(fingerprint);
            }
            continue;
        }
        try {
            account->open();
        } catch (const std::exception& error) {
            log::info("account {} stays closed: {}", account->id(), error.what());
        }
    }
}

void Runtime::stop()
{
    std::map<std::string, std::shared_ptr<Account>> closing;
    {
        const std::lock_guard<std::mutex> lock(accountsMutex_);
        closing.swap(accounts_);
    }
    std::vector<std::shared_ptr<Account>> all;
    for (auto& [id, account] : closing) {
        all.push_back(account);
    }
    closeAll(all);
    const std::lock_guard<std::mutex> lock(hostMutex_);
    if (mailbox_) {
        mailbox_->stop();
        mailbox_.reset();
    }
}

void Runtime::closeAll(const std::vector<std::shared_ptr<Account>>& accounts)
{
    std::vector<std::thread> closers;
    for (const std::shared_ptr<Account>& account : accounts) {
        closers.emplace_back([account]() { account->close(); });
    }
    for (std::thread& closer : closers) {
        closer.join();
    }
}

std::int64_t Runtime::uptimeSeconds() const
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - startedAt_)
        .count();
}

Runtime::RouterReport Runtime::routerReport()
{
    i2p::Router& engine = router();
    RouterReport report;
    report.transport = settings_.transport();
    report.running = engine.running();
    report.online = report.running && engine.ready();
    report.counted = engine.capabilities().routerCounters;
    if (report.counted) {
        report.inboundTunnels = engine.inboundTunnels();
        report.outboundTunnels = engine.outboundTunnels();
        report.transitTunnels = engine.transitTunnels();
        report.knownRouters = engine.knownRouters();
        report.floodfills = engine.floodfills();
        for (const i2p::TransportPeer& peer : engine.transportPeers()) {
            ++report.connections[peer.transport];
        }
    }
    for (const i2p::LocalDestination& destination : engine.localDestinations()) {
        ++report.destinations;
        report.readyDestinations += destination.ready ? 1 : 0;
    }
    return report;
}

void Runtime::setEnabled(const std::string& id, const bool enabled)
{
    const std::shared_ptr<Account> account = this->account(id);
    settings_.setEnabled(id, enabled);
    log::info("account {} switched {}", id, enabled ? "on" : "off");
    if (enabled) {
        account->open();
        return;
    }
    account->close();
    const std::string fingerprint = settings_.fingerprintOf(id);
    if (!fingerprint.empty()) {
        serverSide().takeDown(fingerprint);
    }
}

void Runtime::quiesced(const std::function<void()>& work)
{
    const std::lock_guard<std::mutex> maintenance(maintenanceMutex_);
    std::vector<std::shared_ptr<Account>> all;
    {
        const std::lock_guard<std::mutex> lock(accountsMutex_);
        for (const auto& [id, account] : accounts_) {
            all.push_back(account);
        }
    }
    std::vector<std::shared_ptr<Account>> wereOpen;
    for (const std::shared_ptr<Account>& account : all) {
        if (account->live()) {
            wereOpen.push_back(account);
        }
    }
    // The server stays up: its storage writes each file whole, and only the accounts keep databases.
    closeAll(all);
    const auto reopen = [&wereOpen]() {
        for (const std::shared_ptr<Account>& account : wereOpen) {
            account->open();
        }
    };
    try {
        work();
    } catch (...) {
        reopen();
        throw;
    }
    reopen();
}

Bytes Runtime::exportEverything()
{
    Bytes archive;
    quiesced([this, &archive]() {
        archive = exportBackup(config_.stateDir, config_.path, kBuildVersion, nowSeconds());
    });
    return archive;
}

}  // namespace bazarish::bot
