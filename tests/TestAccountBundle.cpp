// Bazarish project (c) 2026
#include "Runtime.hpp"
#include "Settings.hpp"
#include "TestUtil.hpp"

#include <AccountManager.hpp>
#include <Session.hpp>

#include <bazarish/AccountKind.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using bazarish::AccountKind;
using bazarish::Bytes;
using bazarish::bot::Settings;

namespace {

constexpr char kPassword[] = "bundle password for the test";

struct Exported {
    std::string fingerprint;
    Bytes bundle;
};

Exported exportFrom(const fs::path& accounts, const std::string& name, const AccountKind kind)
{
    bazarish::client::AccountManager manager(accounts);
    const std::string id = manager.create(name, kind).id;
    const std::unique_ptr<bazarish::client::Session> session = manager.open(id);
    return Exported{session->fingerprint(), session->exportAccountBytes(kPassword)};
}

bazarish::bot::Config configIn(const fs::path& dir)
{
    bazarish::bot::Config config;
    config.path = dir / "bot.json";
    fs::create_directories(dir);
    std::ofstream(config.path) << "{}";
    config.stateDir = dir / "state";
    config.listenHost = "127.0.0.1";
    return config;
}

bazarish::bot::Runtime::Summary summaryOf(
    const bazarish::bot::Runtime& runtime, const std::string& id)
{
    for (const auto& summary : runtime.accounts()) {
        if (summary.id == id) {
            return summary;
        }
    }
    throw std::runtime_error("the runtime does not list " + id);
}

}  // namespace

int main()
try {
    const testutil::ScratchDir scratch("bot-account-bundle");
    const fs::path& dir = scratch.path();
    const Exported bot = exportFrom(dir / "source", "Helper", AccountKind::eBot);
    const Exported group = exportFrom(dir / "source", "Club", AccountKind::eChat);
    const Exported person = exportFrom(dir / "source", "Person", AccountKind::eUser);

    const bazarish::bot::Config config = configIn(dir / "host");
    {
        bazarish::bot::Runtime runtime(config);
        CHECK_THROWS(runtime.importAccount(bot.bundle, "not the password"));
        CHECK_THROWS(runtime.importAccount(person.bundle, kPassword));
        CHECK(runtime.accounts().empty());

        const std::string botId = runtime.importAccount(bot.bundle, kPassword);
        const std::string groupId = runtime.importAccount(group.bundle, kPassword);
        CHECK(runtime.settings().kind(botId) == Settings::AccountKind::eBot);
        CHECK(runtime.settings().kind(groupId) == Settings::AccountKind::eGroup);
        CHECK(summaryOf(runtime, botId).fingerprint == bot.fingerprint);
        CHECK(summaryOf(runtime, groupId).fingerprint == group.fingerprint);
        CHECK(runtime.accounts().size() == 2);
        runtime.stop();
    }

    std::fprintf(stderr, "TestAccountBundle passed\n");
    return 0;
} catch (...) {
    return testutil::failed();
}
