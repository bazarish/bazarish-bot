// Bazarish project (c) 2026
#include "HttpFace.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Log.hpp>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <ctime>
#include <functional>
#include <thread>
#include <stdexcept>
#include <utility>

namespace bazarish::daemon {

namespace {

// A sign-in lasts a working day; the secret behind it is minted at every start,
// so a restart signs everyone out.
constexpr std::int64_t kSessionSeconds = 12 * 60 * 60;
constexpr std::size_t kSessionSecretBytes = 32;
// The name the operator is signed in under. There is one account and one
// operator here, so the token binds a role rather than a person.
constexpr const char* kOperator = "operator";
constexpr const char* kSessionCookie = "bazarish_daemon";
// What a long poll waits when the caller asks for nothing in particular.
constexpr int kDefaultWaitSeconds = 30;
// A page of history when the caller names no size.
constexpr int kDefaultPage = 50;

std::string sessionSecret()
{
    Bytes secret(kSessionSecretBytes);
    if (RAND_bytes(secret.data(), static_cast<int>(secret.size())) != 1) {
        throw std::runtime_error("no randomness for the sign-in secret");
    }
    return toHex(secret);
}

std::int64_t nowSeconds()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

http::Response json(const nlohmann::json& body, const int status = 200)
{
    http::Response response;
    response.status = status;
    response.contentType = "application/json";
    response.body = body.dump();
    return response;
}

// The account's backup, named as the desktop client names it: the display name
// with everything a file name will not take replaced, and the extension both
// ends read.
std::string backupFileName(const std::string& displayName)
{
    std::string name;
    for (const char c : displayName) {
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        name += plain ? c : '_';
    }
    if (name.empty()) {
        name = "bazarish";
    }
    return name + ".bazarish";
}

http::Response failure(const int status, const std::string& message)
{
    return json({{"error", message}}, status);
}

nlohmann::json lineJson(const Line& line)
{
    nlohmann::json item = {
        {"id", line.id},
        {"peer", line.peer},
        {"outgoing", line.outgoing},
        {"type", line.type},
        {"text", line.text},
        {"sentAt", line.sentAt},
        {"edited", line.edited},
    };
    if (!line.status.empty()) {
        item["status"] = line.status;
    }
    if (!line.e2eId.empty()) {
        item["messageId"] = line.e2eId;
    }
    if (!line.replyTo.empty()) {
        item["replyTo"] = line.replyTo;
    }
    if (!line.keyboard.empty()) {
        item["keyboard"] = nlohmann::json::parse(line.keyboard, nullptr, false);
    }
    if (!line.reactions.empty()) {
        nlohmann::json reactions = nlohmann::json::array();
        for (const Reaction& reaction : line.reactions) {
            reactions.push_back({{"reactor", reaction.reactor}, {"emoji", reaction.emoji}});
        }
        item["reactions"] = reactions;
    }
    return item;
}

nlohmann::json eventJson(const Event& event)
{
    nlohmann::json item = {
        {"id", event.id},
        {"kind", event.kind},
        {"peer", event.peer},
    };
    if (!event.messageId.empty()) {
        item["messageId"] = event.messageId;
    }
    if (!event.text.empty()) {
        item["text"] = event.text;
    }
    if (!event.actor.empty()) {
        item["actor"] = event.actor;
    }
    if (event.at != 0) {
        item["sentAt"] = event.at;
    }
    return item;
}

nlohmann::json eventsJson(const std::vector<Event>& events)
{
    nlohmann::json array = nlohmann::json::array();
    for (const Event& event : events) {
        array.push_back(eventJson(event));
    }
    return array;
}

nlohmann::json linesJson(const std::vector<Line>& lines)
{
    nlohmann::json array = nlohmann::json::array();
    for (const Line& line : lines) {
        array.push_back(lineJson(line));
    }
    return array;
}

int intParam(const http::Request& request, const std::string& key, const int fallback)
{
    const std::string text = request.query(key);
    if (text.empty()) {
        return fallback;
    }
    try {
        return std::stoi(text);
    } catch (const std::exception&) {
        return fallback;
    }
}

std::int64_t longParam(const http::Request& request, const std::string& key)
{
    const std::string text = request.query(key);
    if (text.empty()) {
        return 0;
    }
    try {
        return std::stoll(text);
    } catch (const std::exception&) {
        return 0;
    }
}

}  // namespace

HttpFace::HttpFace(Runtime& runtime, std::filesystem::path assets)
    : runtime_(runtime)
    , assets_(std::move(assets))
    , sessions_(sessionSecret(), kSessionSeconds)
{
}

HttpFace::~HttpFace()
{
    stop();
}

std::string HttpFace::tokenFor(const http::Request& request) const
{
    const std::string header = request.header("authorization");
    static const std::string kBearer = "Bearer ";
    if (header.size() <= kBearer.size() || header.compare(0, kBearer.size(), kBearer) != 0) {
        return {};
    }
    return runtime_.settings().tokenNameFor(header.substr(kBearer.size()));
}

bool HttpFace::signedIn(const http::Request& request) const
{
    const std::string cookie = request.cookie(kSessionCookie);
    if (cookie.empty()) {
        return false;
    }
    try {
        return sessions_.verify(cookie, nowSeconds()) == kOperator;
    } catch (const std::exception&) {
        return false;
    }
}

int HttpFace::start()
{
    http::Server::Options options;
    options.host = runtime_.config().listenHost;
    options.port = runtime_.config().listenPort;
    server_ = std::make_unique<http::Server>(options);
    routes();
    const int bound = server_->start();
    if (bound == 0) {
        throw std::runtime_error("cannot answer on " + options.host + ":"
            + std::to_string(options.port));
    }
    return bound;
}

void HttpFace::stop()
{
    if (server_) {
        server_->stop();
        server_.reset();
    }
}


void HttpFace::routes()
{
    // ---- the REST API the automation calls -------------------------------
    //
    // Every route here is opened by a token in the Authorization header and by
    // nothing else. A refusal says which of the two it was: no token at all, or
    // a token that is not one of ours.
    const auto guarded = [this](const http::Request& request,
                             const std::function<http::Response(const http::Request&)>& handler) {
        if (tokenFor(request).empty()) {
            return failure(401, request.hasHeader("authorization")
                    ? "this token does not open the API"
                    : "the API is opened by a token in the Authorization header");
        }
        if (!runtime_.running()) {
            return failure(503, "the account is not open: " + runtime_.status());
        }
        try {
            return handler(request);
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    };

    server_->get("/v1/status", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request&) {
            return json({
                {"fingerprint", runtime_.fingerprint()},
                {"name", runtime_.displayName()},
                {"destination", runtime_.destination()},
                {"status", runtime_.status()},
                {"lastEventId", runtime_.lastEventId()},
            });
        });
    });

    server_->get("/v1/invite", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request&) {
            return json({{"invite", runtime_.inviteUri()}});
        });
    });

    server_->get("/v1/contacts", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request&) {
            nlohmann::json array = nlohmann::json::array();
            for (const Runtime::Peer& peer : runtime_.contacts()) {
                array.push_back({{"fingerprint", peer.fingerprint}, {"name", peer.name},
                    {"pending", peer.pending}, {"tokens", peer.tokens}});
            }
            return json(array);
        });
    });

    server_->post("/v1/contacts", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            if (body.contains("invite")) {
                return json({{"fingerprint",
                    runtime_.addByInvite(body.at("invite").get<std::string>(),
                        body.value("text", std::string()))}});
            }
            if (body.contains("accept")) {
                runtime_.acceptContact(body.at("accept").get<std::string>());
                return json({{"accepted", body.at("accept")}});
            }
            return failure(400, "a contact is added by an \"invite\" or accepted by \"accept\"");
        });
    });

    server_->get("/v1/chats", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request&) {
            return json(linesJson(runtime_.chats()));
        });
    });

    server_->get("/v1/messages", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request& call) {
            const std::string peer = call.query("peer");
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation to read");
            }
            return json(linesJson(runtime_.conversation(peer, longParam(call, "before"),
                intParam(call, "limit", kDefaultPage))));
        });
    });

    server_->post("/v1/messages", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string text = body.value("text", std::string());
            if (peer.empty() || text.empty()) {
                return failure(400, "a message is a \"peer\" and a \"text\"");
            }
            const Runtime::Sent sent = body.contains("keyboard")
                ? runtime_.sendKeyboard(peer, text, body.at("keyboard").dump())
                : runtime_.sendText(peer, text);
            return json({{"messageId", sent.messageId}, {"confirmed", sent.confirmed}});
        });
    });

    server_->post("/v1/messages/([0-9a-f]+)", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation the message is in");
            }
            runtime_.editMessage(peer, call.captures.at(0), body.value("text", std::string()));
            return json({{"edited", call.captures.at(0)}});
        });
    });

    server_->del("/v1/messages/([0-9a-f]+)", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request& call) {
            const std::string peer = call.query("peer");
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation the message is in");
            }
            runtime_.deleteMessage(peer, call.captures.at(0));
            return json({{"deleted", call.captures.at(0)}});
        });
    });

    server_->post("/v1/reactions", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string messageId = body.value("messageId", std::string());
            if (peer.empty() || messageId.empty()) {
                return failure(400, "a reaction is a \"peer\", a \"messageId\" and an \"emoji\"");
            }
            // An empty emoji is how a reaction is taken back, so it is allowed.
            runtime_.react(peer, messageId, body.value("emoji", std::string()));
            return json({{"ok", true}});
        });
    });

    server_->post("/v1/receipts", [this, guarded](const http::Request& request) {
        return guarded(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string messageId = body.value("messageId", std::string());
            if (peer.empty() || messageId.empty()) {
                return failure(400, "a receipt is a \"peer\" and a \"messageId\"");
            }
            runtime_.confirmReceipt(peer, messageId);
            return json({{"ok", true}});
        });
    });

    // The one route that waits: it answers when there is something after
    // "since", or with nothing when the wait is up. A bot loops on it and
    // neither spins nor lags.
    server_->getAsync("/v1/events",
        [this](const http::Request& request, http::Responder respond)
            -> std::optional<http::Response> {
            if (tokenFor(request).empty()) {
                return failure(401, "the API is opened by a token in the Authorization header");
            }
            if (!runtime_.running()) {
                return failure(503, "the account is not open: " + runtime_.status());
            }
            const std::int64_t since = longParam(request, "since");
            const int wait = intParam(request, "wait", kDefaultWaitSeconds);
            // The wait is spent on a thread of its own so the loop keeps serving.
            std::thread([this, since, wait, respond = std::move(respond)]() {
                try {
                    const std::vector<Event> events = runtime_.await(since, wait);
                    respond(json({{"events", eventsJson(events)},
                        {"lastEventId", events.empty() ? since : events.back().id}}));
                } catch (const std::exception& error) {
                    respond(failure(503, error.what()));
                }
            }).detach();
            return std::nullopt;
        });

    // ---- what the interface itself calls ---------------------------------
    //
    // Opened by the operator's password, kept in a cookie. Until a password is
    // set there is nothing to sign in to, and the only thing that answers is the
    // route that sets one.
    const auto operatorOnly = [this](const http::Request& request,
                                  const std::function<http::Response(const http::Request&)>& handler) {
        if (!signedIn(request)) {
            return failure(401, "sign in first");
        }
        try {
            return handler(request);
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    };

    server_->get("/ui/state", [this](const http::Request& request) {
        return json({
            {"hasPassword", runtime_.settings().hasPassword()},
            {"signedIn", signedIn(request)},
            {"hasAccount", runtime_.hasAccount()},
            {"running", runtime_.running()},
            {"status", runtime_.status()},
        });
    });

    server_->post("/ui/password", [this](const http::Request& request) {
        try {
            const nlohmann::json body = nlohmann::json::parse(request.body);
            const std::string password = body.value("password", std::string());
            if (password.empty()) {
                return failure(400, "a password that is empty is not a password");
            }
            // The first one is set by whoever reaches the interface first, which
            // on loopback is the operator. Every one after it is a change, and a
            // change asks for the current password even from a signed-in page:
            // otherwise a page left open is a way to take the panel over.
            if (runtime_.settings().hasPassword()) {
                if (!runtime_.settings().passwordMatches(body.value("current", std::string()))) {
                    return failure(401, "the current password does not match");
                }
                // Checked here rather than in the page: a check the page makes is
                // one that is not made.
                if (body.value("repeat", std::string()) != password) {
                    return failure(400, "the two new passwords are not the same");
                }
            }
            runtime_.settings().setPassword(password);
            return json({{"ok", true}});
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    });

    server_->post("/ui/login", [this](const http::Request& request) {
        try {
            const nlohmann::json body = nlohmann::json::parse(request.body);
            if (!runtime_.settings().passwordMatches(body.value("password", std::string()))) {
                return failure(401, "that is not the password");
            }
            http::Response response = json({{"ok", true}});
            response.headers["Set-Cookie"] = std::string(kSessionCookie) + "="
                + sessions_.issue(kOperator, nowSeconds())
                + "; Path=/; HttpOnly; SameSite=Strict";
            return response;
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    });

    server_->post("/ui/logout", [](const http::Request&) {
        http::Response response = json({{"ok", true}});
        response.headers["Set-Cookie"]
            = std::string(kSessionCookie) + "=; Path=/; HttpOnly; Max-Age=0";
        return response;
    });

    server_->get("/ui/settings", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            Settings& settings = runtime_.settings();
            nlohmann::json tokens = nlohmann::json::array();
            for (const ApiToken& token : settings.tokens()) {
                // The secret is not handed back: it was shown when it was issued
                // and is not something this page may leak later.
                tokens.push_back({{"name", token.name}, {"createdAt", token.createdAt}});
            }
            return json({
                {"transport", settings.transport() == Settings::Transport::eSam ? "sam"
                                                                                : "embedded"},
                {"samHost", settings.samHost()},
                {"samPort", settings.samPort()},
                {"privacy", settings.privacy() == i2p::Privacy::eMinimal ? "minimal"
                        : settings.privacy() == i2p::Privacy::eMax       ? "max"
                                                                        : "middle"},
                {"tokens", tokens},
                {"restartNeeded", runtime_.running()},
            });
        });
    });

    server_->post("/ui/settings", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            if (body.contains("transport")) {
                const std::string named = body.at("transport").get<std::string>();
                if (named != "sam" && named != "embedded") {
                    return failure(400, "the transport is \"embedded\" or \"sam\"");
                }
                runtime_.settings().setTransport(
                    named == "sam" ? Settings::Transport::eSam : Settings::Transport::eEmbedded,
                    body.value("samHost", std::string("127.0.0.1")),
                    body.value("samPort", 7656));
            }
            if (body.contains("privacy")) {
                const auto parsed = i2p::privacyFromString(body.at("privacy").get<std::string>());
                if (!parsed) {
                    return failure(400, "the tunnel length is minimal, middle or max");
                }
                runtime_.settings().setPrivacy(*parsed);
            }
            return json({{"ok", true}});
        });
    });

    server_->post("/ui/tokens", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string name = body.value("name", std::string());
            if (name.empty()) {
                return failure(400, "a token is issued under a name");
            }
            const ApiToken token = runtime_.settings().issueToken(name);
            // The one time the secret is shown: it is not kept anywhere it could
            // be shown again.
            return json({{"name", token.name}, {"token", token.secret},
                {"createdAt", token.createdAt}});
        });
    });

    server_->del("/ui/tokens/([^/]+)", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            if (!runtime_.settings().revokeToken(call.captures.at(0))) {
                return failure(404, "no token by that name");
            }
            return json({{"ok", true}});
        });
    });

    server_->post("/ui/account", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string action = body.value("action", std::string());
            const std::string passphrase = body.value("passphrase", std::string());
            // The three of these start the account coming up and return at once:
            // a router that is finding the network takes minutes. What came of it
            // is read from the status, not waited for here.
            if (action == "create") {
                runtime_.createAccount(body.value("name", std::string()), passphrase);
                return json({{"status", runtime_.status()}});
            }
            if (action == "import") {
                // The bundle arrives as bytes in the request: an operator with a
                // browser has a file, not a path on the daemon's machine.
                return json({{"status", [&]() {
                    runtime_.importAccount(fromBase64(body.value("bundle", std::string())),
                        body.value("bundlePassphrase", std::string()), passphrase);
                    return runtime_.status();
                }()}});
            }
            if (action == "rename") {
                runtime_.rename(body.value("name", std::string()));
                return json({{"name", body.value("name", std::string())}});
            }
            if (action == "rotate-serving-key") {
                runtime_.rotateServingKey();
                return json({{"ok", true}});
            }
            if (action == "new-address") {
                runtime_.newAddress();
                return json({{"address", runtime_.destination()}});
            }
            if (action == "open") {
                runtime_.start(passphrase);
                return json({{"status", runtime_.status()}});
            }
            return failure(400,
                "the action is create, import, open, rename, rotate-serving-key or new-address");
        });
    });

    server_->post("/ui/account/export", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const Bytes bundle = runtime_.exportAccount(
                body.value("bundlePassphrase", std::string()));
            // Handed back as a file the browser saves, so nothing is written on
            // the machine the daemon runs on and left there. Named the way the
            // desktop client names it - it is the same backup, and the same file
            // restores it there.
            http::Response response;
            response.contentType = "application/octet-stream";
            response.headers["Content-Disposition"]
                = "attachment; filename=\"" + backupFileName(runtime_.displayName()) + "\"";
            response.body.assign(bundle.begin(), bundle.end());
            return response;
        });
    });

    server_->get("/ui/account", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            return json({
                {"fingerprint", runtime_.fingerprint()},
                {"name", runtime_.displayName()},
                {"destination", runtime_.destination()},
                {"invite", runtime_.running() ? runtime_.inviteUri() : std::string()},
                {"status", runtime_.status()},
            });
        });
    });

    server_->get("/ui/contacts", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            nlohmann::json array = nlohmann::json::array();
            for (const Runtime::Peer& peer : runtime_.contacts()) {
                array.push_back({{"fingerprint", peer.fingerprint}, {"name", peer.name},
                    {"pending", peer.pending}, {"tokens", peer.tokens}});
            }
            return json(array);
        });
    });

    server_->get("/ui/chats", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            return json(linesJson(runtime_.chats()));
        });
    });

    server_->get("/ui/messages", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const std::string peer = call.query("peer");
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation to read");
            }
            return json(linesJson(runtime_.conversation(peer, longParam(call, "before"),
                intParam(call, "limit", kDefaultPage))));
        });
    });

    server_->post("/ui/messages", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string text = body.value("text", std::string());
            if (peer.empty() || text.empty()) {
                return failure(400, "a message is a \"peer\" and a \"text\"");
            }
            const Runtime::Sent sent = runtime_.sendText(peer, text);
            return json({{"messageId", sent.messageId}, {"confirmed", sent.confirmed}});
        });
    });

    server_->post("/ui/contacts", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            if (body.contains("invite")) {
                return json({{"fingerprint",
                    runtime_.addByInvite(body.at("invite").get<std::string>(),
                        body.value("text", std::string()))}});
            }
            if (body.contains("accept")) {
                runtime_.acceptContact(body.at("accept").get<std::string>());
                return json({{"accepted", body.at("accept")}});
            }
            return failure(400, "a contact is added by an \"invite\" or accepted by \"accept\"");
        });
    });

    server_->post("/ui/messages/([0-9a-f]+)", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            runtime_.editMessage(body.value("peer", std::string()), call.captures.at(0),
                body.value("text", std::string()));
            return json({{"edited", call.captures.at(0)}});
        });
    });

    server_->del("/ui/messages/([0-9a-f]+)", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            runtime_.deleteMessage(call.query("peer"), call.captures.at(0));
            return json({{"deleted", call.captures.at(0)}});
        });
    });

    server_->post("/ui/reactions", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            runtime_.react(body.value("peer", std::string()),
                body.value("messageId", std::string()), body.value("emoji", std::string()));
            return json({{"ok", true}});
        });
    });

    // The interface waits for new lines the same way a bot does.
    server_->getAsync("/ui/events",
        [this](const http::Request& request, http::Responder respond)
            -> std::optional<http::Response> {
            if (!signedIn(request)) {
                return failure(401, "sign in first");
            }
            if (!runtime_.running()) {
                return failure(503, "the account is not open: " + runtime_.status());
            }
            const std::int64_t since = longParam(request, "since");
            const int wait = intParam(request, "wait", kDefaultWaitSeconds);
            std::thread([this, since, wait, respond = std::move(respond)]() {
                try {
                    const std::vector<Event> events = runtime_.await(since, wait);
                    respond(json({{"events", eventsJson(events)},
                        {"lastEventId", events.empty() ? since : events.back().id}}));
                } catch (const std::exception& error) {
                    respond(failure(503, error.what()));
                }
            }).detach();
            return std::nullopt;
        });

    // ---- the interface -----------------------------------------------------
    // One address for the panel: the sign-in page until there is a session, the
    // panel itself afterwards. A page that draws its own empty shell and then
    // finds out it is not signed in is a page that flickers.
    server_->get("/", [this](const http::Request& request) {
        http::Response response;
        response.contentType = "text/html; charset=utf-8";
        response.body = assets_.file(signedIn(request) ? "index.html" : "login.html").body;
        return response;
    });
    server_->get("/assets/([A-Za-z0-9._-]+)", [this](const http::Request& request) {
        try {
            const WebAssets::File& file = assets_.file(request.captures.at(0));
            http::Response response;
            response.contentType = file.contentType;
            response.body = file.body;
            return response;
        } catch (const std::exception&) {
            return failure(404, "no such file");
        }
    });
}

}  // namespace bazarish::daemon
