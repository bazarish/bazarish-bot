// Bazarish project (c) 2026
#include "HttpFace.hpp"
#include "Runtime.hpp"
#include "HttpTestUtil.hpp"
#include "TestUtil.hpp"

#include <AccountManager.hpp>
#include <LoginSigner.hpp>

#include <bazarish/AccountKind.hpp>
#include <bazarish/Crypto.hpp>
#include <bazarish/HttpClient.hpp>
#include <bazarish/JsonRead.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>

namespace fs = std::filesystem;
namespace http = bazarish::http;

namespace {

using testutil::Headers;
using testutil::call;

constexpr int kOk = 200;
constexpr int kBadRequest = 400;
constexpr int kUnauthorized = 401;
constexpr int kUnavailable = 503;
constexpr char kMessageId[] = "00112233445566778899aabbccddeeff";

Headers signIn(const int port, const bazarish::Identity& operatorKey)
{
    const http::ClientResponse state = call(port, "GET", "/ui/state");
    CHECK(state.status == kOk);
    const std::string challenge
        = bazarish::parseJson(state.body).at("challenge").get<std::string>();
    const nlohmann::json login = {{"challenge", challenge},
        {"blob", bazarish::client::signLoginChallenge(operatorKey, challenge)}};
    const http::ClientResponse in = call(port, "POST", "/ui/login", {}, login.dump());
    CHECK(in.status == kOk);
    const std::string cookie = in.headers.at("set-cookie");
    return Headers{{"Cookie", cookie.substr(0, cookie.find(';'))}};
}

bool says(const http::ClientResponse& response, const int status, const std::string& words)
{
    return response.status == status && response.body.find(words) != std::string::npos;
}

}  // namespace

int main()
{
    const fs::path dir = testutil::freshDir("bot-face");
    bazarish::bot::Config config;
    config.path = dir / "bot.json";
    {
        std::ofstream out(config.path);
        out << "{}";
    }
    config.stateDir = dir / "state";
    config.listenHost = "127.0.0.1";
    config.consumer = {"Bazarish bot", {"http://127.0.0.1"}, "Operator"};
    const std::string id = bazarish::client::AccountManager(config.stateDir / "accounts")
                               .create("Face", bazarish::AccountKind::eBot)
                               .id;
    const bazarish::Identity operatorKey = bazarish::Identity::generate();
    {
        bazarish::bot::Runtime runtime(config);
        const std::string secret = runtime.settings().issueToken(id, "test").secret;
        bazarish::bot::HttpFace face(runtime, BAZARISH_BOT_SOURCE_DIR "/web");
        const int port = face.start();

        const Headers bearer = {{"Authorization", "Bearer " + secret}};
        CHECK(call(port, "GET", "/v1/messages?peer=x").status == kUnauthorized);
        CHECK(says(call(port, "GET", "/v1/messages?peer=x", bearer), kUnavailable, "not open"));
        CHECK(says(call(port, "POST", "/v1/storage/compact", bearer), kUnavailable, "not open"));
        CHECK(says(call(port, "DELETE", std::string("/v1/messages/") + kMessageId, bearer),
            kUnavailable, "not open"));

        const std::string base = "/ui/accounts/" + id;
        CHECK(call(port, "GET", base + "/contacts").status == kUnauthorized);
        const Headers cookie = signIn(port, operatorKey);
        CHECK(says(call(port, "GET", base + "/messages", cookie), kBadRequest,
            "names whose conversation to read"));
        CHECK(says(call(port, "POST", base + "/messages", cookie, R"({"text":"hi"})"),
            kBadRequest, "a message is a"));
        CHECK(says(call(port, "POST", base + "/messages", cookie,
                       R"({"peer":"p","text":"hi","keyboard":[],"replyTo":"r"})"),
            kBadRequest, "takes no"));
        CHECK(says(call(port, "POST", base + "/messages/" + kMessageId, cookie,
                       R"({"text":"hi"})"),
            kBadRequest, "names whose conversation the message is in"));
        CHECK(says(call(port, "DELETE", base + "/messages/" + kMessageId, cookie), kBadRequest,
            "names whose conversation the message is in"));
        CHECK(says(call(port, "POST", base + "/reactions", cookie, R"({"peer":"p"})"),
            kBadRequest, "a reaction is"));

        face.stop();
    }
    fs::remove_all(dir);
    return 0;
}
