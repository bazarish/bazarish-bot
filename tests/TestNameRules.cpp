// Bazarish project (c) 2026
#include "NameRules.hpp"

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

using bazarish::bot::checkName;
using bazarish::bot::kMaxNameCodepoints;
using bazarish::bot::NameCheck;

int main()
{
    const NameCheck ann = checkName("  Ann ");
    CHECK(ann.allowed);
    CHECK(ann.name == "Ann");
    CHECK(checkName("ANN").key == ann.key);
    CHECK(checkName("\xEF\xBC\xA1nn").key == ann.key);

    const NameCheck latin = checkName("Ace");
    const NameCheck cyrillic = checkName("Асе");
    CHECK(latin.allowed);
    CHECK(cyrillic.allowed);
    CHECK(latin.key == cyrillic.key);
    CHECK(!checkName("Аce").allowed);

    CHECK(checkName("Иван").allowed);
    CHECK(checkName("王小明").allowed);
    CHECK(checkName("محمد").allowed);
    CHECK(checkName("Ann 2").allowed);

    CHECK(!checkName("").allowed);
    CHECK(!checkName("   ").allowed);
    CHECK(checkName(std::string(kMaxNameCodepoints, 'a')).allowed);
    CHECK(!checkName(std::string(kMaxNameCodepoints + 1, 'a')).allowed);
    CHECK(!checkName("An​n").allowed);
    CHECK(!checkName("A\tnn").allowed);
    CHECK(!checkName("bazarish://invite").allowed);

    std::printf("TestNameRules passed\n");
    return 0;
}
