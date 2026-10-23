// Bazarish project (c) 2026
#include "PictureRules.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <utility>

namespace bazarish::bot {

namespace {

// PNG (ISO/IEC 15948) and JPEG (ITU T.81) open with these bytes.
constexpr std::array<std::uint8_t, 8> kPngSignature = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::array<std::uint8_t, 3> kJpegSignature = {0xFF, 0xD8, 0xFF};

template <std::size_t N>
bool opensWith(const Bytes& bytes, const std::array<std::uint8_t, N>& signature)
{
    return bytes.size() >= N && std::equal(signature.begin(), signature.end(), bytes.begin());
}

bool plainName(const std::string& name)
{
    return !name.empty() && name.size() <= kMaxPictureNameBytes
        && std::none_of(name.begin(), name.end(), [](const char c) {
               return c == '/' || c == '\\' || static_cast<unsigned char>(c) < 0x20;
           });
}

PictureCheck refused(const int refusal, std::string problem)
{
    PictureCheck check;
    check.refusal = refusal;
    check.problem = std::move(problem);
    return check;
}

}  // namespace

PictureCheck checkPicture(const std::string& base64, const std::string& name)
{
    Bytes bytes;
    try {
        bytes = fromBase64(base64);
    } catch (const std::exception&) {
        return refused(kPictureMalformed, "\"data\" is not base64");
    }
    if (bytes.empty()) {
        return refused(kPictureMalformed, "a picture is \"peer\" and the bytes in \"data\"");
    }
    if (bytes.size() > kMaxPictureBytes) {
        return refused(kPictureTooLarge,
            "a picture is at most " + std::to_string(kMaxPictureBytes) + " bytes");
    }
    PictureCheck check;
    if (opensWith(bytes, kPngSignature)) {
        check.mime = "image/png";
    } else if (opensWith(bytes, kJpegSignature)) {
        check.mime = "image/jpeg";
    } else {
        return refused(kPictureNotImage, "a picture is a PNG or a JPEG");
    }
    check.accepted = true;
    check.name = plainName(name) ? name
        : check.mime == "image/png"  ? "picture.png"
                                     : "picture.jpg";
    check.bytes = std::move(bytes);
    return check;
}

}  // namespace bazarish::bot
