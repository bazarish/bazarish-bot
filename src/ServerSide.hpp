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
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace bazarish::bot {

class ServerSide {
public:
    ServerSide(std::filesystem::path stateDir, i2p::Privacy privacy, i2p::Router* router);
    ~ServerSide();

    ServerSide(const ServerSide&) = delete;
    ServerSide& operator=(const ServerSide&) = delete;

    void start();
    void stop();

    const std::string& host() const { return host_; }
    int port() const { return facadePort_; }

    const std::string& fingerprint() const { return fingerprint_; }
    Bytes card() const;

    std::string serveDestinationFor(const std::string& account);
    bool destinationNeedsRenewal(const std::string& account, std::int64_t now) const;
    std::string replaceDestinationFor(const std::string& account);
    std::string destination(const std::string& account) const;
    void forget(const std::string& account);
    void endMailWait(const std::string& account);

private:
    std::filesystem::path stateDir_;
    i2p::Privacy privacy_ = i2p::Privacy::eMiddle;
    i2p::Router* router_ = nullptr;
    std::string host_;
    int mailboxPort_ = 0;
    int nodePort_ = 0;
    int facadePort_ = 0;

    std::shared_ptr<const Identity> identity_;
    std::shared_ptr<Key> sealing_;
    std::string fingerprint_;
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

    std::filesystem::path keyFileFor(const std::string& account) const;

    mutable std::mutex expiresMutex_;
    std::map<std::string, std::int64_t> destinationExpires_;
};

}  // namespace bazarish::bot
