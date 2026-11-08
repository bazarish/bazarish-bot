// Bazarish project (c) 2026
#include "Backup.hpp"

#include <bazarish/JsonRead.hpp>
#include <bazarish/PrivateFile.hpp>
#include <bazarish/Zip.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr const char* kManifest = "bazarish-bot-backup.json";
constexpr const char* kStatePrefix = "state/";
constexpr const char* kConfigPrefix = "config/";
constexpr int kFormat = 1;
constexpr const char* kRouterDirectory = "i2p";

fs::path insideState(const std::string& name)
{
    const fs::path relative = fs::path(name.substr(std::string(kStatePrefix).size()));
    if (relative.empty() || relative.is_absolute()) {
        throw std::runtime_error("backup: " + name + " names no file in the state");
    }
    for (const fs::path& part : relative) {
        if (part == ".." || part == ".") {
            throw std::runtime_error("backup: " + name + " climbs out of the state");
        }
    }
    return relative;
}

std::vector<ZipEntry> checkedEntries(const Bytes& archive)
{
    std::vector<ZipEntry> entries = unzipStored(archive);
    const auto manifest = std::find_if(entries.begin(), entries.end(),
        [](const ZipEntry& entry) { return entry.name == kManifest; });
    if (manifest == entries.end()) {
        throw std::runtime_error("backup: this is not a bot backup");
    }
    const nlohmann::json described = bazarish::parseJson(
        std::string(manifest->data.begin(), manifest->data.end()));
    if (described.at("format").get<int>() != kFormat) {
        throw std::runtime_error("backup: a format this build does not read");
    }
    for (const ZipEntry& entry : entries) {
        if (entry.name == kManifest || entry.name.starts_with(kConfigPrefix)) {
            continue;
        }
        if (!entry.name.starts_with(kStatePrefix)) {
            throw std::runtime_error("backup: " + entry.name + " belongs nowhere");
        }
        (void)insideState(entry.name);
    }
    return entries;
}

}  // namespace

Bytes exportBackup(const fs::path& stateDir, const fs::path& configFile,
    const std::string& version, const std::int64_t createdAt)
{
    std::vector<ZipEntry> entries;
    entries.push_back({kManifest, {}});
    for (auto found = fs::recursive_directory_iterator(stateDir);
         found != fs::recursive_directory_iterator(); ++found) {
        const fs::path relative = found->path().lexically_relative(stateDir);
        if (found->is_directory() && relative == kRouterDirectory) {
            found.disable_recursion_pending();
            continue;
        }
        if (!found->is_regular_file() || relative == kRestoreUpload) {
            continue;
        }
        std::error_code gone;
        const fs::perms permissions = fs::status(found->path(), gone).permissions();
        // Mail is deleted as it is acknowledged: a file that went mid-walk is not an error.
        if (gone) {
            continue;
        }
        std::ifstream in(found->path(), std::ios::binary);
        if (!in) {
            if (!fs::exists(found->path())) {
                continue;
            }
            throw std::runtime_error("backup: cannot read " + found->path().string());
        }
        entries.push_back({kStatePrefix + relative.generic_string(),
            Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()),
            static_cast<unsigned>(permissions & fs::perms::mask)});
    }
    entries.push_back({kConfigPrefix + configFile.filename().string(), readFileBytes(configFile),
        static_cast<unsigned>(fs::status(configFile).permissions() & fs::perms::mask)});
    const std::string manifest = nlohmann::json{{"format", kFormat}, {"version", version},
        {"createdAt", createdAt}, {"files", entries.size() - 1}}.dump(2);
    entries.front().data.assign(manifest.begin(), manifest.end());
    return zipStored(entries);
}

void checkBackup(const Bytes& archive)
{
    (void)checkedEntries(archive);
}

void restoreBackup(const Bytes& archive, const fs::path& stateDir)
{
    const std::vector<ZipEntry> entries = checkedEntries(archive);
    for (const fs::directory_entry& found : fs::directory_iterator(stateDir)) {
        if (found.path().filename() != kRestoreUpload) {
            fs::remove_all(found.path());
        }
    }
    for (const ZipEntry& entry : entries) {
        if (!entry.name.starts_with(kStatePrefix)) {
            continue;
        }
        const fs::path target = stateDir / insideState(entry.name);
        fs::create_directories(target.parent_path());
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        fs::permissions(target, static_cast<fs::perms>(entry.mode) & fs::perms::mask);
        out.write(reinterpret_cast<const char*>(entry.data.data()),
            static_cast<std::streamsize>(entry.data.size()));
        if (!out) {
            throw std::runtime_error("backup: cannot write " + target.string());
        }
    }
}

}  // namespace bazarish::bot
