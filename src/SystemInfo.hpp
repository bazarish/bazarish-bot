// Bazarish project (c) 2026
#pragma once

#include <cstdint>
#include <filesystem>

namespace bazarish::bot {

struct MemoryUse {
    std::uint64_t residentBytes = 0;
    std::uint64_t peakBytes = 0;
};

struct DiskUse {
    std::uint64_t stateBytes = 0;
    std::uint64_t freeBytes = 0;
    std::uint64_t totalBytes = 0;
};

MemoryUse memoryUse();
DiskUse diskUse(const std::filesystem::path& stateDir);

}  // namespace bazarish::bot
