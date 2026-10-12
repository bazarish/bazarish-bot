// Bazarish project (c) 2026
#include "Settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

#define CHECK_THROWS(expression)                                                     \
    do {                                                                             \
        bool thrown = false;                                                         \
        try {                                                                        \
            (void)(expression);                                                      \
        } catch (const std::exception&) {                                            \
            thrown = true;                                                           \
        }                                                                            \
        if (!thrown) {                                                                \
            std::fprintf(stderr, "CHECK_THROWS failed at %s:%d: %s did not throw\n", \
                __FILE__, __LINE__, #expression);                                    \
            std::exit(1);                                                            \
        }                                                                            \
    } while (false)

namespace fs = std::filesystem;
using bazarish::bot::Settings;

int main()
{
    const fs::path file = fs::temp_directory_path() / "bz-bot-settings.json";
    fs::remove(file);

    {
        Settings settings(file);
        CHECK(settings.transport() == Settings::Transport::eEmbedded);
        CHECK(!settings.hasPassword());
        CHECK(settings.tokens().empty());
        CHECK(!settings.passwordMatches(""));
        CHECK(!settings.passwordMatches("anything"));
    }

    {
        Settings settings(file);
        settings.setTransport(Settings::Transport::eSam, "127.0.0.1", 7656);
        settings.setPrivacy(bazarish::i2p::Privacy::eMinimal);
        settings.setPassword("correct horse");
        const auto token = settings.issueToken("nightly-report");
        CHECK(token.secret.size() == 64);
        CHECK(token.createdAt > 0);
    }
    {
        Settings settings(file);
        CHECK(settings.transport() == Settings::Transport::eSam);
        CHECK(settings.samHost() == "127.0.0.1");
        CHECK(settings.samPort() == 7656);
        CHECK(settings.privacy() == bazarish::i2p::Privacy::eMinimal);
        CHECK(settings.hasPassword());
        CHECK(settings.passwordMatches("correct horse"));
        CHECK(!settings.passwordMatches("correct horse "));
        CHECK(!settings.passwordMatches(""));
        CHECK(settings.tokens().size() == 1);

        const std::string secret = settings.tokens().front().secret;
        CHECK(settings.tokenNameFor(secret) == "nightly-report");
        CHECK(settings.tokenNameFor("").empty());
        CHECK(settings.tokenNameFor(secret.substr(0, secret.size() - 1)).empty());
        CHECK(settings.tokenNameFor(std::string(secret.size(), 'a')).empty());

        CHECK_THROWS(settings.issueToken("nightly-report"));
        CHECK(settings.issueToken("deploy-hook").secret != secret);
        CHECK(settings.tokens().size() == 2);

        CHECK(settings.revokeToken("nightly-report"));
        CHECK(!settings.revokeToken("nightly-report"));
        CHECK(settings.tokenNameFor(secret).empty());
        CHECK(settings.tokens().size() == 1);
    }

    {
        Settings settings(file);
        settings.setPassword("another one");
        CHECK(settings.passwordMatches("another one"));
        CHECK(!settings.passwordMatches("correct horse"));
    }

    fs::remove(file);
    std::fprintf(stderr, "TestSettings passed\n");
    return 0;
}
