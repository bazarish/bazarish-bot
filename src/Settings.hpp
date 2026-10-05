// Bazarish project (c) 2026
#pragma once

#include <bazarish/I2p.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace bazarish::daemon {

struct ApiToken {
    std::string name;
    std::string secret;
    std::int64_t createdAt = 0;
};

class Settings {
public:
    explicit Settings(std::filesystem::path file);

    enum class Transport { eEmbedded, eSam };

    Transport transport() const;
    std::string samHost() const;
    int samPort() const;
    i2p::Privacy privacy() const;
    void setTransport(Transport transport, std::string samHost, int samPort);
    void setPrivacy(i2p::Privacy privacy);

    bool hasPassword() const;
    void setPassword(const std::string& password);
    bool passwordMatches(const std::string& password) const;

    std::vector<ApiToken> tokens() const;
    ApiToken issueToken(const std::string& name);
    bool revokeToken(const std::string& name);
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
