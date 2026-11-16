// Bazarish project (c) 2026
#pragma once

#include <bazarish/Bytes.hpp>

#include <filesystem>

namespace bazarish::bot {

Bytes takeCheckpoint(const std::filesystem::path& stateDir);

void restoreCheckpoint(const std::filesystem::path& stateDir, const Bytes& checkpoint);

// A file takeCheckpoint itself writes and removes while it copies a database.
bool isCheckpointScratch(const std::filesystem::path& file);

}  // namespace bazarish::bot
