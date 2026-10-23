// Bazarish project (c) 2026
#include "PictureRules.hpp"

#include <bazarish/Bytes.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

using namespace bazarish;
using namespace bazarish::bot;

namespace {

Bytes png(const std::size_t size)
{
    Bytes bytes = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    bytes.resize(size, 0);
    return bytes;
}

}  // namespace

int main()
{
    const PictureCheck accepted = checkPicture(toBase64(png(64)), "qr.png");
    CHECK(accepted.accepted && accepted.mime == "image/png" && accepted.name == "qr.png");
    CHECK(accepted.bytes == png(64));

    Bytes jpeg = {0xFF, 0xD8, 0xFF, 0xE0};
    jpeg.resize(32, 0);
    const PictureCheck photo = checkPicture(toBase64(jpeg), "");
    CHECK(photo.accepted && photo.mime == "image/jpeg" && photo.name == "picture.jpg");

    const PictureCheck limit = checkPicture(toBase64(png(kMaxPictureBytes)), "a.png");
    CHECK(limit.accepted);
    const PictureCheck over = checkPicture(toBase64(png(kMaxPictureBytes + 1)), "a.png");
    CHECK(!over.accepted && over.refusal == kPictureTooLarge);

    const Bytes gif = {'G', 'I', 'F', '8', '9', 'a', 0, 0};
    CHECK(checkPicture(toBase64(gif), "").refusal == kPictureNotImage);
    CHECK(checkPicture("not base64!", "").refusal == kPictureMalformed);
    CHECK(checkPicture("", "").refusal == kPictureMalformed);

    CHECK(checkPicture(toBase64(png(16)), "../etc/passwd").name == "picture.png");
    CHECK(checkPicture(toBase64(png(16)), "a\nb.png").name == "picture.png");
    CHECK(checkPicture(toBase64(png(16)), std::string(kMaxPictureNameBytes + 1, 'a')).name
        == "picture.png");

    std::printf("TestPictureRules: all checks passed\n");
    return 0;
}
