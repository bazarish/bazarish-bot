// Bazarish project (c) 2026
#include "StateCheckpoint.hpp"

#include <AccountKey.hpp>
#include <KeyedDatabase.hpp>

#include <bazarish/JsonRead.hpp>
#include <bazarish/PathText.hpp>
#include <bazarish/PrivateFile.hpp>

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <system_error>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr const char* kRouterDirectory = "i2p";
constexpr const char* kDatabaseExtension = ".db";
constexpr const char* kCopySuffix = ".checkpoint";
constexpr const char* kFilesField = "files";

bool leftOut(const std::string& name)
{
    return name.ends_with("-wal") || name.ends_with("-shm") || name.ends_with(".tmp")
        || name.ends_with(kCopySuffix);
}

Bytes bytesOf(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

}  // namespace

Bytes takeCheckpoint(const fs::path& stateDir)
{
    std::map<std::string, fs::path> found;
    for (auto entry = fs::recursive_directory_iterator(stateDir);
         entry != fs::recursive_directory_iterator(); ++entry) {
        const std::string name = entry->path().lexically_relative(stateDir).generic_string();
        if (entry->is_directory() && name == kRouterDirectory) {
            entry.disable_recursion_pending();
            continue;
        }
        if (entry->is_regular_file() && !leftOut(name)) {
            found.emplace(name, entry->path());
        }
    }
    nlohmann::json files = nlohmann::json::object();
    for (const auto& [name, path] : found) {
        if (path.extension() != kDatabaseExtension) {
            files[name] = nlohmann::json::binary(bytesOf(readFileText(path)));
            continue;
        }
        fs::path copy = path;
        copy += kCopySuffix;
        client::copyKeyedDatabase(path, copy, client::accountkey::keyFor(path, std::string()));
        files[name] = nlohmann::json::binary(bytesOf(readFileText(copy)));
        fs::remove(copy);
    }
    return nlohmann::json::to_cbor(nlohmann::json{{kFilesField, std::move(files)}});
}

void restoreCheckpoint(const fs::path& stateDir, const Bytes& checkpoint)
{
    const nlohmann::json document = bazarish::parseCbor(checkpoint);
    fs::remove_all(stateDir);
    fs::create_directories(stateDir);
    for (const auto& [name, content] : document.at(kFilesField).items()) {
        const fs::path relative = pathFromUtf8(name).lexically_normal();
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
            throw std::runtime_error("checkpoint: " + name + " points outside the state directory");
        }
        const fs::path target = stateDir / relative;
        fs::create_directories(target.parent_path());
        const Bytes& bytes = content.get_binary();
        writePrivateFile(target, std::string(bytes.begin(), bytes.end()));
    }
}

}  // namespace bazarish::bot
