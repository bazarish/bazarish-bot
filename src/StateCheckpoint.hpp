// Bazarish project (c) 2026
#pragma once

#include <bazarish/Bytes.hpp>

#include <filesystem>

namespace bazarish::bot {

// Taken while the accounts keep running; the router's own directory is left out.
Bytes takeCheckpoint(const std::filesystem::path& stateDir);

// Nothing in the directory may be open.
void restoreCheckpoint(const std::filesystem::path& stateDir, const Bytes& checkpoint);

}  // namespace bazarish::bot
