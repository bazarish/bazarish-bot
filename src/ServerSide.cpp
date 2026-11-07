// Bazarish project (c) 2026
#include "ServerSide.hpp"

#include <BuildVersion.hpp>
#include <StateIdentity.hpp>

#include <bazarish/Bytes.hpp>
#include <bazarish/Certificates.hpp>
#include <bazarish/Clock.hpp>
#include <bazarish/I2p.hpp>
#include <bazarish/Limits.hpp>
#include <bazarish/Log.hpp>
#include <bazarish/PrivateFile.hpp>
#include <bazarish/Tls.hpp>



#include <filesystem>
#include <stdexcept>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMailboxHost = "127.0.0.1";
constexpr int kOwnDelegationDays = static_cast<int>(bazarish::kMaxDelegationDays);
constexpr std::uint64_t kOwnQuotaBytes = 4ULL * 1024 * 1024 * 1024;

bool renewalDue(const std::int64_t expires, const std::int64_t now)
{
    const std::int64_t term = kOwnDelegationDays * bazarish::kSecondsPerDay;
    return now > expires
        - static_cast<std::int64_t>(static_cast<double>(term) * bazarish::kDelegationRenewAtFraction);
}

}  // namespace

ServerSide::ServerSide(fs::path stateDir, const i2p::Privacy privacy, i2p::Router* const router)
    : stateDir_(std::move(stateDir))
    , privacy_(privacy)
    , router_(router)
    , host_(kMailboxHost)
    , identity_(std::make_shared<const Identity>(
          server::loadOrCreateIdentity(stateDir_ / "server")))
    , sealing_(std::make_shared<Key>(server::loadOrCreateSealingKey(stateDir_ / "server")))
    , fingerprint_(identity_->fingerprint())
{
    const fs::path serverDir = stateDir_ / "server";
    storage_ = std::make_unique<server::DiskStorage>(serverDir / "storage");
    gate_ = std::make_unique<server::EventGate>();

    server::DeliveryEngineOptions engineOptions;
    engineOptions.onDelivered = [this](const std::string& user) { gate_->notify(user); };
    engine_ = std::make_unique<server::DeliveryEngine>(*storage_, engineOptions);

    destinations_ = std::make_unique<server::PerUserDestinations>(router_, *engine_, serverDir,
        privacy_);
    destinations_->reloadPersisted(nowSeconds());

    card_ = ServerCard::issue(*identity_, ServerCard::Service::eMessaging,
        Key::fromPublicDer(sealing_->publicDer()), nowSeconds());
    writeFileAtomic(serverDir / "server-card.der", card_);

    subscriptions_ = std::make_unique<service::SubscriptionStore>(serverDir / "node");
    grants_ = std::make_unique<service::GrantStore>(serverDir / "node");
    service::NodeSettings::Values nodeValues;
    nodeValues.requireApproval = false;
    nodeValues.defaultQuotaBytes = kOwnQuotaBytes;
    nodeSettings_ = std::make_shared<service::NodeSettings>(
        serverDir / "node" / "node.json", nodeValues);

    const auto nodeSealing
        = std::make_shared<Key>(server::loadOrCreateSealingKey(serverDir / "node"));
    const tls::Credential credential = tls::selfSigned(serverDir);
    service::AccountApiOptions accountOptions;
    accountOptions.serverFingerprint = fingerprint_;
    accountOptions.serverCardDer = card_;
    accountOptions.sealingKey = nodeSealing;
    accountOptions.nodeCardDer = ServerCard::issue(*identity_, ServerCard::Service::eServiceNode,
        Key::fromPublicDer(nodeSealing->publicDer()), nowSeconds());
    accountOptions.tlsCertificate = credential.certificate.string();
    accountOptions.tlsKey = credential.key.string();
    accountOptions.version = kBuildVersion;
    accountOptions.settings = nodeSettings_;
    accountOptions.logins = std::make_shared<service::LoginMailbox>();
    accountOptions.forgetMailbox = [this](const std::string& user) {
        nodeCards_->forget(user);
        destinations_->forget(user);
        storage_->forgetUser(user);
        return true;
    };
    accountOptions.admitsUser = [this](const std::string& user) {
        const std::lock_guard<std::mutex> lock(ownMutex_);
        return own_.count(user) != 0;
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
    options.version = kBuildVersion;
    options.tlsCertificate = credential.certificate.string();
    options.tlsKey = credential.key.string();
    options.defaultQuotaBytes = kOwnQuotaBytes;
    options.eventGate = gate_.get();
    options.serverCardDer = card_;
    options.sealingKey = sealing_;
    options.identity = identity_;
    options.assignDestination
        = [this](const std::string& user) -> server::DestinationAssignment {
        const server::PerUserDestinations::Assignment assigned
            = destinations_->assignmentFor(user, nowSeconds());
        return {assigned.dest, assigned.state};
    };
    nodeCards_ = std::make_shared<server::NodeCards>(
        server::NodeAddress{host_, nodePort_, true, credential.pin}, identity_);
    options.accountKnown = [cards = nodeCards_](const std::string& user) {
        return cards->card(user).has_value();
    };
    api_ = std::make_unique<server::HttpApi>(*storage_, options);
    mailboxPort_ = api_->bind(host_, 0);
    if (mailboxPort_ <= 0) {
        throw std::runtime_error("the mailbox cannot take a port on " + host_);
    }

    facade::FacadeOptions facadeOptions;
    facadeOptions.messaging = {host_, mailboxPort_, credential.pin};
    facadeOptions.serviceNode = {host_, nodePort_, credential.pin};
    facadeOptions.version = kBuildVersion;
    facade_ = std::make_unique<facade::Facade>(facadeOptions);
    facadePort_ = facade_->bind(host_, 0);
    if (facadePort_ <= 0) {
        throw std::runtime_error("the facade cannot take a port on " + host_);
    }

    destinations_->setGetCard(
        [cards = nodeCards_](const std::string& fingerprint) { return cards->card(fingerprint); });

    log::info("server up on {}: mailbox {}, accounts {}, facade {}", host_, mailboxPort_,
        nodePort_, facadePort_);
}

void ServerSide::admit(const std::string& fingerprint)
{
    const std::lock_guard<std::mutex> lock(ownMutex_);
    own_.insert(fingerprint);
}

void ServerSide::endMailWait(const std::string& account)
{
    gate_->notify(account);
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

fs::path ServerSide::keyFileFor(const std::string& account) const
{
    return stateDir_ / "server" / "destinations" / (account + ".key");
}

std::string ServerSide::serveDestinationFor(
    const std::string& account, const client::AddressKeys& keys)
{
    const fs::path keyFile = keyFileFor(account);
    // Raised again, an address leaves a dead lease set that senders keep using: a live one stays.
    const std::int64_t held = destinations_->heldUntil(account);
    const std::shared_ptr<Key> served = destinations_->assignmentFor(account, nowSeconds()).sealingKey;
    const bool sameKeys
        = served && served->publicDer() == Key::fromPrivatePem(keys.sealingPem).publicDer();
    if (held > 0 && sameKeys && fs::exists(keyFile) && !renewalDue(held, nowSeconds())) {
        {
            const std::lock_guard<std::mutex> lock(expiresMutex_);
            destinationExpires_[account] = held;
        }
        return destinations_->assignmentFor(account, nowSeconds()).dest;
    }
    std::string blob;
    if (fs::exists(keyFile)) {
        blob = readFileText(keyFile);
    } else {
        blob = toBase64(bazarish::i2p::Keys::generate().blob());
        fs::create_directories(keyFile.parent_path());
        writePrivateFile(keyFile, blob);
    }
    const bazarish::i2p::Keys delegated
        = bazarish::i2p::Keys::fromBlob(fromBase64(blob)).issueTransient(kOwnDelegationDays);
    {
        const std::lock_guard<std::mutex> lock(expiresMutex_);
        destinationExpires_[account] = delegated.transientExpires();
    }
    destinations_->provision(account, toBase64(delegated.blob()), delegated.transientExpires(),
        keys.sealingPem, keys.signingPem);
    return destinations_->assignmentFor(account, nowSeconds()).dest;
}

bool ServerSide::destinationNeedsRenewal(const std::string& account, const std::int64_t now) const
{
    std::int64_t expires = 0;
    {
        const std::lock_guard<std::mutex> lock(expiresMutex_);
        const auto found = destinationExpires_.find(account);
        if (found == destinationExpires_.end()) {
            return false;
        }
        expires = found->second;
    }
    return renewalDue(expires, now);
}

std::string ServerSide::replaceDestinationFor(
    const std::string& account, const client::AddressKeys& keys)
{
    std::error_code ignored;
    fs::remove(keyFileFor(account), ignored);
    return serveDestinationFor(account, keys);
}

std::string ServerSide::destination(const std::string& account) const
{
    {
        const std::lock_guard<std::mutex> lock(expiresMutex_);
        if (destinationExpires_.count(account) == 0) {
            return {};
        }
    }
    return destinations_->assignmentFor(account, nowSeconds()).dest;
}

void ServerSide::takeDown(const std::string& account)
{
    {
        const std::lock_guard<std::mutex> lock(expiresMutex_);
        destinationExpires_.erase(account);
    }
    destinations_->forget(account);
}

void ServerSide::forget(const std::string& account)
{
    {
        const std::lock_guard<std::mutex> lock(expiresMutex_);
        destinationExpires_.erase(account);
    }
    destinations_->forget(account);
    storage_->forgetUser(account);
    std::error_code ignored;
    fs::remove(keyFileFor(account), ignored);
}

}  // namespace bazarish::bot
