// Bazarish project (c) 2026
#include "Config.hpp"

#include <bazarish/ConfigFile.hpp>

#include <stdexcept>

namespace bazarish::daemon {

namespace {

// The API listens on loopback unless the operator says otherwise: it carries a
// token that opens the account, and a default that reaches the network is a
// default nobody chose.
constexpr const char* kDefaultApiHost = "127.0.0.1";
constexpr int kDefaultApiPort = 8440;
// The SAM port i2pd serves by default.
constexpr int kDefaultSamPort = 7656;
constexpr const char* kDefaultLogLevel = "info";
constexpr const char* kDefaultPrivacy = "middle";

}  // namespace

Config readConfig(const std::filesystem::path& path)
{
    const ConfigView view(ConfigFile(path).read());

    Config config;
    config.stateDir = view.text("state");
    if (config.stateDir.empty()) {
        throw std::runtime_error("config: \"state\" names where everything is kept");
    }

    const auto api = view.endpoint("api.listen");
    config.apiHost = api ? api->host : kDefaultApiHost;
    config.apiPort = api ? api->port : kDefaultApiPort;
    config.apiToken = view.text("api.token");
    if (config.apiToken.empty()) {
        throw std::runtime_error("config: \"api.token\" is what keeps the account closed");
    }

    if (view.has("i2p.sam")) {
        const auto sam = view.endpoint("i2p.sam");
        if (!sam) {
            throw std::runtime_error("config: \"i2p.sam\" is a host and a port");
        }
        config.useSam = true;
        config.samHost = sam->host;
        config.samPort = sam->port == 0 ? kDefaultSamPort : sam->port;
    }

    const std::string privacy = view.text("i2p.privacy", kDefaultPrivacy);
    const std::optional<i2p::Privacy> parsed = i2p::privacyFromString(privacy);
    if (!parsed) {
        throw std::runtime_error("config: \"i2p.privacy\" is minimal, middle or max, not \""
            + privacy + "\"");
    }
    config.privacy = *parsed;

    config.logLevel = view.text("logLevel", kDefaultLogLevel);
    return config;
}

}  // namespace bazarish::daemon
