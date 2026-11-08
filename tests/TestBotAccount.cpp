// Bazarish project (c) 2026
#include "Account.hpp"
#include "Settings.hpp"
#include "TestUtil.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Crypto.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using bazarish::bot::Account;
using bazarish::bot::Settings;

namespace {

constexpr std::size_t kUniqueBytes = 8;

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

    fs::remove_all(root);
    std::printf("TestBotAccount ok\n");
    return 0;
}
