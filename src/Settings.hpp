// Bazarish project (c) 2026
#pragma once

#include <bazarish/I2p.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bazarish::daemon {

// A token that opens the REST API, as the operator sees it: a name they gave it
// and the day it was made, so a token can be recognised before it is revoked.
struct ApiToken {
    std::string name;
    std::string secret;
    std::int64_t createdAt = 0;
};

// Everything the operator sets after the daemon is running, which is everything
// except where it keeps its state and where it answers. Kept beside the account
// and written whole on every change: it is small, and a half-written settings
// file is a daemon that will not start.
class Settings {
public:
    explicit Settings(std::filesystem::path file);

    // The transport. Embedded runs a router in this process; SAM speaks to one
    // that is already running, which is what makes several daemons on one host
    // cheap.
    enum class Transport { eEmbedded, eSam };

    Transport transport() const;
    std::string samHost() const;
    int samPort() const;
    i2p::Privacy privacy() const;
    void setTransport(Transport transport, std::string samHost, int samPort);
    void setPrivacy(i2p::Privacy privacy);

    // Whether the operator has set the password that guards the interface. Until
    // they do, the daemon serves nothing but the page that asks for one.
    bool hasPassword() const;
    void setPassword(const std::string& password);
    bool passwordMatches(const std::string& password) const;

    std::vector<ApiToken> tokens() const;
    // Issues a token under this name and returns it. The secret is shown once,
    // here, because it is not kept anywhere it could be shown again.
    ApiToken issueToken(const std::string& name);
    // True when a token by that name was there to revoke.
    bool revokeToken(const std::string& name);
    // The name the secret belongs to, or empty when it belongs to nothing.
    std::string tokenNameFor(const std::string& secret) const;

private:
    void load();
    void save() const;

    std::filesystem::path file_;
    mutable std::mutex mutex_;

    Transport transport_ = Transport::eEmbedded;
    std::string samHost_;
    int samPort_ = 0;
    i2p::Privacy privacy_ = i2p::Privacy::eMiddle;
    std::string passwordSalt_;
    std::string passwordHash_;
    std::vector<ApiToken> tokens_;
};

}  // namespace bazarish::daemon
