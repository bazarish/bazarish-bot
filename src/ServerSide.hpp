// Bazarish project (c) 2026
#pragma once

#include <filesystem>

#include <DeliveryEngine.hpp>
#include <DiskStorage.hpp>
#include <EventGate.hpp>
#include <HttpApi.hpp>
#include <PerUserDestinations.hpp>

#include <bazarish/Crypto.hpp>
#include <bazarish/I2p.hpp>

#include <memory>
#include <string>
#include <thread>

namespace bazarish::daemon {

// The mailbox this daemon keeps for itself: the same server the desktop client
// talks to over I2P, running in this process and answering on loopback.
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

    // Where the account side reaches this mailbox.
    const std::string& host() const { return host_; }
    int port() const { return port_; }

    // The server's own identity, which the account subscribes to.
    const std::string& fingerprint() const { return fingerprint_; }
    // The card the account needs to seal deliveries to this mailbox.
    Bytes card() const;

    // Brings up (or renews) the destination this daemon federates on, from a key
    // it owns and keeps. Returns the routing host once it is up, empty while it
    // is still building.
    std::string serveDestinationFor(const std::string& accountFingerprint);
    std::string destination() const;

private:
    std::filesystem::path stateDir_;
    i2p::Privacy privacy_ = i2p::Privacy::eMiddle;
    i2p::Router* router_ = nullptr;
    std::string host_;
    int port_ = 0;

    Identity identity_;
    Key sealing_;
    std::string fingerprint_;

    std::unique_ptr<server::DiskStorage> storage_;
    std::unique_ptr<server::EventGate> gate_;
    std::unique_ptr<server::DeliveryEngine> engine_;
    std::unique_ptr<server::PerUserDestinations> destinations_;
    std::unique_ptr<server::HttpApi> api_;
    std::string account_;
    std::thread serving_;
};

}  // namespace bazarish::daemon
