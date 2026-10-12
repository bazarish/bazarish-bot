// Bazarish project (c) 2026
#pragma once

#include <bazarish/LoginChallenge.hpp>
#include <bazarish/Portal.hpp>

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>

namespace bazarish::bot {

class OperatorAuth {
public:
    OperatorAuth(std::filesystem::path configPath, std::string operatorFingerprint,
        service::LoginConsumer consumer);

    bool claimed() const;
    std::string operatorFingerprint() const;

    std::string issueChallenge(std::int64_t now);
    std::string signIn(const std::string& challenge, const std::string& blob, std::int64_t now);

private:
    const std::filesystem::path configPath_;
    service::LoginChallenge challenges_;
    mutable std::mutex mutex_;
    std::string operator_;
};

}  // namespace bazarish::bot
