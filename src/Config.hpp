// Bazarish project (c) 2026
#pragma once

#include <filesystem>
#include <string>

namespace bazarish::daemon {

struct Config {
    std::filesystem::path stateDir;
    std::string listenHost;
    int listenPort = 0;
    std::string logLevel;
};

Config readConfig(const std::filesystem::path& path);

}  // namespace bazarish::daemon
