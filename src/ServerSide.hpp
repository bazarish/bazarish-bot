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
#include <OperatorPush.hpp>
#include <PerUserDestinations.hpp>
#include <SubscriptionStore.hpp>

#include <bazarish/Crypto.hpp>
#include <bazarish/I2p.hpp>

#include <memory>
#include <string>
#include <thread>

namespace bazarish::daemon {

// The server this daemon keeps for itself: the same three components the desktop
// client talks to over I2P - the mailbox, the account node and the facade in
// front of them - running in this process and answering on loopback.
//
// It serves exactly one account, which is also the operator, so nothing here is
// delegated. On a public server the account hands over a short-lived offline
// transient and keeps its identity key; here the two are the same party, so the
// destination key is simply owned - which is also what lets the transport be
// SAM, where an offline key cannot be issued at all.
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

    // Brings up (or renews) the destination this daemon federates on, from a key
    // it owns and keeps. Returns the routing host once it is up, empty while it
    // is still building.
    std::string serveDestinationFor(const std::string& accountFingerprint);
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

    Identity identity_;
    Key sealing_;
    std::string fingerprint_;

    std::unique_ptr<server::DiskStorage> storage_;
    std::unique_ptr<server::EventGate> gate_;
    std::unique_ptr<server::DeliveryEngine> engine_;
    std::unique_ptr<server::PerUserDestinations> destinations_;
    std::unique_ptr<server::HttpApi> api_;

    std::unique_ptr<service::SubscriptionStore> subscriptions_;
    std::unique_ptr<service::GrantStore> grants_;
    std::unique_ptr<service::OperatorPush> push_;
    std::shared_ptr<service::NodeSettings> nodeSettings_;
    std::unique_ptr<service::AccountApi> accounts_;
    std::unique_ptr<facade::Facade> facade_;

    std::string account_;
    std::thread servingMailbox_;
    std::thread servingAccounts_;
    std::thread servingFacade_;
};

}  // namespace bazarish::daemon
