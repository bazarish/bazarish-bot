// Bazarish project (c) 2026
#pragma once

#include <string>

namespace bazarish::bot {

inline constexpr int kFileMalformed = 400;
inline constexpr int kFileMissing = 404;
inline constexpr int kFileInTheWay = 409;

struct FileCheck {
    bool accepted = false;
    int refusal = 0;
    std::string problem;
};

FileCheck checkSource(const std::string& path);
FileCheck checkTarget(const std::string& path);

}  // namespace bazarish::bot
