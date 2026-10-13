// Bazarish project (c) 2026
#include "NameRules.hpp"

#include <unicode/normalizer2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <unicode/uspoof.h>

#include <memory>
#include <stdexcept>

namespace bazarish::bot {

namespace {

constexpr const char* kLinkMark = "bazarish://";

struct SpoofCloser {
    void operator()(USpoofChecker* checker) const { uspoof_close(checker); }
};

using SpoofChecker = std::unique_ptr<USpoofChecker, SpoofCloser>;

SpoofChecker openChecker()
{
    UErrorCode status = U_ZERO_ERROR;
    SpoofChecker checker(uspoof_open(&status));
    if (U_FAILURE(status)) {
        throw std::runtime_error(std::string("name rules: ") + u_errorName(status));
    }
    uspoof_setRestrictionLevel(checker.get(), USPOOF_HIGHLY_RESTRICTIVE);
    uspoof_setChecks(checker.get(),
        USPOOF_RESTRICTION_LEVEL | USPOOF_INVISIBLE | USPOOF_MIXED_NUMBERS, &status);
    if (U_FAILURE(status)) {
        throw std::runtime_error(std::string("name rules: ") + u_errorName(status));
    }
    return checker;
}

bool forbiddenCharacter(const UChar32 point)
{
    const std::int8_t type = u_charType(point);
    return type == U_CONTROL_CHAR || type == U_FORMAT_CHAR || type == U_LINE_SEPARATOR
        || type == U_PARAGRAPH_SEPARATOR;
}

}  // namespace

NameCheck checkName(const std::string& typed)
{
    NameCheck check;
    icu::UnicodeString name = icu::UnicodeString::fromUTF8(typed);
    name.trim();
    const std::int32_t points = name.countChar32();
    if (points < 1 || static_cast<std::size_t>(points) > kMaxNameCodepoints) {
        return check;
    }
    for (std::int32_t at = 0; at < name.length(); at = name.moveIndex32(at, 1)) {
        if (forbiddenCharacter(name.char32At(at))) {
            return check;
        }
    }

    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* const folding = icu::Normalizer2::getNFKCCasefoldInstance(status);
    if (U_FAILURE(status)) {
        throw std::runtime_error(std::string("name rules: ") + u_errorName(status));
    }
    const icu::UnicodeString folded = folding->normalize(name, status);
    if (U_FAILURE(status)) {
        throw std::runtime_error(std::string("name rules: ") + u_errorName(status));
    }
    std::string foldedText;
    folded.toUTF8String(foldedText);
    if (foldedText.find(kLinkMark) != std::string::npos) {
        return check;
    }

    const SpoofChecker checker = openChecker();
    std::string nameText;
    name.toUTF8String(nameText);
    if (uspoof_checkUTF8(checker.get(), nameText.c_str(), -1, nullptr, &status) != 0) {
        return check;
    }
    if (U_FAILURE(status)) {
        throw std::runtime_error(std::string("name rules: ") + u_errorName(status));
    }
    icu::UnicodeString skeleton;
    uspoof_getSkeletonUnicodeString(checker.get(), 0, folded, skeleton, &status);
    if (U_FAILURE(status)) {
        throw std::runtime_error(std::string("name rules: ") + u_errorName(status));
    }
    check.allowed = true;
    check.name = nameText;
    skeleton.toUTF8String(check.key);
    return check;
}

}  // namespace bazarish::bot
