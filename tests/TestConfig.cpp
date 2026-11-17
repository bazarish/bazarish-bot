// Bazarish project (c) 2026
#include "Config.hpp"
#include "TestUtil.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using bazarish::bot::readConfig;

namespace {

fs::path write(
    const testutil::ScratchDir& scratch, const std::string& name, const std::string& text)
{
    const fs::path path = scratch.path() / name;
    std::ofstream out(path);
    out << text;
    return path;
}

}  // namespace

int main()
try {
    const testutil::ScratchDir scratch("bot-config");
    {
        const fs::path path = write(scratch, "full.json", R"({
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
    }

    {
        const fs::path path = write(scratch, "min.json", R"({
            "state": "/tmp/bz"
        })");
        const auto config = readConfig(path);
        CHECK(config.listenHost == "127.0.0.1");
        CHECK(config.listenPort == 8450);
        CHECK(config.logLevel == "info");
        CHECK(config.operatorFingerprint.empty());
        CHECK(config.consumer.name == "Bazarish bot");
        CHECK(config.consumer.place == std::vector<std::string>{"http://127.0.0.1:8450"});
        CHECK(config.consumer.role == "Operator");
    }

    {
        const fs::path noState = write(scratch, "nostate.json", R"({
            "listen": {"host": "127.0.0.1", "port": 8450}
        })");
        CHECK_THROWS(readConfig(noState));

        const fs::path noPort = write(scratch, "noport.json", R"({
            "state": "/tmp/bz",
            "listen": {"host": "127.0.0.1", "port": 0}
        })");
        CHECK_THROWS(readConfig(noPort));
    }

    {
        const auto shipped = readConfig(fs::path(BAZARISH_BOT_SOURCE_DIR) / "config.example.json");
        CHECK(shipped.stateDir == "/var/lib/bazarish/bot");
        CHECK(shipped.listenPort == 8450);
        CHECK(shipped.operatorFingerprint.empty());
    }

    CHECK_THROWS(readConfig(scratch.path() / "missing.json"));

    std::fprintf(stderr, "TestConfig passed\n");
    return 0;
} catch (...) {
    return testutil::failed();
}
