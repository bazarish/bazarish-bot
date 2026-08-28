// Bazarish project (c) 2026
#include "ServerSide.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Certificates.hpp>
#include <bazarish/HttpClient.hpp>
#include <bazarish/Log.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>

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
// What the account is allowed to keep in its own mailbox on its own disk. There
// is nobody to ration here, so this is a backstop against a runaway, not a
// tariff: 4 GiB.
constexpr std::uint64_t kOwnQuotaBytes = 4ULL * 1024 * 1024 * 1024;
// The account node is in this process; a lookup that takes longer than this is
// not a slow node, it is a broken one.
constexpr int kNodeTimeoutSeconds = 5;

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
    // The account asks its server for the slice of netDb a router with no peers
    // starts from. Here that is this same process's router, which bootstraps from
    // the public hosts because there is no other server to ask.
    if (router_ != nullptr) {
        options.reseedSample
            = [](const std::size_t count) { return i2p::sampleRouterInfos(count); };
    }
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

    // The account node: what issues this account's card, its quota and its
    // tokens. On a public server it is a separate daemon run by an operator; here
    // the operator is the account, so it registers openly and asks no captcha.
    subscriptions_ = std::make_unique<service::SubscriptionStore>(serverDir / "node");
    grants_ = std::make_unique<service::GrantStore>(serverDir / "node");
    nodeSettings_ = std::make_shared<service::NodeSettings>(serverDir / "node" / "node.json",
        service::NodeSettings::Values{.requireApproval = false,
            .registrationMessage = {},
            .portalMessage = {},
            .requireCaptcha = false,
            .facades = {},
            .reseeds = {},
            .defaultQuotaBytes = kOwnQuotaBytes,
            .idleDeleteDays = 0});

    service::AccountApiOptions accountOptions;
    accountOptions.serverFingerprint = fingerprint_;
    accountOptions.serverCardDer = card;
    accountOptions.version = kVersion;
    accountOptions.settings = nodeSettings_;
    accountOptions.defaultQuotaBytes = kOwnQuotaBytes;
    accountOptions.registration = service::AccountApiOptions::Registration::eOpen;
    accounts_ = std::make_unique<service::AccountApi>(*subscriptions_, *grants_, accountOptions);
}

ServerSide::~ServerSide()
{
    stop();
}

void ServerSide::start()
{
    // Each component takes a port the system hands out: they answer this process
    // and nothing else, so there is no address for an operator to keep free.
    mailboxPort_ = api_->bindToAnyPort(host_, 0);
    if (mailboxPort_ == 0) {
        throw std::runtime_error("the mailbox cannot take a port on " + host_);
    }
    nodePort_ = accounts_->bindToAnyPort(host_);
    if (nodePort_ == 0) {
        throw std::runtime_error("the account node cannot take a port on " + host_);
    }

    // What the node owes the mailbox - the quota an account was granted - goes
    // over the same private channel a deployed fleet uses.
    push_ = std::make_unique<service::OperatorPush>(
        "http://" + host_ + ":" + std::to_string(mailboxPort_), identity_);

    facade::FacadeOptions facadeOptions;
    facadeOptions.messagingHost = host_;
    facadeOptions.messagingPort = mailboxPort_;
    facadeOptions.accountHost = host_;
    facadeOptions.accountPort = nodePort_;
    facadeOptions.version = kVersion;
    facade_ = std::make_unique<facade::Facade>(facadeOptions);
    facadePort_ = facade_->bindToAnyPort(host_);
    if (facadePort_ == 0) {
        throw std::runtime_error("the facade cannot take a port on " + host_);
    }

    // First contact: a peer dials this account's destination and asks for its
    // card. The destination does not hold cards - the account node does - so the
    // question is passed to it, exactly as a deployed server passes it to the
    // service node it belongs to.
    const std::string nodeHost = host_;
    const int nodePort = nodePort_;
    destinations_->setGetCard(
        [nodeHost, nodePort](const std::string& fingerprint,
            const std::string& view) -> std::optional<Bytes> {
            http::ClientRequest lookup;
            lookup.method = "GET";
            lookup.target = "/v1/account/card?user=" + fingerprint + "&view=" + view;
            http::ClientOptions options;
            options.connectTimeout = std::chrono::seconds(kNodeTimeoutSeconds);
            options.readTimeout = std::chrono::seconds(kNodeTimeoutSeconds);
            const http::ClientResponse answer
                = http::request(nodeHost, nodePort, lookup, options);
            if (answer.status != 200) {
                return std::nullopt;
            }
            try {
                const nlohmann::json body = nlohmann::json::parse(answer.body);
                return fromBase64(body.at("card").get<std::string>());
            } catch (const std::exception&) {
                return std::nullopt;
            }
        });

    // Each serving call parks its caller until stop(), so each gets a thread.
    servingMailbox_ = std::thread([this]() {
        if (!api_->listenAfterBind()) {
            log::error("the mailbox bound its port but will not serve");
        }
    });
    servingAccounts_ = std::thread([this]() {
        if (!accounts_->listenAfterBind()) {
            log::error("the account node bound its port but will not serve");
        }
    });
    servingFacade_ = std::thread([this]() {
        if (!facade_->listenAfterBind()) {
            log::error("the facade bound its port but will not serve");
        }
    });
    log::info("server up on {}: mailbox {}, accounts {}, facade {}", host_, mailboxPort_,
        nodePort_, facadePort_);
}

void ServerSide::stop()
{
    if (facade_) {
        facade_->stop();
    }
    if (accounts_) {
        accounts_->stop();
    }
    if (api_) {
        api_->stop();
    }
    for (std::thread* const serving : {&servingFacade_, &servingAccounts_, &servingMailbox_}) {
        if (serving->joinable()) {
            serving->join();
        }
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
