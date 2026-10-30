// Bazarish project (c) 2026
#include "Backup.hpp"
#include "Config.hpp"
#include "HttpFace.hpp"
#include "Runtime.hpp"

#include <BuildVersion.hpp>
#include <bazarish/Log.hpp>

#include <Restart.hpp>

#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

constexpr const char* kConfigFlag = "--config";
constexpr const char* kWebDirectory = "web";

std::mutex g_stopMutex;
std::condition_variable g_stop;
bool g_stopping = false;
bool g_restoring = false;

void stopWhenAsked()
{
    const std::lock_guard<std::mutex> lock(g_stopMutex);
    g_stopping = true;
    g_stop.notify_all();
}

// Signals land on one sigwait thread: a handler may not touch a mutex or a condition variable.
sigset_t stopSignals()
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &set, nullptr) != 0) {
        throw std::runtime_error("the stop signals could not be blocked");
    }
    return set;
}

void usage()
{
    std::printf("bazarish-bot %s\n\n"
                "Bazarish accounts that run as a service: bots and groups on one I2P\n"
                "router, a REST API for automation and a web interface for the operator.\n"
                "Everything else is set in that interface.\n\n"
                "Usage:\n"
                "  bazarish-bot --config <path>\n",
        bazarish::kBuildVersion);
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

    bazarish::log::setComponent("bot");
    const bazarish::bot::Config config = bazarish::bot::readConfig(configPath);
    const auto level = bazarish::log::levelFromString(config.logLevel);
    if (!level) {
        throw std::runtime_error("config: \"logLevel\" is not a level: " + config.logLevel);
    }
    bazarish::log::setLevel(*level);

    const sigset_t signals = stopSignals();
    std::thread([signals]() {
        int caught = 0;
        if (sigwait(&signals, &caught) == 0) {
            stopWhenAsked();
        }
    }).detach();
    {
        bazarish::bot::Runtime runtime(config);
        runtime.openAll();

        bazarish::bot::HttpFace face(runtime, webDirectory(argv[0]));
        face.onRestoreReady([]() {
            {
                const std::lock_guard<std::mutex> lock(g_stopMutex);
                g_restoring = true;
            }
            stopWhenAsked();
        });
        const int port = face.start();
        bazarish::log::info(
            "bazarish-bot {} on http://{}:{}", bazarish::kBuildVersion, config.listenHost, port);
        if (config.operatorFingerprint.empty()) {
            bazarish::log::info(
                "open that address: the first key to sign in there operates this host");
        }

        {
            std::unique_lock<std::mutex> lock(g_stopMutex);
            g_stop.wait(lock, []() { return g_stopping; });
        }
        bazarish::log::info("stopping");
        face.stop();
        runtime.stop();
    }
    if (!g_restoring) {
        return 0;
    }
    const std::filesystem::path upload = config.stateDir / bazarish::bot::kRestoreUpload;
    std::ifstream in(upload, std::ios::binary);
    const bazarish::Bytes archive(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    bazarish::bot::restoreBackup(archive, config.stateDir);
    std::filesystem::remove(upload);
    bazarish::log::info("the backup is restored; exiting to be started again");
    return bazarish::kRestartExitCode;
}
