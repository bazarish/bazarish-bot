// Bazarish project (c) 2026
#include "Account.hpp"
#include "Settings.hpp"
#include "TestUtil.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Crypto.hpp>

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <stop_token>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using bazarish::bot::Account;
using bazarish::bot::Settings;

namespace {

constexpr std::chrono::seconds kBringUpPatience{30};
constexpr std::int64_t kWithinKeptBound = std::numeric_limits<int>::max();
constexpr std::int64_t kPastKeptBound = kWithinKeptBound + 1;

bool trimSays(const std::function<void()>& trim, const std::string& words)
{
    try {
        trim();
    } catch (const std::exception& error) {
        return std::string(error.what()).find(words) != std::string::npos;
    }
    return false;
}

void checkFailedBringUpIsTold(const fs::path& root, Settings& settings)
{
    Account broken("broken", root / "broken.db", settings, {});
    broken.open();
    std::stop_source stop;
    std::future<std::string> outcome = std::async(std::launch::async, [&broken, &stop]() {
        try {
            broken.awaitRunning(stop.get_token());
            return std::string("came up");
        } catch (const std::exception& error) {
            return std::string(error.what());
        }
    });
    const bool told = outcome.wait_for(kBringUpPatience) == std::future_status::ready;
    stop.request_stop();
    const std::string cause = outcome.get();
    CHECK(told);
    CHECK(cause.find("did not come up") != std::string::npos);
    CHECK(broken.status().starts_with("failed: "));
    broken.close();
}

}  // namespace

int main()
try {
    const testutil::ScratchDir scratch("bot-account");
    const fs::path& root = scratch.path();
    Settings settings(root / "settings.json");
    Account never("never", root / "never.db", settings, {});

    CHECK_THROWS(never.setMenu(std::string(52, 'p'), {}));
    CHECK(trimSays([&never]() { never.trimChat("peer", -1); }, "from 0 to"));
    CHECK(trimSays([&never]() { never.trimEveryChat(kPastKeptBound); }, "from 0 to"));
    CHECK(trimSays([&never]() { never.trimChat("peer", 0); }, "not open"));
    CHECK(trimSays([&never]() { never.trimEveryChat(kWithinKeptBound); }, "not open"));

    std::thread other([&never]() { never.close(); });
    never.close();
    other.join();
    never.close();
    CHECK(!never.running());

    checkFailedBringUpIsTold(root, settings);

    std::printf("TestBotAccount ok\n");
    return 0;
} catch (...) {
    return testutil::failed();
}
