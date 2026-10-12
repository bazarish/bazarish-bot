// Bazarish project (c) 2026
#include "HttpFace.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Log.hpp>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <ctime>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bazarish::bot {

namespace {

constexpr std::int64_t kSessionSeconds = 12 * 60 * 60;
constexpr std::size_t kSessionSecretBytes = 32;
constexpr const char* kOperator = "operator";
constexpr const char* kSessionCookie = "bazarish_bot";
constexpr int kDefaultWaitSeconds = 30;
constexpr int kDefaultPage = 50;
constexpr const char* kAccountPath = "/ui/accounts/([0-9a-f]+)";

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

nlohmann::json peersJson(const std::vector<Account::Peer>& peers)
{
    nlohmann::json array = nlohmann::json::array();
    for (const Account::Peer& peer : peers) {
        array.push_back({{"fingerprint", peer.fingerprint}, {"name", peer.name},
            {"pending", peer.pending}, {"writable", peer.writable}});
    }
    return array;
}

nlohmann::json pairingJson(const Account::Pairing& pairing)
{
    return {
        {"active", pairing.active},
        {"uri", pairing.uri},
        {"code", pairing.code},
        {"stage", pairing.stage},
        {"done", pairing.done},
        {"total", pairing.total},
        {"wrongCodes", pairing.wrongCodes},
        {"error", pairing.error},
    };
}

nlohmann::json pairingImportJson(const Runtime::PairingImport& pairing)
{
    return {
        {"active", pairing.active},
        {"needsCode", pairing.needsCode},
        {"stage", pairing.stage},
        {"done", pairing.done},
        {"total", pairing.total},
        {"triesLeft", pairing.triesLeft},
        {"error", pairing.error},
        {"account", pairing.account},
    };
}

Settings::AccountKind kindParam(const nlohmann::json& body)
{
    const auto kind = accountKindFromName(body.value("kind", std::string("bot")));
    if (!kind) {
        throw std::runtime_error("an account is a \"bot\" or a \"group\"");
    }
    return *kind;
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

void answerEvents(const std::shared_ptr<Account>& account, const http::Request& request,
    http::Responder respond)
{
    const std::int64_t since = longParam(request, "since");
    const int wait = intParam(request, "wait", kDefaultWaitSeconds);
    std::thread([account, since, wait, respond = std::move(respond)]() {
        try {
            const std::vector<Event> events = account->await(since, wait);
            respond(json({{"events", eventsJson(events)},
                {"lastEventId", events.empty() ? since : events.back().id}}));
        } catch (const std::exception& error) {
            respond(failure(503, error.what()));
        }
    }).detach();
}

nlohmann::json accountJson(Account& account, Settings& settings)
{
    nlohmann::json tokens = nlohmann::json::array();
    for (const ApiToken& token : settings.tokens(account.id())) {
        tokens.push_back({{"name", token.name}, {"createdAt", token.createdAt}});
    }
    const bool running = account.running();
    const Account::PendingRouting owed = account.pendingRouting();
    return {
        {"id", account.id()},
        {"kind", accountKindName(account.kind())},
        {"fingerprint", running ? account.fingerprint() : settings.fingerprintOf(account.id())},
        {"name", account.displayName()},
        {"destination", account.destination()},
        {"invite", running ? account.inviteUri() : std::string()},
        {"running", running},
        {"status", account.status()},
        {"pendingRouting", owed.contacts},
        {"pendingRegistry", owed.registry},
        {"tokens", tokens},
    };
}

}  // namespace

HttpFace::HttpFace(Runtime& runtime, std::filesystem::path assets)
    : runtime_(runtime)
    , operator_(runtime.config().path, runtime.config().operatorFingerprint,
          runtime.config().consumer)
    , assets_(std::move(assets))
    , sessions_(sessionSecret(), kSessionSeconds)
{
}

HttpFace::~HttpFace()
{
    stop();
}

std::shared_ptr<Account> HttpFace::accountForToken(const http::Request& request) const
{
    const std::string header = request.header("authorization");
    static const std::string kBearer = "Bearer ";
    if (header.size() <= kBearer.size() || header.compare(0, kBearer.size(), kBearer) != 0) {
        return nullptr;
    }
    return runtime_.accountForToken(header.substr(kBearer.size()));
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
    apiRoutes();
    operatorRoutes();
    accountRoutes();
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

void HttpFace::apiRoutes()
{
    using Handler = std::function<http::Response(Account&, const http::Request&)>;
    const auto guarded = [this](const http::Request& request, const Handler& handler) {
        const std::shared_ptr<Account> account = accountForToken(request);
        if (account == nullptr) {
            return failure(401, request.hasHeader("authorization")
                    ? "this token does not open the API"
                    : "the API is opened by a token in the Authorization header");
        }
        if (!account->running()) {
            return failure(503, "the account is not open: " + account->status());
        }
        try {
            return handler(*account, request);
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    };

    server_->get("/v1/status", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request&) {
            return json({
                {"fingerprint", account.fingerprint()},
                {"name", account.displayName()},
                {"kind", accountKindName(account.kind())},
                {"destination", account.destination()},
                {"status", account.status()},
                {"lastEventId", account.lastEventId()},
            });
        });
    });

    server_->get("/v1/invite", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request&) {
            return json({{"invite", account.inviteUri()}});
        });
    });

    server_->get("/v1/contacts", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request&) {
            return json(peersJson(account.contacts()));
        });
    });

    server_->post("/v1/contacts", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            if (body.contains("invite")) {
                return json({{"fingerprint",
                    account.addByInvite(body.at("invite").get<std::string>(),
                        body.value("text", std::string()))}});
            }
            if (body.contains("accept")) {
                account.acceptContact(body.at("accept").get<std::string>());
                return json({{"accepted", body.at("accept")}});
            }
            return failure(400, "a contact is added by an \"invite\" or accepted by \"accept\"");
        });
    });

    server_->get("/v1/chats", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request&) {
            return json(linesJson(account.chats()));
        });
    });

    server_->get("/v1/messages", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const std::string peer = call.query("peer");
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation to read");
            }
            return json(linesJson(account.conversation(peer, longParam(call, "before"),
                intParam(call, "limit", kDefaultPage))));
        });
    });

    server_->post("/v1/messages", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string text = body.value("text", std::string());
            if (peer.empty() || text.empty()) {
                return failure(400, "a message is a \"peer\" and a \"text\"");
            }
            const Account::Sent sent = body.contains("keyboard")
                ? account.sendKeyboard(peer, text, body.at("keyboard").dump())
                : account.sendText(peer, text);
            return json({{"messageId", sent.messageId}, {"confirmed", sent.confirmed}});
        });
    });

    server_->post("/v1/messages/([0-9a-f]+)", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation the message is in");
            }
            account.editMessage(peer, call.captures.at(0), body.value("text", std::string()));
            return json({{"edited", call.captures.at(0)}});
        });
    });

    server_->del("/v1/messages/([0-9a-f]+)", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const std::string peer = call.query("peer");
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation the message is in");
            }
            account.deleteMessage(peer, call.captures.at(0));
            return json({{"deleted", call.captures.at(0)}});
        });
    });

    server_->post("/v1/reactions", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string messageId = body.value("messageId", std::string());
            if (peer.empty() || messageId.empty()) {
                return failure(400, "a reaction is a \"peer\", a \"messageId\" and an \"emoji\"");
            }
            account.react(peer, messageId, body.value("emoji", std::string()));
            return json({{"ok", true}});
        });
    });

    server_->post("/v1/chats/([a-z2-7]+)/clear", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            account.clearChat(call.captures.at(0));
            return json({{"cleared", call.captures.at(0)}});
        });
    });

    server_->post("/v1/receipts", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string messageId = body.value("messageId", std::string());
            if (peer.empty() || messageId.empty()) {
                return failure(400, "a receipt is a \"peer\" and a \"messageId\"");
            }
            account.confirmReceipt(peer, messageId);
            return json({{"ok", true}});
        });
    });

    server_->getAsync("/v1/events",
        [this](const http::Request& request, http::Responder respond)
            -> std::optional<http::Response> {
            const std::shared_ptr<Account> account = accountForToken(request);
            if (account == nullptr) {
                return failure(401, "the API is opened by a token in the Authorization header");
            }
            if (!account->running()) {
                return failure(503, "the account is not open: " + account->status());
            }
            answerEvents(account, request, std::move(respond));
            return std::nullopt;
        });
}

void HttpFace::operatorRoutes()
{
    using Handler = std::function<http::Response(const http::Request&)>;
    const auto operatorOnly = [this](const http::Request& request, const Handler& handler) {
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
            {"claimed", operator_.claimed()},
            {"signedIn", signedIn(request)},
            {"challenge", operator_.issueChallenge(nowSeconds())},
        });
    });

    server_->post("/ui/login", [this](const http::Request& request) {
        try {
            const nlohmann::json body = nlohmann::json::parse(request.body);
            operator_.signIn(body.value("challenge", std::string()),
                body.value("blob", std::string()), nowSeconds());
            http::Response response = json({{"ok", true}});
            response.headers["Set-Cookie"] = std::string(kSessionCookie) + "="
                + sessions_.issue(kOperator, nowSeconds())
                + "; Path=/; HttpOnly; SameSite=Strict";
            return response;
        } catch (const std::exception& error) {
            log::warn("panel sign-in refused: {}", error.what());
            return failure(401, error.what());
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
            bool anyRunning = false;
            for (const Runtime::Summary& summary : runtime_.accounts()) {
                anyRunning = anyRunning || summary.running;
            }
            return json({
                {"transport", settings.transport() == Settings::Transport::eSam ? "sam"
                                                                                : "embedded"},
                {"samHost", settings.samHost()},
                {"samPort", settings.samPort()},
                {"privacy", std::string(privacyName(settings.privacy()))},
                {"operator", operator_.operatorFingerprint()},
                {"restartNeeded", anyRunning},
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
                    body.value("samHost", std::string(kDefaultSamHost)),
                    body.value("samPort", kDefaultSamPort));
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

    server_->get("/ui/accounts", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            nlohmann::json array = nlohmann::json::array();
            for (const Runtime::Summary& summary : runtime_.accounts()) {
                array.push_back({
                    {"id", summary.id},
                    {"name", summary.name},
                    {"fingerprint", summary.fingerprint},
                    {"kind", accountKindName(summary.kind)},
                    {"running", summary.running},
                    {"status", summary.status},
                });
            }
            return json(array);
        });
    });

    server_->post("/ui/accounts", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string action = body.value("action", std::string());
            if (action == "create") {
                return json({{"id",
                    runtime_.createAccount(body.value("name", std::string()), kindParam(body))}});
            }
            if (action == "import") {
                return json({{"id",
                    runtime_.importAccount(fromBase64(body.value("bundle", std::string())),
                        body.value("bundlePassphrase", std::string()), kindParam(body))}});
            }
            return failure(400, "the action is create or import");
        });
    });

    server_->get("/ui/pairing", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            return json(pairingImportJson(runtime_.pairingImport()));
        });
    });

    server_->post("/ui/pairing", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            if (body.contains("code")) {
                runtime_.submitPairCode(body.at("code").get<std::string>());
            } else {
                runtime_.startPairingImport(body.value("link", std::string()), kindParam(body));
            }
            return json(pairingImportJson(runtime_.pairingImport()));
        });
    });

    server_->del("/ui/pairing", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            runtime_.cancelPairingImport();
            return json({{"ok", true}});
        });
    });

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

void HttpFace::accountRoutes()
{
    using Handler = std::function<http::Response(Account&, const http::Request&)>;
    const auto forAccount = [this](const http::Request& request, const Handler& handler) {
        if (!signedIn(request)) {
            return failure(401, "sign in first");
        }
        try {
            const std::shared_ptr<Account> account = runtime_.account(request.captures.at(0));
            return handler(*account, request);
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    };
    const std::string base = kAccountPath;

    server_->get(base, [this, forAccount](const http::Request& request) {
        return forAccount(request, [this](Account& account, const http::Request&) {
            return json(accountJson(account, runtime_.settings()));
        });
    });

    server_->post(base, [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string action = body.value("action", std::string());
            if (action == "open") {
                account.open();
                return json({{"status", account.status()}});
            }
            if (action == "kind") {
                account.setKind(kindParam(body));
                return json({{"kind", accountKindName(account.kind())}});
            }
            if (action == "rename") {
                account.rename(body.value("name", std::string()));
                return json({{"name", body.value("name", std::string())}});
            }
            if (action == "new-address") {
                account.newAddress();
                return json({{"address", account.destination()}});
            }
            if (action == "retry-routing") {
                account.retryRouting(body.value("peer", std::string()));
                return json({{"sending", true}});
            }
            return failure(400, "the action is open, kind, rename, new-address or retry-routing");
        });
    });

    server_->del(base, [this](const http::Request& request) {
        if (!signedIn(request)) {
            return failure(401, "sign in first");
        }
        try {
            runtime_.removeAccount(request.captures.at(0));
            return json({{"ok", true}});
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    });

    server_->post(base + "/export", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const Bytes bundle = account.exportBundle(body.value("bundlePassphrase", std::string()));
            http::Response response;
            response.contentType = "application/octet-stream";
            response.headers["Content-Disposition"]
                = "attachment; filename=\"" + backupFileName(account.displayName()) + "\"";
            response.body.assign(bundle.begin(), bundle.end());
            return response;
        });
    });

    server_->get(base + "/pairing", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return json(pairingJson(account.pairing()));
        });
    });

    server_->post(base + "/pairing", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return json(pairingJson(account.startPairing()));
        });
    });

    server_->del(base + "/pairing", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            account.stopPairing();
            return json({{"ok", true}});
        });
    });

    server_->post(base + "/tokens", [this, forAccount](const http::Request& request) {
        return forAccount(request, [this](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string name = body.value("name", std::string());
            if (name.empty()) {
                return failure(400, "a token is issued under a name");
            }
            const ApiToken token = runtime_.settings().issueToken(account.id(), name);
            return json({{"name", token.name}, {"token", token.secret},
                {"createdAt", token.createdAt}});
        });
    });

    server_->del(base + "/tokens/([^/]+)", [this, forAccount](const http::Request& request) {
        return forAccount(request, [this](Account& account, const http::Request& call) {
            if (!runtime_.settings().revokeToken(account.id(), call.captures.at(1))) {
                return failure(404, "no token by that name");
            }
            return json({{"ok", true}});
        });
    });

    server_->get(base + "/contacts", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return json(peersJson(account.contacts()));
        });
    });

    server_->post(base + "/contacts", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            if (body.contains("invite")) {
                return json({{"fingerprint",
                    account.addByInvite(body.at("invite").get<std::string>(),
                        body.value("text", std::string()))}});
            }
            if (body.contains("accept")) {
                account.acceptContact(body.at("accept").get<std::string>());
                return json({{"accepted", body.at("accept")}});
            }
            return failure(400, "a contact is added by an \"invite\" or accepted by \"accept\"");
        });
    });

    server_->get(base + "/chats", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return json(linesJson(account.chats()));
        });
    });

    server_->get(base + "/messages", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const std::string peer = call.query("peer");
            if (peer.empty()) {
                return failure(400, "\"peer\" names whose conversation to read");
            }
            return json(linesJson(account.conversation(peer, longParam(call, "before"),
                intParam(call, "limit", kDefaultPage))));
        });
    });

    server_->post(base + "/messages", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string text = body.value("text", std::string());
            if (peer.empty() || text.empty()) {
                return failure(400, "a message is a \"peer\" and a \"text\"");
            }
            const Account::Sent sent = account.sendText(peer, text);
            return json({{"messageId", sent.messageId}, {"confirmed", sent.confirmed}});
        });
    });

    server_->post(base + "/messages/([0-9a-f]+)", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            account.editMessage(body.value("peer", std::string()), call.captures.at(1),
                body.value("text", std::string()));
            return json({{"edited", call.captures.at(1)}});
        });
    });

    server_->del(base + "/messages/([0-9a-f]+)", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            account.deleteMessage(call.query("peer"), call.captures.at(1));
            return json({{"deleted", call.captures.at(1)}});
        });
    });

    server_->post(base + "/reactions", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            account.react(body.value("peer", std::string()),
                body.value("messageId", std::string()), body.value("emoji", std::string()));
            return json({{"ok", true}});
        });
    });

    server_->getAsync(base + "/events",
        [this](const http::Request& request, http::Responder respond)
            -> std::optional<http::Response> {
            if (!signedIn(request)) {
                return failure(401, "sign in first");
            }
            std::shared_ptr<Account> account;
            try {
                account = runtime_.account(request.captures.at(0));
            } catch (const std::exception& error) {
                return failure(404, error.what());
            }
            if (!account->running()) {
                return failure(503, "the account is not open: " + account->status());
            }
            answerEvents(account, request, std::move(respond));
            return std::nullopt;
        });
}

}  // namespace bazarish::bot
