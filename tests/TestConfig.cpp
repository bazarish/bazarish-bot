// Bazarish project (c) 2026
#include "Config.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

#define CHECK_THROWS(expression)                                                     \
    do {                                                                             \
        bool thrown = false;                                                         \
        try {                                                                        \
            (void)(expression);                                                      \
        } catch (const std::exception&) {                                            \
            thrown = true;                                                           \
        }                                                                            \
        if (!thrown) {                                                                \
            std::fprintf(stderr, "CHECK_THROWS failed at %s:%d: %s did not throw\n", \
                __FILE__, __LINE__, #expression);                                    \
            std::exit(1);                                                            \
        }                                                                            \
    } while (false)

namespace fs = std::filesystem;
using bazarish::bot::readConfig;

namespace {

fs::path write(const std::string& name, const std::string& text)
{
    const fs::path path = fs::temp_directory_path() / name;
    std::ofstream out(path);
    out << text;
    return path;
}

}  // namespace

int main()
{
    {
        const fs::path path = write("bz-bot-full.json", R"({
            // where everything lives
            "state": "/var/lib/bazarish/bot",
            "listen": {"host": "0.0.0.0", "port": 9000},
            "logLevel": "debug",
            "operator": "operatorfingerprint",
            "consumer": {"name": "Weather bots", "place": ["https://bots.example"], "role": "Owner"}
        })");
        const auto config = readConfig(path);
        CHECK(config.stateDir == "/var/lib/bazarish/bot");
        CHECK(config.listenHost == "0.0.0.0");
        CHECK(config.listenPort == 9000);
        CHECK(config.logLevel == "debug");
        CHECK(config.path == path);
        CHECK(config.operatorFingerprint == "operatorfingerprint");
        CHECK(config.consumer.name == "Weather bots");
        CHECK(config.consumer.place == std::vector<std::string>{"https://bots.example"});
        CHECK(config.consumer.role == "Owner");
        fs::remove(path);
    }

    {
        const fs::path path = write("bz-bot-min.json", R"({
            "state": "/tmp/bz"
        })");
        const auto config = readConfig(path);
        CHECK(config.listenHost == "127.0.0.1");
        CHECK(config.listenPort == 8440);
        CHECK(config.logLevel == "info");
        CHECK(config.operatorFingerprint.empty());
        CHECK(config.consumer.name == "Bazarish bot");
        CHECK(config.consumer.place == std::vector<std::string>{"http://127.0.0.1:8440"});
        CHECK(config.consumer.role == "Operator");
        fs::remove(path);
    }

    {
        const fs::path noState = write("bz-bot-nostate.json", R"({
            "listen": {"host": "127.0.0.1", "port": 8440}
        })");
        CHECK_THROWS(readConfig(noState));
        fs::remove(noState);

        const fs::path noPort = write("bz-bot-noport.json", R"({
            "state": "/tmp/bz",
            "listen": {"host": "127.0.0.1", "port": 0}
        })");
        CHECK_THROWS(readConfig(noPort));
        fs::remove(noPort);
    }

    {
        const auto shipped = readConfig(fs::path(BAZARISH_BOT_SOURCE_DIR) / "config.example.json");
        CHECK(shipped.stateDir == "/var/lib/bazarish/bot");
        CHECK(shipped.listenPort == 8440);
        CHECK(shipped.operatorFingerprint.empty());
    }

    CHECK_THROWS(readConfig(fs::temp_directory_path() / "bz-bot-missing.json"));

    std::fprintf(stderr, "TestConfig passed\n");
    return 0;
}
