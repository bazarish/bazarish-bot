// Bazarish project (c) 2026
#include "FileRules.hpp"

#include <bazarish/PathText.hpp>

#include <filesystem>
#include <system_error>
#include <utility>

namespace bazarish::bot {

namespace {

FileCheck refused(const int refusal, std::string problem)
{
    return FileCheck{false, refusal, std::move(problem)};
}

}  // namespace

FileCheck checkSource(const std::string& path)
{
    const std::filesystem::path source = pathFromUtf8(path);
    if (!source.is_absolute()) {
        return refused(kFileMalformed, "\"path\" names a file on this host from the root");
    }
    std::error_code error;
    if (!std::filesystem::is_regular_file(source, error)) {
        return refused(kFileMissing, "there is no file at " + path);
    }
    return FileCheck{true, 0, {}};
}

FileCheck checkTarget(const std::string& path)
{
    const std::filesystem::path target = pathFromUtf8(path);
    if (!target.is_absolute() || !target.has_filename()) {
        return refused(kFileMalformed, "\"path\" names a file on this host from the root");
    }
    std::error_code error;
    if (std::filesystem::exists(std::filesystem::symlink_status(target, error))) {
        return refused(kFileInTheWay, "something is already at " + path + ": it is not written over");
    }
    if (!std::filesystem::is_directory(target.parent_path(), error)) {
        return refused(kFileMissing, "there is no directory " + utf8OfPath(target.parent_path()));
    }
    return FileCheck{true, 0, {}};
}

}  // namespace bazarish::bot
