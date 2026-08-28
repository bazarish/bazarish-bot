// Bazarish project (c) 2026
#include "Config.hpp"

#include <bazarish/Log.hpp>

#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace {

constexpr const char* kVersion = "0.0.1";
constexpr const char* kConfigFlag = "--config";

void usage()
{
    std::printf("bazarish-daemon %s\n\n"
                "A Bazarish account that runs as a service and answers a REST API.\n\n"
                "Usage:\n"
                "  bazarish-daemon --config <path>\n",
        kVersion);
}

}  // namespace

int main(int argc, char** argv)
{
    std::string configPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], kConfigFlag) == 0 && i + 1 < argc) {
            configPath = argv[++i];
        } else {
            usage();
            return 1;
        }
    }
    if (configPath.empty()) {
        usage();
        return 1;
    }

    bazarish::log::setComponent("daemon");
    const bazarish::daemon::Config config = bazarish::daemon::readConfig(configPath);
    if (const auto level = bazarish::log::levelFromString(config.logLevel)) {
        bazarish::log::setLevel(*level);
    } else {
        throw std::runtime_error("config: \"logLevel\" is not a level: " + config.logLevel);
    }

    bazarish::log::info("state {}, api {}:{}, i2p {}", config.stateDir.string(),
        config.apiHost, config.apiPort,
        config.useSam ? "sam " + config.samHost + ":" + std::to_string(config.samPort)
                      : std::string("own router"));
    return 0;
}
