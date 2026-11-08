// Bazarish project (c) 2026
#include "Settings.hpp"
#include "TestUtil.hpp"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using bazarish::bot::Settings;

int main()
{
    const fs::path dir = testutil::freshDir("bot-settings");
    const fs::path file = dir / "settings.json";

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
        settings.rememberAccount(first, "fp-first", Settings::AccountKind::eBot);
        settings.rememberAccount(second, "fp-second", Settings::AccountKind::eGroup);
        CHECK(settings.acceptsContacts(first));
        CHECK(!settings.acceptsContacts(second));
        CHECK(settings.enabled(first) && settings.enabled(second));
        settings.setEnabled(second, false);
        settings.rememberAccount(second, "fp-second", Settings::AccountKind::eGroup);
        CHECK(!settings.enabled(second));
        CHECK_THROWS(settings.setEnabled("unknown", false));
        CHECK(settings.keepsHistory(first));
        settings.setKeepsHistory(first, false);
        CHECK_THROWS(settings.setKeepsHistory("unknown", false));
        settings.setAcceptsContacts(first, false);
        settings.rememberAccount(first, "fp-first", Settings::AccountKind::eBot);
        CHECK_THROWS(settings.setAcceptsContacts("unknown", true));
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
        CHECK(!settings.acceptsContacts(first));
        CHECK(!settings.acceptsContacts(second));
        CHECK(settings.enabled(first));
        CHECK(!settings.enabled(second));
        CHECK(!settings.keepsHistory(first) && settings.keepsHistory(second));
        CHECK(!settings.enabled("unknown"));
        CHECK(settings.fingerprintOf(second) == "fp-second");
        CHECK(settings.claimFingerprint("intruder", "fp-second", Settings::AccountKind::eBot)
            == second);
        CHECK(settings.fingerprintOf("intruder").empty());
        CHECK(settings.claimFingerprint(second, "fp-second", Settings::AccountKind::eGroup).empty());
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
        CHECK(settings.fingerprintOf(second).empty());
        CHECK(settings.tokens(first).size() == 1);
    }

    fs::remove_all(dir);
    std::fprintf(stderr, "TestSettings passed\n");
    return 0;
}
