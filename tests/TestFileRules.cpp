// Bazarish project (c) 2026
#include "FileRules.hpp"
#include "TestUtil.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace bazarish::bot;

int main()
{
    const fs::path root = testutil::freshDir("bot-file-rules-test");
    fs::create_directories(root / "dir");
    { std::ofstream(root / "kept.txt") << "the operator's data\n"; }
    fs::create_symlink(root / "nowhere", root / "dangling");

    CHECK(checkSource((root / "kept.txt").string()).accepted);
    CHECK(checkSource("kept.txt").refusal == kFileMalformed);
    CHECK(checkSource((root / "absent.txt").string()).refusal == kFileMissing);
    CHECK(checkSource((root / "dir").string()).refusal == kFileMissing);

    CHECK(checkTarget((root / "dir" / "fresh.bin").string()).accepted);
    CHECK(checkTarget("fresh.bin").refusal == kFileMalformed);
    CHECK(checkTarget((root / "dir").string() + "/").refusal == kFileMalformed);
    CHECK(checkTarget((root / "kept.txt").string()).refusal == kFileInTheWay);
    CHECK(checkTarget((root / "dir").string()).refusal == kFileInTheWay);
    CHECK(checkTarget((root / "dangling").string()).refusal == kFileInTheWay);
    CHECK(checkTarget((root / "no-such-dir" / "fresh.bin").string()).refusal == kFileMissing);

    fs::remove_all(root);
    std::printf("TestFileRules OK\n");
    return 0;
}
