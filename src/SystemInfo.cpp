// Bazarish project (c) 2026
#include "SystemInfo.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr const char* kStatusFile = "/proc/self/status";
constexpr const char* kResidentField = "VmRSS:";
constexpr const char* kPeakField = "VmHWM:";
// proc(5) gives these two fields in kibibytes.
constexpr std::uint64_t kKibibyte = 1024;

}  // namespace

MemoryUse memoryUse()
{
    std::ifstream status(kStatusFile);
    if (!status) {
        throw std::runtime_error("system: cannot read " + std::string(kStatusFile));
    }
    MemoryUse use;
    std::string line;
    while (std::getline(status, line)) {
        std::istringstream fields(line);
        std::string name;
        std::uint64_t kibibytes = 0;
        fields >> name >> kibibytes;
        if (name == kResidentField) {
            use.residentBytes = kibibytes * kKibibyte;
        } else if (name == kPeakField) {
            use.peakBytes = kibibytes * kKibibyte;
        }
    }
    return use;
}

DiskUse diskUse(const fs::path& stateDir)
{
    DiskUse use;
    std::error_code failure;
    for (fs::recursive_directory_iterator walk(stateDir, failure), end; !failure && walk != end;
         walk.increment(failure)) {
        std::error_code gone;
        if (walk->is_regular_file(gone)) {
            const std::uintmax_t size = walk->file_size(gone);
            use.stateBytes += gone ? 0 : size;
        }
    }
    if (failure) {
        throw std::runtime_error("system: cannot walk " + stateDir.string() + ": "
            + failure.message());
    }
    const fs::space_info space = fs::space(stateDir);
    use.freeBytes = space.available;
    use.totalBytes = space.capacity;
    return use;
}

}  // namespace bazarish::bot
