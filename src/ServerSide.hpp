// Bazarish project (c) 2026
#pragma once

#include <filesystem>

#include <AccountApi.hpp>
#include <DeliveryEngine.hpp>
#include <DiskStorage.hpp>
#include <EventGate.hpp>
#include <Facade.hpp>
#include <GrantStore.hpp>
#include <HttpApi.hpp>
#include <NodeSettings.hpp>
#include <PerUserDestinations.hpp>
#include <SubscriptionStore.hpp>

#include <bazarish/Crypto.hpp>
#include <bazarish/I2p.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace bazarish::daemon {

// The server this daemon keeps for itself: the same three components the desktop
// client talks to over I2P - the mailbox, the account node and the facade in
// front of them - running in this process and answering on loopback.
//
// It serves exactly one account, which is also the operator - but the address is
// still delegated rather than handed over whole: the per-day blinded keys in a
// delegation are what let the mailbox publish an encrypted leaseset, and it
// refuses one that carries none. So this side keeps the master and issues itself
// a term from it, re-issuing before it runs out.
class ServerSide {
public:
    ServerSide(std::filesystem::path stateDir, i2p::Privacy privacy, i2p::Router* router);
    ~ServerSide();

    ServerSide(const ServerSide&) = delete;
    ServerSide& operator=(const ServerSide&) = delete;

    // Starts listening. Throws when the port cannot be taken: a mailbox nobody
    // can reach is not a mailbox.
    void start();
    void stop();

    // Where the account side reaches this server: the facade, as any client
    // reaches any server, only without leaving the process.
    const std::string& host() const { return host_; }
    int port() const { return facadePort_; }

    // The server's own identity, which the account subscribes to.
    const std::string& fingerprint() const { return fingerprint_; }
    // The card the account needs to seal deliveries to this mailbox.
    Bytes card() const;

    // Brings up (or renews) the destination this daemon federates on, from the
    // master key it owns and keeps. Returns the routing host once it is up, empty
    // while it is still building.
    std::string serveDestinationFor(const std::string& accountFingerprint);
    // Whether the delegation the server side is operating this address under is
    // past half its term. The daemon owns both halves, so renewing is re-issuing
    // from the master it already has - but the address still has to be delegated,
    // and a delegation still runs out.
    bool destinationNeedsRenewal(std::int64_t now) const;
    // Throws the destination key away and raises a fresh one: a new address for
    // the same account. The old address stops answering at once.
    std::string replaceDestinationFor(const std::string& accountFingerprint);
    std::string destination() const;

private:
    std::filesystem::path stateDir_;
    i2p::Privacy privacy_ = i2p::Privacy::eMiddle;
    i2p::Router* router_ = nullptr;
    std::string host_;
    int mailboxPort_ = 0;
    int nodePort_ = 0;
    int facadePort_ = 0;

    // One object each, shared with everything that needs them: a key parsed again
    // from its own PEM is a second copy of a secret.
    std::shared_ptr<const Identity> identity_;
    std::shared_ptr<Key> sealing_;
    std::string fingerprint_;
    // The card both the mailbox and the account node hand out.
    Bytes card_;

    std::unique_ptr<server::DiskStorage> storage_;
    std::unique_ptr<server::EventGate> gate_;
    std::unique_ptr<server::DeliveryEngine> engine_;
    std::unique_ptr<server::PerUserDestinations> destinations_;
    std::unique_ptr<server::HttpApi> api_;

    std::unique_ptr<service::SubscriptionStore> subscriptions_;
    std::unique_ptr<service::GrantStore> grants_;
    std::shared_ptr<service::NodeSettings> nodeSettings_;
    std::unique_ptr<service::AccountApi> accounts_;
    std::unique_ptr<facade::Facade> facade_;

    std::string account_;
    // When the delegation this address is served under runs out.
    std::int64_t destinationExpires_ = 0;
};

}  // namespace bazarish::daemon
