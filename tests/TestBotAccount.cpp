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
#include <future>
#include <stop_token>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using bazarish::bot::Account;
using bazarish::bot::Settings;

namespace {

constexpr std::size_t kUniqueBytes = 8;
constexpr std::chrono::seconds kBringUpPatience{30};

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
{
    const std::string unique = bazarish::toHex(bazarish::randomBytes(kUniqueBytes));
    const fs::path root = fs::temp_directory_path() / ("bz-bot-account-" + unique);
    fs::create_directories(root);
    Settings settings(root / "settings.json");
    Account never("never", root / "never.db", settings, {});

    CHECK_THROWS(never.setMenu(std::string(52, 'p'), {}));

    std::thread other([&never]() { never.close(); });
    never.close();
    other.join();
    never.close();
    CHECK(!never.running());

    checkFailedBringUpIsTold(root, settings);

    fs::remove_all(root);
    std::printf("TestBotAccount ok\n");
    return 0;
}
