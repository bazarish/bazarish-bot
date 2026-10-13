// Bazarish project (c) 2026
#pragma once

#include <bazarish/Bytes.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace bazarish::bot {

inline constexpr const char* kRestoreUpload = ".restore-upload.zip";

Bytes exportBackup(const std::filesystem::path& stateDir, const std::filesystem::path& configFile,
    const std::string& version, std::int64_t createdAt);
void checkBackup(const Bytes& archive);
void restoreBackup(const Bytes& archive, const std::filesystem::path& stateDir);

}  // namespace bazarish::bot
