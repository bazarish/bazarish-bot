// Bazarish project (c) 2026
#pragma once

#include <cstddef>
#include <string>

namespace bazarish::bot {

inline constexpr std::size_t kMaxNameCodepoints = 32;

struct NameCheck {
    bool allowed = false;
    std::string name;
    std::string key;
};

NameCheck checkName(const std::string& typed);

}  // namespace bazarish::bot
