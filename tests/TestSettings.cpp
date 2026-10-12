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

    const std::string first = "0123456789abcdef";
    const std::string second = "fedcba9876543210";

    {
        Settings settings(file);
        CHECK(settings.transport() == Settings::Transport::eEmbedded);
        CHECK(settings.tokens(first).empty());
        CHECK(settings.kind(first) == Settings::AccountKind::eBot);
    }

    {
        Settings settings(file);
        settings.setTransport(Settings::Transport::eSam, "127.0.0.1", 7656);
        settings.setPrivacy(bazarish::i2p::Privacy::eMinimal);
        CHECK_THROWS(settings.setKind(second, Settings::AccountKind::eGroup));
        settings.rememberAccount(first, "fp-first", Settings::AccountKind::eBot);
        settings.rememberAccount(second, "fp-second", Settings::AccountKind::eBot);
        settings.setKind(second, Settings::AccountKind::eGroup);
        const auto token = settings.issueToken(first, "nightly-report");
        CHECK(token.secret.size() == 64);
        CHECK(token.createdAt > 0);
        CHECK(token.account == first);
    }
    {
        Settings settings(file);
        CHECK(settings.transport() == Settings::Transport::eSam);
        CHECK(settings.samHost() == "127.0.0.1");
        CHECK(settings.samPort() == 7656);
        CHECK(settings.privacy() == bazarish::i2p::Privacy::eMinimal);
        CHECK(settings.kind(first) == Settings::AccountKind::eBot);
        CHECK(settings.kind(second) == Settings::AccountKind::eGroup);
        CHECK(settings.accountWithFingerprint("fp-second") == second);
        CHECK(settings.accountWithFingerprint("fp-third").empty());
        CHECK(settings.fingerprintOf(first) == "fp-first");
        CHECK(settings.fingerprintOf("unknown").empty());
        CHECK(settings.tokens(first).size() == 1);
        CHECK(settings.tokens(second).empty());

        const std::string secret = settings.tokens(first).front().secret;
        CHECK(settings.tokenFor(secret)->name == "nightly-report");
        CHECK(settings.tokenFor(secret)->account == first);
        CHECK(!settings.tokenFor(""));
        CHECK(!settings.tokenFor(secret.substr(0, secret.size() - 1)));
        CHECK(!settings.tokenFor(std::string(secret.size(), 'a')));

        CHECK_THROWS(settings.issueToken(first, "nightly-report"));
        const auto other = settings.issueToken(second, "nightly-report");
        CHECK(other.secret != secret);
        CHECK(settings.tokenFor(other.secret)->account == second);
        CHECK(settings.issueToken(first, "deploy-hook").secret != secret);
        CHECK(settings.tokens(first).size() == 2);

        CHECK(!settings.revokeToken(second, "deploy-hook"));
        CHECK(settings.revokeToken(first, "nightly-report"));
        CHECK(!settings.revokeToken(first, "nightly-report"));
        CHECK(!settings.tokenFor(secret));
        CHECK(settings.tokens(first).size() == 1);

        settings.forgetAccount(second);
        CHECK(!settings.tokenFor(other.secret));
        CHECK(settings.kind(second) == Settings::AccountKind::eBot);
        CHECK(settings.accountWithFingerprint("fp-second").empty());
        CHECK(settings.tokens(first).size() == 1);
    }

    fs::remove(file);
    std::fprintf(stderr, "TestSettings passed\n");
    return 0;
}
