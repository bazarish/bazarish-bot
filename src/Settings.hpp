// Bazarish project (c) 2026
#pragma once

#include <bazarish/I2p.hpp>

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace bazarish::bot {

// The SAM port i2pd serves by default.
inline constexpr int kDefaultSamPort = 7656;
inline constexpr const char* kDefaultSamHost = "127.0.0.1";

struct ApiToken {
    std::string account;
    std::string name;
    std::string secret;
    std::int64_t createdAt = 0;
};

class Settings {
public:
    explicit Settings(std::filesystem::path file);

    enum class Transport { eEmbedded, eSam };
    enum class AccountKind { eBot, eGroup };

    Transport transport() const;
    std::string samHost() const;
    int samPort() const;
    i2p::Privacy privacy() const;
    void setTransport(Transport transport, std::string samHost, int samPort);
    void setPrivacy(i2p::Privacy privacy);

    bool hasPassword() const;
    void setPassword(const std::string& password);
    bool passwordMatches(const std::string& password) const;

    std::vector<ApiToken> tokens(const std::string& account) const;
    ApiToken issueToken(const std::string& account, const std::string& name);
    bool revokeToken(const std::string& account, const std::string& name);
    std::optional<ApiToken> tokenFor(const std::string& secret) const;

    void rememberAccount(const std::string& account, const std::string& fingerprint, AccountKind kind);
    std::string accountWithFingerprint(const std::string& fingerprint) const;
    std::string fingerprintOf(const std::string& account) const;
    AccountKind kind(const std::string& account) const;
    void setKind(const std::string& account, AccountKind kind);
    void forgetAccount(const std::string& account);

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
    struct AccountEntry {
        std::string fingerprint;
        AccountKind kind = AccountKind::eBot;
    };
    std::map<std::string, AccountEntry> accounts_;
};

std::string accountKindName(Settings::AccountKind kind);
std::optional<Settings::AccountKind> accountKindFromName(const std::string& name);

}  // namespace bazarish::bot
