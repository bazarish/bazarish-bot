// Bazarish project (c) 2026
#include "Settings.hpp"

#include "Password.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Log.hpp>

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace bazarish::daemon {

namespace fs = std::filesystem;

namespace {

// The SAM port i2pd serves by default.
constexpr int kDefaultSamPort = 7656;
constexpr const char* kDefaultSamHost = "127.0.0.1";
// A token is read by machines and pasted by people: 32 bytes of randomness, hex.
constexpr std::size_t kTokenBytes = 32;

std::string transportName(const Settings::Transport transport)
{
    return transport == Settings::Transport::eSam ? "sam" : "embedded";
}

std::int64_t nowSeconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

}  // namespace

Settings::Settings(fs::path file)
    : file_(std::move(file))
{
    load();
}

void Settings::load()
{
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        return;  // nothing set yet: the defaults above are the answer
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    if (text.empty()) {
        return;
    }
    const nlohmann::json document = nlohmann::json::parse(text);
    if (document.value("transport", std::string("embedded")) == "sam") {
        transport_ = Transport::eSam;
    }
    samHost_ = document.value("samHost", std::string(kDefaultSamHost));
    samPort_ = document.value("samPort", kDefaultSamPort);
    if (const auto parsed = i2p::privacyFromString(document.value("privacy", "middle"))) {
        privacy_ = *parsed;
    }
    passwordSalt_ = document.value("passwordSalt", std::string());
    passwordHash_ = document.value("passwordHash", std::string());
    for (const nlohmann::json& entry : document.value("tokens", nlohmann::json::array())) {
        ApiToken token;
        token.name = entry.value("name", std::string());
        token.secret = entry.value("secret", std::string());
        token.createdAt = entry.value("createdAt", std::int64_t{0});
        if (!token.name.empty() && !token.secret.empty()) {
            tokens_.push_back(std::move(token));
        }
    }
}

void Settings::save() const
{
    nlohmann::json tokens = nlohmann::json::array();
    for (const ApiToken& token : tokens_) {
        tokens.push_back({{"name", token.name}, {"secret", token.secret},
            {"createdAt", token.createdAt}});
    }
    const nlohmann::json document = {
        {"transport", transportName(transport_)},
        {"samHost", samHost_},
        {"samPort", samPort_},
        {"privacy", privacyName(privacy_)},
        {"passwordSalt", passwordSalt_},
        {"passwordHash", passwordHash_},
        {"tokens", tokens},
    };
    const std::string text = document.dump(2);
    fs::create_directories(file_.parent_path());
    // Written through a neighbour and renamed: a settings file torn in half is a
    // daemon that does not start, and this is written on every change.
    const fs::path scratch = fs::path(file_).concat(".new");
    {
        std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write " + scratch.string());
        }
    }
    fs::rename(scratch, file_);
    fs::permissions(file_, fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
}

Settings::Transport Settings::transport() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return transport_;
}

std::string Settings::samHost() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return samHost_.empty() ? kDefaultSamHost : samHost_;
}

int Settings::samPort() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return samPort_ == 0 ? kDefaultSamPort : samPort_;
}

i2p::Privacy Settings::privacy() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return privacy_;
}

void Settings::setTransport(const Transport transport, std::string samHost, const int samPort)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    transport_ = transport;
    samHost_ = std::move(samHost);
    samPort_ = samPort;
    save();
}

void Settings::setPrivacy(const i2p::Privacy privacy)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    privacy_ = privacy;
    save();
}

bool Settings::hasPassword() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return !passwordHash_.empty();
}

void Settings::setPassword(const std::string& password)
{
    const PasswordRecord record = hashPassword(password);
    const std::lock_guard<std::mutex> lock(mutex_);
    passwordSalt_ = record.salt;
    passwordHash_ = record.hash;
    save();
}

bool Settings::passwordMatches(const std::string& password) const
{
    PasswordRecord record;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        record = {passwordSalt_, passwordHash_};
    }
    return daemon::passwordMatches(record, password);
}

std::vector<ApiToken> Settings::tokens() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return tokens_;
}

ApiToken Settings::issueToken(const std::string& name)
{
    Bytes secret(kTokenBytes);
    if (RAND_bytes(secret.data(), static_cast<int>(secret.size())) != 1) {
        throw std::runtime_error("token: no randomness available");
    }
    ApiToken token;
    token.name = name;
    token.secret = toHex(secret);
    token.createdAt = nowSeconds();

    const std::lock_guard<std::mutex> lock(mutex_);
    const auto same = std::find_if(tokens_.begin(), tokens_.end(),
        [&name](const ApiToken& held) { return held.name == name; });
    if (same != tokens_.end()) {
        throw std::runtime_error("a token named \"" + name + "\" is already issued");
    }
    tokens_.push_back(token);
    save();
    return token;
}

bool Settings::revokeToken(const std::string& name)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(tokens_.begin(), tokens_.end(),
        [&name](const ApiToken& held) { return held.name == name; });
    if (found == tokens_.end()) {
        return false;
    }
    tokens_.erase(found);
    save();
    return true;
}

std::string Settings::tokenNameFor(const std::string& secret) const
{
    if (secret.empty()) {
        return {};
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const ApiToken& token : tokens_) {
        if (token.secret.size() == secret.size()
            && CRYPTO_memcmp(token.secret.data(), secret.data(), secret.size()) == 0) {
            return token.name;
        }
    }
    return {};
}

}  // namespace bazarish::daemon
