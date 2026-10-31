// Bazarish project (c) 2026
#include "OperatorAuth.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/ConfigFile.hpp>
#include <bazarish/Log.hpp>

#include <stdexcept>
#include <utility>

namespace bazarish::bot {

namespace {

constexpr std::size_t kChallengeSecretBytes = 16;
constexpr const char* kOperatorKey = "operator";

}  // namespace

OperatorAuth::OperatorAuth(std::filesystem::path configPath, std::string operatorFingerprint,
    service::LoginConsumer consumer)
    : configPath_(std::move(configPath))
    , challenges_(toHex(randomBytes(kChallengeSecretBytes)), std::move(consumer))
    , operator_(std::move(operatorFingerprint))
{
}

bool OperatorAuth::claimed() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return !operator_.empty();
}

std::string OperatorAuth::operatorFingerprint() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return operator_;
}

std::string OperatorAuth::issueChallenge(const std::int64_t now)
{
    return challenges_.issue(now);
}

std::string OperatorAuth::signIn(
    const std::string& challenge, const std::string& blob, const std::int64_t now)
{
    const std::string fingerprint = challenges_.verify(challenge, blob, now);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (operator_.empty()) {
        ConfigFile(configPath_).set({kOperatorKey}, fingerprint);
        operator_ = fingerprint;
        log::info("this bot host is now operated by {}", fingerprint);
        return fingerprint;
    }
    if (fingerprint != operator_) {
        throw std::runtime_error("that key is not this host's operator");
    }
    return fingerprint;
}

}  // namespace bazarish::bot
