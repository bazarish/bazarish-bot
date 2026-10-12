// Bazarish project (c) 2026
#include "OperatorAuth.hpp"

#include <LoginSigner.hpp>

#include <bazarish/ConfigFile.hpp>
#include <bazarish/Crypto.hpp>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
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
using bazarish::Identity;
using bazarish::bot::OperatorAuth;
using bazarish::client::signLoginChallenge;

int main()
{
    const fs::path config = fs::temp_directory_path() / "bz-bot-operator.json";
    {
        std::ofstream out(config);
        out << R"({"state": "/tmp/bz"})";
    }
    const bazarish::service::LoginConsumer consumer{"Bazarish bot", {"http://127.0.0.1:8440"},
        "Operator"};
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const Identity first = Identity::generate();
    const Identity second = Identity::generate();

    {
        OperatorAuth auth(config, std::string(), consumer);
        CHECK(!auth.claimed());

        const std::string challenge = auth.issueChallenge(now);
        CHECK(auth.signIn(challenge, signLoginChallenge(first, challenge), now)
            == first.fingerprint());
        CHECK(auth.claimed());
        CHECK(auth.operatorFingerprint() == first.fingerprint());
        CHECK_THROWS(auth.signIn(challenge, signLoginChallenge(first, challenge), now));

        const std::string other = auth.issueChallenge(now);
        CHECK_THROWS(auth.signIn(other, signLoginChallenge(second, other), now));
        CHECK(auth.operatorFingerprint() == first.fingerprint());
    }

    const std::string written
        = bazarish::ConfigView(bazarish::ConfigFile(config).read()).text("operator");
    CHECK(written == first.fingerprint());

    {
        OperatorAuth auth(config, written, consumer);
        const std::string challenge = auth.issueChallenge(now);
        CHECK(auth.signIn(challenge, signLoginChallenge(first, challenge), now)
            == first.fingerprint());
        const std::string other = auth.issueChallenge(now);
        CHECK_THROWS(auth.signIn(other, signLoginChallenge(second, other), now));
    }

    fs::remove(config);
    std::fprintf(stderr, "TestOperatorAuth passed\n");
    return 0;
}
