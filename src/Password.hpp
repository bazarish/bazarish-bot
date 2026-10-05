// Bazarish project (c) 2026
#pragma once

#include <string>

namespace bazarish::daemon {

struct PasswordRecord {
    std::string salt;
    std::string hash;
};

PasswordRecord hashPassword(const std::string& password);
bool passwordMatches(const PasswordRecord& record, const std::string& password);

}  // namespace bazarish::daemon
