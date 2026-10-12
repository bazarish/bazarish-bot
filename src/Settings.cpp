// Bazarish project (c) 2026
#include "Settings.hpp"

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

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

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

std::string accountKindName(const Settings::AccountKind kind)
{
    return kind == Settings::AccountKind::eGroup ? "group" : "bot";
}

std::optional<Settings::AccountKind> accountKindFromName(const std::string& name)
{
    if (name == "bot") {
        return Settings::AccountKind::eBot;
    }
    if (name == "group") {
        return Settings::AccountKind::eGroup;
    }
    return std::nullopt;
}

Settings::Settings(fs::path file)
    : file_(std::move(file))
{
    load();
}

void Settings::load()
{
    std::ifstream in(file_, std::ios::binary);
    if (!in) {
        return;
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
    for (const nlohmann::json& entry : document.value("tokens", nlohmann::json::array())) {
        ApiToken token;
        token.account = entry.value("account", std::string());
        token.name = entry.value("name", std::string());
        token.secret = entry.value("secret", std::string());
        token.createdAt = entry.value("createdAt", std::int64_t{0});
        if (!token.account.empty() && !token.name.empty() && !token.secret.empty()) {
            tokens_.push_back(std::move(token));
        }
    }
    const nlohmann::json accounts = document.value("accounts", nlohmann::json::object());
    for (const auto& [account, entry] : accounts.items()) {
        const auto kind = accountKindFromName(entry.value("kind", std::string()));
        if (!kind) {
            throw std::runtime_error("settings: account " + account + " has no kind");
        }
        accounts_[account] = {entry.value("fingerprint", std::string()), *kind};
    }
}

void Settings::save() const
{
    nlohmann::json tokens = nlohmann::json::array();
    for (const ApiToken& token : tokens_) {
        tokens.push_back({{"account", token.account}, {"name", token.name},
            {"secret", token.secret}, {"createdAt", token.createdAt}});
    }
    nlohmann::json accounts = nlohmann::json::object();
    for (const auto& [account, entry] : accounts_) {
        accounts[account]
            = {{"fingerprint", entry.fingerprint}, {"kind", accountKindName(entry.kind)}};
    }
    const nlohmann::json document = {
        {"transport", transportName(transport_)},
        {"samHost", samHost_},
        {"samPort", samPort_},
        {"privacy", privacyName(privacy_)},
        {"tokens", tokens},
        {"accounts", accounts},
    };
    const std::string text = document.dump(2);
    fs::create_directories(file_.parent_path());
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

std::vector<ApiToken> Settings::tokens(const std::string& account) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ApiToken> held;
    for (const ApiToken& token : tokens_) {
        if (token.account == account) {
            held.push_back(token);
        }
    }
    return held;
}

ApiToken Settings::issueToken(const std::string& account, const std::string& name)
{
    Bytes secret(kTokenBytes);
    if (RAND_bytes(secret.data(), static_cast<int>(secret.size())) != 1) {
        throw std::runtime_error("token: no randomness available");
    }
    ApiToken token;
    token.account = account;
    token.name = name;
    token.secret = toHex(secret);
    token.createdAt = nowSeconds();

    const std::lock_guard<std::mutex> lock(mutex_);
    const auto same = std::find_if(tokens_.begin(), tokens_.end(),
        [&account, &name](const ApiToken& held) {
            return held.account == account && held.name == name;
        });
    if (same != tokens_.end()) {
        throw std::runtime_error("a token named \"" + name + "\" is already issued");
    }
    tokens_.push_back(token);
    save();
    return token;
}

bool Settings::revokeToken(const std::string& account, const std::string& name)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(tokens_.begin(), tokens_.end(),
        [&account, &name](const ApiToken& held) {
            return held.account == account && held.name == name;
        });
    if (found == tokens_.end()) {
        return false;
    }
    tokens_.erase(found);
    save();
    return true;
}

std::optional<ApiToken> Settings::tokenFor(const std::string& secret) const
{
    if (secret.empty()) {
        return std::nullopt;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const ApiToken& token : tokens_) {
        if (token.secret.size() == secret.size()
            && CRYPTO_memcmp(token.secret.data(), secret.data(), secret.size()) == 0) {
            return token;
        }
    }
    return std::nullopt;
}

void Settings::rememberAccount(
    const std::string& account, const std::string& fingerprint, const AccountKind kind)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    accounts_[account] = {fingerprint, kind};
    save();
}

std::string Settings::accountWithFingerprint(const std::string& fingerprint) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [account, entry] : accounts_) {
        if (entry.fingerprint == fingerprint) {
            return account;
        }
    }
    return {};
}

std::string Settings::fingerprintOf(const std::string& account) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = accounts_.find(account);
    return found == accounts_.end() ? std::string() : found->second.fingerprint;
}

Settings::AccountKind Settings::kind(const std::string& account) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = accounts_.find(account);
    return found == accounts_.end() ? AccountKind::eBot : found->second.kind;
}

void Settings::setKind(const std::string& account, const AccountKind kind)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = accounts_.find(account);
    if (found == accounts_.end()) {
        throw std::runtime_error("no account " + account + " to set the kind of");
    }
    found->second.kind = kind;
    save();
}

void Settings::forgetAccount(const std::string& account)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    accounts_.erase(account);
    tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(),
                      [&account](const ApiToken& held) { return held.account == account; }),
        tokens_.end());
    save();
}

}  // namespace bazarish::bot
