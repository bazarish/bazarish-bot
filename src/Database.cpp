// Bazarish project (c) 2026
#include "Database.hpp"

#include <AccountKey.hpp>

#include <stdexcept>
#include <utility>

namespace bazarish::bot {

Database openDatabase(const std::filesystem::path& file, std::string label)
{
    std::filesystem::create_directories(file.parent_path());
    client::SqliteHandle db
        = client::openKeyed(file, client::accountkey::keyFor(file, std::string()));
    if (db == nullptr) {
        throw std::runtime_error(label + ": cannot open " + file.string());
    }
    return Database(std::move(db), std::move(label));
}

}  // namespace bazarish::bot
