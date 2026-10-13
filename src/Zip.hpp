// Bazarish project (c) 2026
#pragma once

#include <bazarish/Bytes.hpp>

#include <string>
#include <vector>

namespace bazarish::bot {

inline constexpr unsigned kOwnerOnlyMode = 0600;

struct ZipEntry {
    std::string name;
    Bytes data;
    unsigned mode = kOwnerOnlyMode;
};

// Entries are stored, not deflated: what a bot keeps is mostly ciphertext already.
Bytes zipStored(const std::vector<ZipEntry>& entries);
std::vector<ZipEntry> unzipStored(const Bytes& archive);

}  // namespace bazarish::bot
