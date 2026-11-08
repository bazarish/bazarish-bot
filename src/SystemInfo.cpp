// Bazarish project (c) 2026
#include "SystemInfo.hpp"

#include <Health.hpp>

namespace bazarish::bot {

namespace fs = std::filesystem;

MemoryUse memoryUse()
{
    const health::MemoryUsage usage = health::processMemoryUsage();
    return MemoryUse{usage.current, usage.peak};
}

DiskUse diskUse(const fs::path& stateDir)
{
    return DiskUse{health::treeBytes(stateDir), fs::space(stateDir).available};
}

}  // namespace bazarish::bot
