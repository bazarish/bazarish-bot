// Bazarish project (c) 2026
#include "HttpFace.hpp"
#include "Runtime.hpp"
#include "Settings.hpp"
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
#include <tuple>
#include <vector>

namespace fs = std::filesystem;
namespace http = bazarish::http;

namespace {

using testutil::Headers;
using testutil::call;

constexpr int kOk = 200;
constexpr int kBadRequest = 400;
constexpr int kUnauthorized = 401;
constexpr int kNotFound = 404;
constexpr int kConflict = 409;
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
try {
    const testutil::ScratchDir scratch("bot-face");
    const fs::path& dir = scratch.path();
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
    const std::string groupId = bazarish::client::AccountManager(config.stateDir / "accounts")
                                    .create("Club", bazarish::AccountKind::eChat)
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
        CHECK(says(call(port, "POST", "/v1/storage/trim", bearer, R"({"keep":1})"), kUnavailable,
            "not open"));
        CHECK(says(call(port, "POST", "/v1/chats/p/trim", bearer, R"({"keep":1})"), kUnavailable,
            "not open"));
        CHECK(says(call(port, "DELETE", std::string("/v1/messages/") + kMessageId, bearer),
            kUnavailable, "not open"));

        CHECK(call(port, "GET", "/v1/profile").status == kUnauthorized);
        for (const auto& [verb, path, body] : std::vector<std::tuple<std::string, std::string,
                 std::string>>{{"GET", "/v1/profile", ""},
                 {"POST", "/v1/profile", R"({"statusText":"busy"})"},
                 {"GET", "/v1/profile/avatar", ""},
                 {"POST", "/v1/profile/avatar", R"({"data":""})"},
                 {"DELETE", "/v1/profile/avatar", ""}}) {
            CHECK(says(call(port, verb, path, bearer, body), kUnavailable, "not open"));
        }

        CHECK(says(call(port, "GET", "/v1/group", bearer), kConflict, "only a group account"));
        CHECK(says(call(port, "POST", "/v1/group/posts", bearer, R"({"text":"hi"})"), kConflict,
            "only a group account"));
        runtime.settings().rememberAccount(
            groupId, "fp-club", bazarish::bot::Settings::AccountKind::eGroup);
        const Headers groupBearer
            = {{"Authorization", "Bearer " + runtime.settings().issueToken(groupId, "g").secret}};
        CHECK(says(call(port, "GET", "/v1/messages?peer=x", groupBearer), kConflict,
            "a group answers"));
        CHECK(says(call(port, "GET", "/v1/events?since=0", groupBearer), kConflict, "a group"));
        for (const auto& [verb, path, body] : std::vector<std::tuple<std::string, std::string,
                 std::string>>{{"GET", "/v1/group", ""},
                 {"POST", "/v1/group", R"({"joinMode":"open"})"},
                 {"GET", "/v1/group/members", ""},
                 {"POST", "/v1/group/members", R"({"fingerprint":"f","action":"admin"})"},
                 {"GET", "/v1/group/requests", ""},
                 {"POST", "/v1/group/requests", R"({"number":1,"allow":true})"},
                 {"GET", "/v1/group/posts", ""},
                 {"POST", "/v1/group/posts", R"({"text":"hi"})"},
                 {"DELETE", "/v1/group/posts/p", ""},
                 {"GET", "/v1/profile", ""},
                 {"POST", "/v1/profile", R"({"statusText":"open"})"},
                 {"POST", "/v1/profile/avatar", R"({"data":""})"}}) {
            CHECK(says(call(port, verb, path, groupBearer, body), kUnavailable, "not open"));
        }
        CHECK(call(port, "POST", "/v1/posts", groupBearer, R"({"text":"hi"})").status
            == kNotFound);

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
        CHECK(says(call(port, "POST", base + "/messages/" + kMessageId, cookie,
                       R"({"peer":"p","text":"hi","keyboard":"ping"})"),
            kBadRequest, "array of rows"));
        CHECK(says(call(port, "POST", base + "/messages", cookie,
                       R"({"peer":"p","text":"hi","keyboard":[["ping"]]})"),
            kBadRequest, "a button is"));
        CHECK(says(call(port, "DELETE", base + "/messages/" + kMessageId, cookie), kBadRequest,
            "names whose conversation the message is in"));
        CHECK(says(call(port, "POST", base + "/reactions", cookie, R"({"peer":"p"})"),
            kBadRequest, "a reaction is"));
        CHECK(call(port, "GET", base + "/messages?peer=p", cookie).status != kUnauthorized);
        CHECK(nlohmann::json::parse(call(port, "GET", base, cookie).body).at("history") == true);
        runtime.settings().rememberAccount(
            id, "fp-face", bazarish::bot::Settings::AccountKind::eBot);
        CHECK(call(port, "POST", base, cookie, R"({"action":"history","keeps":false})").status
            == kOk);
        CHECK(!runtime.settings().keepsHistory(id));
        CHECK(says(call(port, "POST", base, cookie, R"({"action":"nothing"})"), kBadRequest,
            "history"));
        CHECK(call(port, "POST", "/ui/logout", cookie).status == kOk);
        CHECK(call(port, "GET", base + "/messages?peer=p", cookie).status == kUnauthorized);

        face.stop();
        runtime.stop();
    }
    return 0;
} catch (...) {
    return testutil::failed();
}
