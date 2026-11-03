// Bazarish project (c) 2026
#pragma once

#include <string>

namespace bazarish::bot {

// The text as a black-on-white QR code, one SVG path; throws when it does not fit a symbol.
std::string qrSvg(const std::string& text);

}  // namespace bazarish::bot
