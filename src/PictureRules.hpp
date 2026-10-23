// Bazarish project (c) 2026
#pragma once

#include <bazarish/Bytes.hpp>

#include <cstddef>
#include <string>

namespace bazarish::bot {

// The composer's own cap: a picture this size and its envelope stay inside one 512 KiB message.
inline constexpr std::size_t kMaxPictureBytes = 256 * 1024;
inline constexpr std::size_t kMaxPictureNameBytes = 128;

inline constexpr int kPictureMalformed = 400;
inline constexpr int kPictureTooLarge = 413;
inline constexpr int kPictureNotImage = 415;

struct PictureCheck {
    bool accepted = false;
    int refusal = 0;
    std::string problem;
    Bytes bytes;
    std::string mime;
    std::string name;
};

PictureCheck checkPicture(const std::string& base64, const std::string& name);

}  // namespace bazarish::bot
