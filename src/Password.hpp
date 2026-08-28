// Bazarish project (c) 2026
#pragma once

#include <string>

namespace bazarish::daemon {

// The operator's password, as it is kept: never the password itself, and never
// a digest a graphics card walks through. Argon2id, with the salt beside the
// hash; both are hex so the settings document stays readable.
struct PasswordRecord {
    std::string salt;
    std::string hash;
};

PasswordRecord hashPassword(const std::string& password);
// Constant-time in the comparison: an answer that comes back sooner is an
// answer about the password.
bool passwordMatches(const PasswordRecord& record, const std::string& password);

}  // namespace bazarish::daemon
