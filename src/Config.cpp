// Bazarish project (c) 2026
#include "Config.hpp"

#include <bazarish/ConfigFile.hpp>

#include <stdexcept>

namespace bazarish::bot {

namespace {

constexpr const char* kDefaultListenHost = "127.0.0.1";
constexpr int kDefaultListenPort = 8440;
constexpr const char* kDefaultLogLevel = "info";
constexpr const char* kDefaultConsumerName = "Bazarish bot";
constexpr const char* kDefaultConsumerRole = "Operator";

}  // namespace

Config readConfig(const std::filesystem::path& path)
{
    const ConfigView view(ConfigFile(path).read());

    Config config;
    config.path = path;
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
    config.operatorFingerprint = view.text("operator");
    config.consumer.name = view.text("consumer.name", kDefaultConsumerName);
    config.consumer.place = view.list("consumer.place");
    if (config.consumer.place.empty()) {
        config.consumer.place
            = {"http://" + config.listenHost + ":" + std::to_string(config.listenPort)};
    }
    config.consumer.role = view.text("consumer.role", kDefaultConsumerRole);
    return config;
}

}  // namespace bazarish::bot
