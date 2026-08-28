// Bazarish project (c) 2026
#include "Config.hpp"

#include <bazarish/ConfigFile.hpp>

#include <stdexcept>

namespace bazarish::daemon {

namespace {

// Loopback: what is served here is the account itself, and a default that
// reaches the network is a default nobody chose.
constexpr const char* kDefaultListenHost = "127.0.0.1";
constexpr int kDefaultListenPort = 8440;
constexpr const char* kDefaultLogLevel = "info";

}  // namespace

Config readConfig(const std::filesystem::path& path)
{
    const ConfigView view(ConfigFile(path).read());

    Config config;
    config.stateDir = view.text("state");
    if (config.stateDir.empty()) {
        throw std::runtime_error("config: \"state\" names where everything is kept");
    }

    const auto listen = view.endpoint("listen");
    config.listenHost = listen ? listen->host : kDefaultListenHost;
    config.listenPort = listen ? listen->port : kDefaultListenPort;
    if (config.listenPort <= 0) {
        throw std::runtime_error("config: \"listen.port\" is a port to answer on");
    }

    config.logLevel = view.text("logLevel", kDefaultLogLevel);
    return config;
}

}  // namespace bazarish::daemon
