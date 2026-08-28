// Bazarish project (c) 2026
#include "Config.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>

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
using bazarish::daemon::readConfig;

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
    // A full document, comments and all.
    {
        const fs::path path = write("bz-daemon-full.json", R"({
            // where everything lives
            "state": "/var/lib/bazarish/daemon",
            "api": {"listen": {"host": "127.0.0.1", "port": 8440}, "token": "s3cret"},
            "i2p": {"sam": {"host": "127.0.0.1", "port": 7656}, "privacy": "minimal"},
            "logLevel": "debug"
        })");
        const auto config = readConfig(path);
        CHECK(config.stateDir == "/var/lib/bazarish/daemon");
        CHECK(config.apiHost == "127.0.0.1");
        CHECK(config.apiPort == 8440);
        CHECK(config.apiToken == "s3cret");
        CHECK(config.useSam);
        CHECK(config.samPort == 7656);
        CHECK(config.privacy == bazarish::i2p::Privacy::eMinimal);
        CHECK(config.logLevel == "debug");
        fs::remove(path);
    }

    // Without a SAM address the daemon runs its own router, and the listener
    // defaults to loopback.
    {
        const fs::path path = write("bz-daemon-own.json", R"({
            "state": "/tmp/bz",
            "api": {"token": "t"}
        })");
        const auto config = readConfig(path);
        CHECK(!config.useSam);
        CHECK(config.apiHost == "127.0.0.1");
        CHECK(config.apiPort == 8440);
        CHECK(config.privacy == bazarish::i2p::Privacy::eMiddle);
        CHECK(config.logLevel == "info");
        fs::remove(path);
    }

    // What the daemon refuses to start without: somewhere to keep its account,
    // and something that closes the API.
    {
        const fs::path noState = write("bz-daemon-nostate.json", R"({
            "api": {"token": "t"}
        })");
        CHECK_THROWS(readConfig(noState));
        fs::remove(noState);

        const fs::path noToken = write("bz-daemon-notoken.json", R"({
            "state": "/tmp/bz"
        })");
        CHECK_THROWS(readConfig(noToken));
        fs::remove(noToken);

        const fs::path badPrivacy = write("bz-daemon-badprivacy.json", R"({
            "state": "/tmp/bz",
            "api": {"token": "t"},
            "i2p": {"privacy": "paranoid"}
        })");
        CHECK_THROWS(readConfig(badPrivacy));
        fs::remove(badPrivacy);
    }

    // A file that is not there is not an empty config.
    CHECK_THROWS(readConfig(fs::temp_directory_path() / "bz-daemon-missing.json"));

    std::fprintf(stderr, "TestConfig passed\n");
    return 0;
}
