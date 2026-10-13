// Bazarish project (c) 2026
#include "GroupCopy.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

using namespace bazarish::bot;

int main()
{
    CHECK(copyComplete());
    CHECK(copyText("en", Copy::eWelcome, {"Ann"}) == "Welcome, Ann.");
    CHECK(copyText("ru", Copy::eAllowedBy, {"Fay", "Bob"}) == "Fay: одобрено админом Bob.");
    CHECK(copyText("ar", Copy::eWelcome, {"Ann"}).find("Ann") != std::string::npos);
    for (const char* const language : kGroupLanguages) {
        CHECK(copyText(language, Copy::eWantsToJoin, {"x", "y"}).find('x') != std::string::npos);
        CHECK(copyText(language, Copy::eWantsToJoin, {"x", "y"}).find('y') != std::string::npos);
        CHECK(copyText(language, Copy::eAsksYou, {"x", "y"}).find('y') != std::string::npos);
    }
    bool refused = false;
    try {
        (void)copyText("de", Copy::eWelcome);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    CHECK(refused);
    std::printf("TestGroupCopy passed\n");
    return 0;
}
