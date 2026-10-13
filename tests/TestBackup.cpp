// Bazarish project (c) 2026
#include "Backup.hpp"
#include "Zip.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
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
        bool threw = false;                                                          \
        try {                                                                        \
            expression;                                                              \
        } catch (const std::exception&) {                                            \
            threw = true;                                                            \
        }                                                                            \
        if (!threw) {                                                                \
            std::fprintf(stderr, "CHECK_THROWS failed at %s:%d: %s did not throw\n", \
                __FILE__, __LINE__, #expression);                                    \
            std::exit(1);                                                            \
        }                                                                            \
    } while (false)

namespace fs = std::filesystem;
using bazarish::Bytes;
using namespace bazarish::bot;

namespace {

void write(const fs::path& file, const std::string& text, const fs::perms mode)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
    fs::permissions(file, mode);
}

std::string read(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

Bytes bytesOf(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

}  // namespace

int main()
{
    const fs::path root = fs::temp_directory_path() / "bz-bot-backup-test";
    fs::remove_all(root);
    const fs::path from = root / "from";
    const fs::path to = root / "to";
    const fs::path config = root / "bot.json";
    const fs::perms secret = fs::perms::owner_read | fs::perms::owner_write;
    const fs::perms open = secret | fs::perms::group_read | fs::perms::others_read;
    write(from / "settings.json", "{\"accounts\": {}}", secret);
    write(from / "accounts" / "a.key", std::string("\0key\xff", 5), secret);
    write(from / "i2p" / "netDb" / "r1" / "routerInfo.dat", "a router record", open);
    write(from / kRestoreUpload, "an upload in progress", secret);
    write(config, "{\"operator\": \"me\"}", secret);

    const Bytes archive = exportBackup(from, config, "0.0.1", 1780000000);
    checkBackup(archive);
    bool sawConfig = false;
    for (const ZipEntry& entry : unzipStored(archive)) {
        CHECK(entry.name.find(kRestoreUpload) == std::string::npos);
        sawConfig = sawConfig || entry.name == "config/bot.json";
    }
    CHECK(sawConfig);

    write(to / "settings.json", "{\"other\": true}", secret);
    write(to / "stale" / "left.db", "from before", secret);
    write(to / kRestoreUpload, "kept", secret);
    restoreBackup(archive, to);
    CHECK(read(to / "settings.json") == "{\"accounts\": {}}");
    CHECK(read(to / "accounts" / "a.key") == std::string("\0key\xff", 5));
    CHECK(read(to / "i2p" / "netDb" / "r1" / "routerInfo.dat") == "a router record");
    CHECK(!fs::exists(to / "stale"));
    CHECK(!fs::exists(to / "bot.json"));
    CHECK(read(to / kRestoreUpload) == "kept");
    CHECK((fs::status(to / "accounts" / "a.key").permissions() & fs::perms::mask) == secret);
    CHECK((fs::status(to / "i2p" / "netDb" / "r1" / "routerInfo.dat").permissions()
              & fs::perms::mask)
        == open);

    Bytes tampered = archive;
    const std::string marker = "a router record";
    const auto at = std::search(tampered.begin(), tampered.end(), marker.begin(), marker.end());
    CHECK(at != tampered.end());
    *at = 'R';
    CHECK_THROWS(checkBackup(tampered));
    CHECK_THROWS(checkBackup(bytesOf("not an archive at all, just text")));
    const Bytes manifest = bytesOf("{\"format\": 1}");
    CHECK_THROWS(checkBackup(zipStored({{"state/x", bytesOf("x")}})));
    CHECK_THROWS(checkBackup(zipStored({{"bazarish-bot-backup.json", bytesOf("{\"format\": 2}")}})));
    CHECK_THROWS(checkBackup(zipStored(
        {{"bazarish-bot-backup.json", manifest}, {"state/../escaped", bytesOf("x")}})));
    CHECK_THROWS(checkBackup(zipStored(
        {{"bazarish-bot-backup.json", manifest}, {"elsewhere/x", bytesOf("x")}})));
    CHECK_THROWS(checkBackup(zipStored(
        {{"bazarish-bot-backup.json", manifest}, {"state//etc/passwd", bytesOf("x")}})));
    checkBackup(zipStored({{"bazarish-bot-backup.json", manifest}, {"state/ok", bytesOf("x")}}));

    fs::remove_all(root);
    std::fprintf(stderr, "TestBackup passed\n");
    return 0;
}
