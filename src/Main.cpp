// Bazarish project (c) 2026
#include "Config.hpp"
#include "HttpFace.hpp"
#include "Runtime.hpp"

#include <bazarish/Log.hpp>

#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>

namespace {

constexpr const char* kVersion = "0.0.1";
constexpr const char* kConfigFlag = "--config";
constexpr const char* kWebDirectory = "web";

std::mutex g_stopMutex;
std::condition_variable g_stop;
bool g_stopping = false;

void onSignal(int)
{
    const std::lock_guard<std::mutex> lock(g_stopMutex);
    g_stopping = true;
    g_stop.notify_all();
}

void usage()
{
    std::printf("bazarish-daemon %s\n\n"
                "A Bazarish account that runs as a service: a REST API for automation\n"
                "and a web interface for the operator. Everything else is set in that\n"
                "interface.\n\n"
                "Usage:\n"
                "  bazarish-daemon --config <path>\n",
        kVersion);
}

std::filesystem::path webDirectory(const char* const argv0)
{
    std::error_code failure;
    const std::filesystem::path self = std::filesystem::weakly_canonical(argv0, failure);
    if (!failure && self.has_parent_path()) {
        const std::filesystem::path beside = self.parent_path() / kWebDirectory;
        if (std::filesystem::is_directory(beside, failure)) {
            return beside;
        }
    }
    return std::filesystem::current_path() / kWebDirectory;
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
    const auto level = bazarish::log::levelFromString(config.logLevel);
    if (!level) {
        throw std::runtime_error("config: \"logLevel\" is not a level: " + config.logLevel);
    }
    bazarish::log::setLevel(*level);

    bazarish::daemon::Runtime runtime(config);
    if (runtime.hasAccount()) {
        try {
            runtime.start({});
        } catch (const std::exception& error) {
            bazarish::log::info("the account stays closed: {}", error.what());
        }
    }

    bazarish::daemon::HttpFace face(runtime, webDirectory(argv[0]));
    const int port = face.start();
    bazarish::log::info("bazarish-daemon {} on http://{}:{}", kVersion, config.listenHost, port);
    if (!runtime.settings().hasPassword()) {
        bazarish::log::info("open that address to set the password and the account");
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    {
        std::unique_lock<std::mutex> lock(g_stopMutex);
        g_stop.wait(lock, []() { return g_stopping; });
    }
    bazarish::log::info("stopping");
    face.stop();
    runtime.stop();
    return 0;
}
