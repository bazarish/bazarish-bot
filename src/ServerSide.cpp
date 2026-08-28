// Bazarish project (c) 2026
#include "ServerSide.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Certificates.hpp>
#include <bazarish/Log.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bazarish::daemon {

namespace fs = std::filesystem;

namespace {

// The version this mailbox reports to its one client.
constexpr const char* kVersion = "0.0.1";
// The mailbox answers this process and nothing else, so it takes a port the
// system hands out rather than one an operator has to keep out of the way.
constexpr const char* kMailboxHost = "127.0.0.1";
// This daemon's destination is not delegated and does not lapse; the field the
// server keeps for an offline transient's expiry is given a year and renewed at
// every start, so the sweep that retires lapsed destinations never takes it.
constexpr std::int64_t kOwnDestLifetimeSeconds = 365 * 24 * 60 * 60;
// The mailbox is this account's alone, so its destination carries one account's
// traffic rather than a server's: a full pool would be tunnels built for nobody.
constexpr int kOwnDestTunnelQuantity = 3;

std::int64_t nowSeconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

std::string readFileText(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void writePrivateFile(const fs::path& path, const std::string& text)
{
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write " + path.string());
        }
    }
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
}

Identity loadOrCreateIdentity(const fs::path& path)
{
    if (fs::exists(path)) {
        return Identity::fromPrivatePem(readFileText(path));
    }
    Identity identity = Identity::generate();
    writePrivateFile(path, identity.privatePem());
    return identity;
}

Key loadOrCreateSealingKey(const fs::path& path)
{
    if (fs::exists(path)) {
        return Key::fromPrivatePem(readFileText(path));
    }
    Key key = Key::generateSealing();
    writePrivateFile(path, key.privatePem());
    return key;
}

}  // namespace

ServerSide::ServerSide(fs::path stateDir, const i2p::Privacy privacy, i2p::Router* const router)
    : stateDir_(std::move(stateDir))
    , privacy_(privacy)
    , router_(router)
    , host_(kMailboxHost)
    , port_(0)
    , identity_(loadOrCreateIdentity(stateDir_ / "server" / "identity.pem"))
    , sealing_(loadOrCreateSealingKey(stateDir_ / "server" / "sealing.pem"))
    , fingerprint_(identity_.fingerprint())
{
    const fs::path serverDir = stateDir_ / "server";
    storage_ = std::make_unique<server::DiskStorage>(serverDir / "storage");
    gate_ = std::make_unique<server::EventGate>();

    server::DeliveryEngineOptions engineOptions;
    engineOptions.onDelivered = [this](const std::string& user) { gate_->notify(user); };
    engine_ = std::make_unique<server::DeliveryEngine>(*storage_, fingerprint_,
        Key::fromPrivatePem(sealing_.privatePem()), engineOptions);

    destinations_ = std::make_unique<server::PerUserDestinations>(router_, *engine_, serverDir,
        privacy_, kOwnDestTunnelQuantity);
    destinations_->reloadPersisted(nowSeconds());

    server::HttpApiOptions options;
    options.version = kVersion;
    options.defaultQuotaBytes = engineOptions.defaultQuotaBytes;
    options.assignDestination
        = [this](const std::string& user) -> server::DestinationAssignment {
        const std::shared_ptr<Key> key = destinations_->servingKeyFor(user);
        if (!key) {
            return {std::string(), nullptr, "none"};
        }
        return {destinations_->destinationFor(user), key,
            destinations_->stateFor(user, nowSeconds())};
    };
    api_ = std::make_unique<server::HttpApi>(*storage_, *engine_, options);
    api_->setEventGate(gate_.get());

    // The card carries no endpoint: this mailbox has no address of its own, and
    // the account is reached at the destination it federates on.
    const Bytes card = ServerCard::issue(identity_, {},
        Key::fromPublicDer(sealing_.publicDer()), nowSeconds());
    {
        std::ofstream out(serverDir / "server-card.der", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(card.data()),
            static_cast<std::streamsize>(card.size()));
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write the server card");
        }
    }
}

ServerSide::~ServerSide()
{
    stop();
}

void ServerSide::start()
{
    const int bound = api_->bindToAnyPort(host_, port_);
    if (bound == 0) {
        throw std::runtime_error(
            "the mailbox cannot listen on " + host_ + ":" + std::to_string(port_));
    }
    port_ = bound;
    // The engine serves on threads of its own; this call is the one that parks a
    // caller until stop(), so it is given a thread rather than this one.
    serving_ = std::thread([this]() {
        if (!api_->listenAfterBind()) {
            log::error("the mailbox bound its port but will not serve");
        }
    });
    log::info("mailbox listening on {}:{}", host_, port_);
}

void ServerSide::stop()
{
    if (api_) {
        api_->stop();
    }
    if (serving_.joinable()) {
        serving_.join();
    }
}

Bytes ServerSide::card() const
{
    const fs::path path = stateDir_ / "server" / "server-card.der";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read the server card");
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    return Bytes(text.begin(), text.end());
}

std::string ServerSide::serveDestinationFor(const std::string& accountFingerprint)
{
    account_ = accountFingerprint;
    const fs::path keyFile = stateDir_ / "server" / "destination.key";
    std::string blob;
    if (fs::exists(keyFile)) {
        blob = readFileText(keyFile);
    } else {
        if (router_ == nullptr) {
            throw std::runtime_error("no router: this daemon has no destination to serve on");
        }
        blob = toBase64(router_->generateKeys().blob());
        writePrivateFile(keyFile, blob);
    }
    destinations_->provision(account_, blob, nowSeconds() + kOwnDestLifetimeSeconds);
    return destinations_->destinationFor(account_);
}

std::string ServerSide::destination() const
{
    return account_.empty() ? std::string() : destinations_->destinationFor(account_);
}

}  // namespace bazarish::daemon
