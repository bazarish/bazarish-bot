// Bazarish project (c) 2026
#include "ServerSide.hpp"

#include <bazarish/Auth.hpp>
#include <bazarish/Bytes.hpp>
#include <bazarish/Certificates.hpp>
#include <bazarish/HttpClient.hpp>
#include <bazarish/Clock.hpp>
#include <bazarish/I2p.hpp>
#include <bazarish/Limits.hpp>
#include <bazarish/Log.hpp>
#include <bazarish/Tls.hpp>

#include <nlohmann/json.hpp>

#include <chrono>
#include <map>
#include <optional>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bazarish::daemon {

namespace fs = std::filesystem;

namespace {

constexpr const char* kVersion = "0.0.1";
constexpr const char* kMailboxHost = "127.0.0.1";
constexpr int kOwnDelegationDays = static_cast<int>(bazarish::kMaxDelegationDays);
constexpr int kOwnDestTunnelQuantity = 3;
constexpr std::uint64_t kOwnQuotaBytes = 4ULL * 1024 * 1024 * 1024;
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
    , identity_(std::make_shared<const Identity>(
          loadOrCreateIdentity(stateDir_ / "server" / "identity.pem")))
    , sealing_(std::make_shared<Key>(loadOrCreateSealingKey(stateDir_ / "server" / "sealing.pem")))
    , fingerprint_(identity_->fingerprint())
{
    const fs::path serverDir = stateDir_ / "server";
    storage_ = std::make_unique<server::DiskStorage>(serverDir / "storage");
    gate_ = std::make_unique<server::EventGate>();

    server::DeliveryEngineOptions engineOptions;
    engineOptions.onDelivered = [this](const std::string& user) { gate_->notify(user); };
    engine_ = std::make_unique<server::DeliveryEngine>(
        *storage_, fingerprint_, sealing_, engineOptions);

    destinations_ = std::make_unique<server::PerUserDestinations>(router_, *engine_, serverDir,
        privacy_, kOwnDestTunnelQuantity);
    destinations_->reloadPersisted(nowSeconds());

    card_ = ServerCard::issue(
        *identity_, Key::fromPublicDer(sealing_->publicDer()), nowSeconds());
    {
        std::ofstream out(serverDir / "server-card.der", std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(card_.data()),
            static_cast<std::streamsize>(card_.size()));
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write the server card");
        }
    }

    subscriptions_ = std::make_unique<service::SubscriptionStore>(serverDir / "node");
    grants_ = std::make_unique<service::GrantStore>(serverDir / "node");
    service::NodeSettings::Values nodeValues;
    nodeValues.requireApproval = false;
    nodeValues.defaultQuotaBytes = kOwnQuotaBytes;
    nodeSettings_ = std::make_shared<service::NodeSettings>(
        serverDir / "node" / "node.json", nodeValues);

    service::AccountApiOptions accountOptions;
    accountOptions.serverFingerprint = fingerprint_;
    accountOptions.serverCardDer = card_;
    accountOptions.version = kVersion;
    accountOptions.settings = nodeSettings_;
    accountOptions.forgetMailbox = [this](const std::string& user) {
        destinations_->revoke(user);
        storage_->forgetUser(user);
        return true;
    };
    accountOptions.publishQuota
        = [this](const std::string& user, const std::uint64_t quotaBytes) {
        storage_->setUserQuota(user, quotaBytes);
    };
    accounts_ = std::make_unique<service::AccountApi>(*subscriptions_, *grants_, accountOptions);
}

ServerSide::~ServerSide()
{
    stop();
}

void ServerSide::start()
{
    nodePort_ = accounts_->bind(host_, 0);
    if (nodePort_ <= 0) {
        throw std::runtime_error("the account node cannot take a port on " + host_);
    }

    const tls::Credential credential = tls::selfSigned(stateDir_ / "server");

    server::HttpApiOptions options;
    options.version = kVersion;
    options.tlsCertificate = credential.certificate.string();
    options.tlsKey = credential.key.string();
    options.defaultQuotaBytes = kOwnQuotaBytes;
    options.eventGate = gate_.get();
    options.serverCardDer = card_;
    options.sealingKey = sealing_;
    options.identity = identity_;
    options.serviceNodeUrl = "http://" + host_ + ":" + std::to_string(nodePort_);
    if (router_ != nullptr) {
        options.reseedSample
            = [](const std::size_t count) { return i2p::sampleRouterInfos(count); };
    }
    options.assignDestination
        = [this](const std::string& user) -> server::DestinationAssignment {
        const server::PerUserDestinations::Assignment assigned
            = destinations_->assignmentFor(user, nowSeconds());
        return {assigned.dest, assigned.sealingKey, assigned.state};
    };
    api_ = std::make_unique<server::HttpApi>(*storage_, options);
    mailboxPort_ = api_->bind(host_, 0);
    if (mailboxPort_ <= 0) {
        throw std::runtime_error("the mailbox cannot take a port on " + host_);
    }

    facade::FacadeOptions facadeOptions;
    facadeOptions.messagingHost = host_;
    facadeOptions.messagingPort = mailboxPort_;
    facadeOptions.messagingPin = credential.pin;
    facadeOptions.version = kVersion;
    facade_ = std::make_unique<facade::Facade>(facadeOptions);
    facadePort_ = facade_->bind(host_, 0);
    if (facadePort_ <= 0) {
        throw std::runtime_error("the facade cannot take a port on " + host_);
    }

    const std::string nodeHost = host_;
    const int nodePort = nodePort_;
    const std::shared_ptr<const Identity> identity = identity_;
    destinations_->setGetCard(
        [nodeHost, nodePort, identity](const std::string& fingerprint) -> std::optional<Bytes> {
            http::ClientRequest lookup;
            lookup.method = "GET";
            lookup.target = "/v1/account/card?user=" + fingerprint;
            const auth::Headers signature
                = auth::signRequest(*identity, nowSeconds(), "GET", "/v1/account/card", {});
            lookup.headers = std::map<std::string, std::string>(signature.begin(), signature.end());
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
}

Bytes ServerSide::card() const
{
    return card_;
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
        blob = toBase64(bazarish::i2p::Keys::generate().blob());
        writePrivateFile(keyFile, blob);
    }
    const bazarish::i2p::Keys delegated
        = bazarish::i2p::Keys::fromBlob(fromBase64(blob)).issueTransient(kOwnDelegationDays);
    destinationExpires_ = delegated.transientExpires();
    destinations_->provision(account_, toBase64(delegated.blob()), destinationExpires_);
    return destinations_->assignmentFor(account_, nowSeconds()).dest;
}

bool ServerSide::destinationNeedsRenewal(const std::int64_t now) const
{
    if (account_.empty() || destinationExpires_ == 0) {
        return false;
    }
    const std::int64_t term = kOwnDelegationDays * bazarish::kSecondsPerDay;
    return now > destinationExpires_
        - static_cast<std::int64_t>(static_cast<double>(term) * bazarish::kDelegationRenewAtFraction);
}

std::string ServerSide::replaceDestinationFor(const std::string& accountFingerprint)
{
    std::error_code ignored;
    fs::remove(stateDir_ / "server" / "destination.key", ignored);
    return serveDestinationFor(accountFingerprint);
}

std::string ServerSide::destination() const
{
    return account_.empty() ? std::string()
                            : destinations_->assignmentFor(account_, nowSeconds()).dest;
}

}  // namespace bazarish::daemon
