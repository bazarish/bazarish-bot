// Bazarish project (c) 2026
#include "ServerSide.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Certificates.hpp>
#include <bazarish/HttpClient.hpp>
#include <bazarish/Clock.hpp>
#include <bazarish/I2p.hpp>
#include <bazarish/Limits.hpp>
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
// How long a delegation this daemon issues to its own server side runs for, and
// the point in that term at which it is re-issued. The daemon owns both halves,
// but the server side operates the address under a delegation like any other: it
// publishes the encrypted LeaseSet from the blinded keys the delegation carries,
// and it retires an address whose term has run out.
constexpr int kOwnDelegationDays = static_cast<int>(bazarish::kMaxDelegationDays);
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

    // The card every client of this mailbox needs before it can seal anything: it
    // carries the sealing key a tunnel hello is sealed to. Issued here, because
    // both the mailbox and the account node are handed it below.
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

    // The account node: what issues this account's card, its quota and its
    // passes. On a public server it is a separate daemon run by an operator; here
    // the operator is the account, so registration is open and nobody moderates
    // it.
    subscriptions_ = std::make_unique<service::SubscriptionStore>(serverDir / "node");
    grants_ = std::make_unique<service::GrantStore>(serverDir / "node");
    // Only the two that mean anything inside one process: the operator is the
    // account, so nothing is moderated, and the account it grants itself gets the
    // whole quota. The rest of the node's settings describe a deployment with a
    // portal and addresses of its own, which this one has not.
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
    // The node prepares a new serving key and the mailbox puts it in force. In a
    // deployment those are two daemons and a private channel between them; here
    // they are two objects, so the call goes straight across.
    accountOptions.prepareServingKey
        = [this](const std::string& user) -> std::optional<std::string> {
        const std::string prepared = destinations_->prepareServingKey(user);
        if (prepared.empty()) {
            return std::nullopt;
        }
        return prepared;
    };
    accountOptions.commitServingKey
        = [this](const std::string& user) { return destinations_->commitServingKey(user); };
    accountOptions.forgetMailbox = [this](const std::string& user) {
        destinations_->revoke(user);
        storage_->forgetUser(user);
        return true;
    };
    // In a deployment this is the node's signed push to a server across a private
    // channel. Here the two are objects in one process, so it is a call - but it
    // still has to happen, or the mailbox would enforce its own default against a
    // quota the node promised.
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
    // Each component takes a port the system hands out: they answer this process
    // and nothing else, so there is no address for an operator to keep free. The
    // account node goes first, because the mailbox has to be told where to forward
    // a client's account calls.
    nodePort_ = accounts_->bind(host_, 0);
    if (nodePort_ <= 0) {
        throw std::runtime_error("the account node cannot take a port on " + host_);
    }

    server::HttpApiOptions options;
    options.version = kVersion;
    options.defaultQuotaBytes = kOwnQuotaBytes;
    options.eventGate = gate_.get();
    // What a client needs before it can say anything: the card that hands out the
    // sealing key, and the key itself, which its tunnel hello is sealed to.
    options.serverCardDer = card_;
    options.sealingKey = sealing_;
    // An account call arrives inside the same tunnel as everything else and is
    // passed on to the node under this server's own signature, naming whose call
    // it is - exactly as a deployed pair does it, only without leaving the process.
    options.identity = identity_;
    options.serviceNodeUrl = "http://" + host_ + ":" + std::to_string(nodePort_);
    // The account asks its server for the slice of netDb a router with no peers
    // starts from. Here that is this same process's router, which bootstraps from
    // the public hosts because there is no other server to ask.
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
    // Rotating the key this account is served under is the account's own to ask
    // for, and the destinations hold it: without these the server answers that it
    // cannot, which is true of a server that was never given them.
    options.prepareServingKey
        = [this](const std::string& user) { return destinations_->prepareServingKey(user); };
    options.commitServingKey
        = [this](const std::string& user) { return destinations_->commitServingKey(user); };
    api_ = std::make_unique<server::HttpApi>(*storage_, options);
    mailboxPort_ = api_->bind(host_, 0);
    if (mailboxPort_ <= 0) {
        throw std::runtime_error("the mailbox cannot take a port on " + host_);
    }

    facade::FacadeOptions facadeOptions;
    facadeOptions.messagingHost = host_;
    facadeOptions.messagingPort = mailboxPort_;
    facadeOptions.version = kVersion;
    facade_ = std::make_unique<facade::Facade>(facadeOptions);
    facadePort_ = facade_->bind(host_, 0);
    if (facadePort_ <= 0) {
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

    // Each engine serves on threads of its own once it is bound; nothing here has
    // to park to keep it answering.
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
        // Its own master, and it keeps it: the address is derived from this key,
        // so it is what makes the address survive a restart.
        blob = toBase64(bazarish::i2p::Keys::generate().blob());
        writePrivateFile(keyFile, blob);
    }
    // The server side publishes the address blinded, which it can only do from the
    // per-day keys a delegation carries - so even here, where both halves are the
    // same party, what it is handed is a delegation and not the master.
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
