// Bazarish project (c) 2026
#pragma once

#include <KeyedDatabase.hpp>

#include <filesystem>
#include <string>

namespace bazarish::bot {

using client::Database;
using client::Statement;

Database openDatabase(const std::filesystem::path& file, std::string label);

}  // namespace bazarish::bot
