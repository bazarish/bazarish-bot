// Bazarish project (c) 2026
#include "Backup.hpp"

#include "Zip.hpp"

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

Bytes readFile(const fs::path& file)
{
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("backup: cannot read " + file.string());
    }
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool startsWith(const std::string& text, const std::string& prefix)
{
    return text.compare(0, prefix.size(), prefix) == 0;
}

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
    const nlohmann::json described = nlohmann::json::parse(
        std::string(manifest->data.begin(), manifest->data.end()));
    if (described.at("format").get<int>() != kFormat) {
        throw std::runtime_error("backup: a format this build does not read");
    }
    for (const ZipEntry& entry : entries) {
        if (entry.name == kManifest || startsWith(entry.name, kConfigPrefix)) {
            continue;
        }
        if (!startsWith(entry.name, kStatePrefix)) {
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
    for (const fs::directory_entry& found : fs::recursive_directory_iterator(stateDir)) {
        if (!found.is_regular_file()) {
            continue;
        }
        const fs::path relative = fs::relative(found.path(), stateDir);
        // The router keeps rewriting its network database; a record it dropped mid-walk is gone.
        if (relative == kRestoreUpload || !fs::exists(found.path())) {
            continue;
        }
        entries.push_back({kStatePrefix + relative.generic_string(), readFile(found.path()),
            static_cast<unsigned>(found.status().permissions() & fs::perms::mask)});
    }
    entries.push_back({kConfigPrefix + configFile.filename().string(), readFile(configFile),
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
        if (!startsWith(entry.name, kStatePrefix)) {
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
