// Bazarish project (c) 2026
#include "Runtime.hpp"

#include "Backup.hpp"
#include "Version.hpp"

#include <AccountKey.hpp>
#include <DevicePairing.hpp>
#include <I2pRouter.hpp>

#include <bazarish/Log.hpp>
#include <bazarish/PairLink.hpp>
#include <bazarish/ServerDescriptor.hpp>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr const char* kAccountsDirectory = "accounts";
constexpr const char* kTranscriptsDirectory = "transcripts";
constexpr const char* kGroupsDirectory = "groups";
constexpr const char* kI2pDirectory = "i2p";
constexpr const char* kTranscriptSuffix = ".db";
constexpr int kPairPollSeconds = 2;

}  // namespace

Runtime::Runtime(Config config)
    : config_(std::move(config))
    , settings_(config_.stateDir / "settings.json")
    , manager_(config_.stateDir / kAccountsDirectory)
{
    fs::create_directories(config_.stateDir / kTranscriptsDirectory);
    bazarish::setSelfHostedFacadeOnLoopback(true);
    for (const client::AccountInfo& info : manager_.list()) {
        const std::lock_guard<std::mutex> lock(accountsMutex_);
        accounts_[info.id] = std::make_shared<Account>(info.id, info.file,
            transcriptFileFor(info.id), groupFileFor(info.id), settings_,
            [this]() -> ServerSide& { return serverSide(); });
    }
}

Runtime::~Runtime()
{
    stop();
}

fs::path Runtime::transcriptFileFor(const std::string& id) const
{
    return config_.stateDir / kTranscriptsDirectory / (id + kTranscriptSuffix);
}

fs::path Runtime::groupFileFor(const std::string& id) const
{
    return config_.stateDir / kGroupsDirectory / (id + kTranscriptSuffix);
}

i2p::Router& Runtime::router()
{
    client::setI2pEnabled(true);
    client::setRouterTransit(settings_.transit());
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
        auto mailbox = std::make_unique<ServerSide>(config_.stateDir, settings_.privacy(), &shared);
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
    const std::string held = settings_.accountWithFingerprint(info.fingerprint);
    if (!held.empty() && held != info.id) {
        manager_.remove(info.id);
        throw std::runtime_error("this account is already here as " + held);
    }
    settings_.rememberAccount(info.id, info.fingerprint, kind);
    const auto added = std::make_shared<Account>(info.id, info.file, transcriptFileFor(info.id),
        groupFileFor(info.id), settings_, [this]() -> ServerSide& { return serverSide(); });
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
    std::error_code ignored;
    for (const fs::path& file : {transcriptFileFor(id), groupFileFor(id)}) {
        client::accountkey::forget(file);
        fs::remove(file, ignored);
        fs::remove(client::accountkey::sidecarFor(file), ignored);
    }
    settings_.forgetAccount(id);
    log::info("account {} removed", id);
}

void Runtime::startPairingImport(const std::string& link, const Settings::AccountKind kind)
{
    const PairLink parsed = parsePairLink(link);
    const std::shared_ptr<PairingSlot> slot = pairing_;
    const auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        const std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->state.active) {
            throw std::runtime_error("an account is already being received");
        }
        slot->state = {};
        slot->state.active = true;
        slot->state.stage = "reaching";
        slot->cancel = cancel;
        slot->endpoint.reset();
        slot->dest = parsed.dest;
        slot->kind = kind;
    }
    const fs::path i2pDir = config_.stateDir / kI2pDirectory;
    runPairWorker([this, slot, cancel, parsed, i2pDir]() {
        const auto settle = [&slot, &cancel](const std::string& error,
                                std::shared_ptr<i2p::Endpoint> endpoint) {
            const std::lock_guard<std::mutex> lock(slot->mutex);
            if (slot->cancel != cancel) {
                return;
            }
            if (!error.empty()) {
                slot->state.active = false;
                slot->state.stage = "failed";
                slot->state.error = error;
                slot->cancel.reset();
                return;
            }
            slot->endpoint = std::move(endpoint);
            slot->state.stage = "code";
            slot->state.needsCode = true;
        };
        try {
            client::applyLinkReseed(i2pDir, parsed.reseeds);
            i2p::Router& shared = router();
            if (client::bootstrapI2pRouter(i2pDir) == client::I2pBootstrap::eEmpty) {
                settle("no reseed answered: this host has no network database", nullptr);
                return;
            }
            const std::shared_ptr<i2p::Endpoint> endpoint
                = client::openPairLink(shared, client::tunnelPrivacy(), client::kPairingOwner);
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(client::kPairOwnTunnelsSeconds);
            bool ready = false;
            while (!cancel->load() && !ready) {
                ready = endpoint->waitReady(std::chrono::seconds(kPairPollSeconds));
                if (!ready && std::chrono::steady_clock::now() >= deadline) {
                    break;
                }
            }
            if (cancel->load()) {
                endpoint->stop();
                return;
            }
            if (!ready) {
                endpoint->stop();
                settle("this host could not build I2P tunnels", nullptr);
                return;
            }
            const std::unique_ptr<i2p::Stream> reached
                = endpoint->connect(parsed.dest, std::chrono::seconds(client::kPairDialSeconds));
            if (!reached) {
                endpoint->stop();
                settle("cannot reach the other device", nullptr);
                return;
            }
            reached->close();
            settle({}, endpoint);
        } catch (const std::exception& error) {
            settle(error.what(), nullptr);
        }
    });
}

void Runtime::submitPairCode(const std::string& code)
{
    if (!client::isPairCode(code)) {
        throw std::runtime_error("the code is the digits shown on the other device");
    }
    const std::shared_ptr<PairingSlot> slot = pairing_;
    std::shared_ptr<std::atomic<bool>> cancel;
    std::shared_ptr<i2p::Endpoint> endpoint;
    std::string dest;
    Settings::AccountKind kind = Settings::AccountKind::eBot;
    {
        const std::lock_guard<std::mutex> lock(slot->mutex);
        if (!slot->state.needsCode || slot->endpoint == nullptr) {
            throw std::runtime_error("no device is waiting for a code");
        }
        slot->state.needsCode = false;
        slot->state.stage = "receiving";
        cancel = slot->cancel;
        endpoint = slot->endpoint;
        dest = slot->dest;
        kind = slot->kind;
    }
    runPairWorker([this, slot, cancel, endpoint, dest, code, kind]() {
        std::string error;
        std::string id;
        client::PairFetchResult got;
        try {
            const client::PairProgressFn onProgress
                = [&slot, &cancel](const std::uint64_t done, const std::uint64_t total) {
                      const std::lock_guard<std::mutex> lock(slot->mutex);
                      if (slot->cancel == cancel) {
                          slot->state.done = done;
                          slot->state.total = total;
                      }
                  };
            got = client::fetchPairBundle(*endpoint, dest, code, onProgress, *cancel);
            if (!got.wrongCode && !cancel->load()) {
                id = importAccount(got.bundle, code, kind);
            }
        } catch (const std::exception& failure) {
            error = failure.what();
        }
        const std::lock_guard<std::mutex> lock(slot->mutex);
        if (slot->cancel != cancel) {
            return;
        }
        if (got.wrongCode) {
            slot->state.stage = "code";
            slot->state.needsCode = true;
            slot->state.triesLeft = got.triesLeft;
            return;
        }
        endpoint->stop();
        slot->endpoint.reset();
        slot->cancel.reset();
        slot->state.active = false;
        slot->state.needsCode = false;
        if (!error.empty()) {
            slot->state.stage = "failed";
            slot->state.error = error;
            return;
        }
        slot->state.stage = "done";
        slot->state.account = id;
    });
}

void Runtime::runPairWorker(std::function<void()> work)
{
    const std::lock_guard<std::mutex> lock(pairWorkerMutex_);
    if (pairWorker_.joinable()) {
        pairWorker_.join();
    }
    pairWorker_ = std::thread(std::move(work));
}

void Runtime::cancelPairingImport()
{
    const std::lock_guard<std::mutex> lock(pairing_->mutex);
    if (pairing_->cancel) {
        pairing_->cancel->store(true);
    }
    if (pairing_->endpoint) {
        pairing_->endpoint->stop();
    }
    pairing_->cancel.reset();
    pairing_->endpoint.reset();
    pairing_->state = {};
}

Runtime::PairingImport Runtime::pairingImport() const
{
    const std::lock_guard<std::mutex> lock(pairing_->mutex);
    return pairing_->state;
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
        try {
            account->open();
        } catch (const std::exception& error) {
            log::info("account {} stays closed: {}", account->id(), error.what());
        }
    }
}

void Runtime::stop()
{
    cancelPairingImport();
    {
        const std::lock_guard<std::mutex> lock(pairWorkerMutex_);
        if (pairWorker_.joinable()) {
            pairWorker_.join();
        }
    }
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
        archive = exportBackup(config_.stateDir, config_.path, kVersion,
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
    });
    return archive;
}

}  // namespace bazarish::bot
