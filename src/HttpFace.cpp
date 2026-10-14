// Bazarish project (c) 2026
#include "HttpFace.hpp"
#include "Backup.hpp"
#include "GroupCopy.hpp"
#include "SystemInfo.hpp"
#include "Version.hpp"

#include <bazarish/Bytes.hpp>
#include <bazarish/Log.hpp>
#include <bazarish/Portal.hpp>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr std::int64_t kSessionSeconds = 12 * 60 * 60;
constexpr std::size_t kSessionSecretBytes = 32;
constexpr const char* kOperator = "operator";
constexpr const char* kSessionCookie = "bazarish_bot";
constexpr int kDefaultWaitSeconds = 30;
constexpr int kDefaultPage = 50;
constexpr const char* kAccountPath = "/ui/accounts/([0-9a-f]+)";
constexpr std::uint64_t kMebibyte = 1024 * 1024;
constexpr std::uint64_t kMaxBackupBytes = 512 * kMebibyte;

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
            {"pending", peer.pending}, {"accepting", peer.accepting},
            {"writable", peer.writable}});
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

http::Response addContact(Account& account, const nlohmann::json& body)
{
    const std::string text = body.value("text", std::string());
    if (body.contains("invite")) {
        return json({{"fingerprint",
            account.addByInvite(body.at("invite").get<std::string>(), text)}});
    }
    if (body.contains("alias")) {
        return json({{"fingerprint",
            account.addByAlias(body.at("alias").get<std::string>(), text)}});
    }
    if (body.contains("accept")) {
        account.acceptContact(body.at("accept").get<std::string>());
        return json({{"accepted", body.at("accept")}});
    }
    return failure(400,
        "a contact is added by an \"invite\" or an \"alias\", or accepted by \"accept\"");
}

nlohmann::json aliasesJson(const Account::Aliases& aliases)
{
    nlohmann::json held = nlohmann::json::array();
    for (const Account::Alias& alias : aliases.held) {
        held.push_back({{"alias", alias.alias}, {"notAfter", alias.notAfter},
            {"autoRenew", alias.autoRenew}, {"resolving", alias.resolving},
            {"live", alias.live}});
    }
    nlohmann::json body = {{"aliases", held}, {"depositCovers", aliases.depositCovers},
        {"checking", aliases.checking}};
    if (aliases.checked) {
        body["checked"] = {{"ok", aliases.checked->ok}, {"note", aliases.checked->note}};
    }
    return body;
}

nlohmann::json consumerJson(const std::string& challenge)
{
    try {
        const service::LoginConsumer consumer = service::readLoginConsumer(challenge);
        return {{"ok", true}, {"name", consumer.name}, {"place", consumer.place},
            {"role", consumer.role}};
    } catch (const std::exception& error) {
        return {{"ok", false}, {"problem", error.what()}};
    }
}

nlohmann::json groupConfigJson(const GroupConfig& config)
{
    return {
        {"kind", groupKindName(config.kind)},
        {"joinMode", joinModeName(config.joinMode)},
        {"historySize", config.historySize},
        {"contactRequests", config.contactRequests},
        {"language", config.language},
        {"greeting", config.greeting},
        {"welcome", config.welcome},
        {"greetingDefault", copyText(config.language, Copy::eAskName)},
        {"welcomeDefault", copyText(config.language, Copy::eWelcome, {"{name}"})},
    };
}

nlohmann::json membersJson(const std::vector<Member>& members)
{
    nlohmann::json rows = nlohmann::json::array();
    for (const Member& member : members) {
        rows.push_back({
            {"fingerprint", member.fingerprint},
            {"name", member.name},
            {"state", memberStateName(member.state)},
            {"admin", member.admin},
            {"contactable", member.contactable},
            {"joinedAt", member.joinedAt},
        });
    }
    return rows;
}

std::string nameOf(const std::vector<Member>& members, const std::string& fingerprint)
{
    for (const Member& member : members) {
        if (member.fingerprint == fingerprint) {
            return member.name;
        }
    }
    return {};
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
        {"acceptsContacts", settings.acceptsContacts(account.id())},
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
        if (account->kind() == Settings::AccountKind::eGroup && request.path != "/v1/status") {
            return failure(409, "a group relays its members itself; the API does not write for it");
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
            return addContact(account, nlohmann::json::parse(call.body));
        });
    });

    server_->del("/v1/contacts/([a-z2-7]+)", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            account.removeContact(call.captures.at(0));
            return json({{"removed", call.captures.at(0)}});
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
                : account.sendText(peer, text, body.value("replyTo", std::string()));
            return json({{"messageId", sent.messageId}, {"confirmed", sent.confirmed}});
        });
    });

    server_->post("/v1/forward", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string from = body.value("from", std::string());
            const std::string messageId = body.value("messageId", std::string());
            const std::string to = body.value("to", std::string());
            if (from.empty() || messageId.empty() || to.empty()) {
                return failure(400, "a forward is \"from\", \"messageId\" and \"to\"");
            }
            const Account::Sent sent = account.forward(from, messageId, to);
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
            if (account->kind() == Settings::AccountKind::eGroup) {
                return failure(
                    409, "a group relays its members itself; the API does not write for it");
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
        const bool in = signedIn(request);
        nlohmann::json state = {
            {"claimed", operator_.claimed()},
            {"signedIn", in},
            {"challenge", operator_.issueChallenge(nowSeconds())},
        };
        if (in) {
            state["version"] = kVersion;
        }
        return json(state);
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

    server_->get("/ui/system", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            const Runtime::RouterReport router = runtime_.routerReport();
            const MemoryUse memory = memoryUse();
            const DiskUse disk = diskUse(runtime_.config().stateDir);
            nlohmann::json routerJson = {{"counted", router.counted},
                {"destinations", router.destinations},
                {"readyDestinations", router.readyDestinations}};
            if (router.counted) {
                routerJson["inboundTunnels"] = router.inboundTunnels;
                routerJson["outboundTunnels"] = router.outboundTunnels;
                routerJson["transitTunnels"] = router.transitTunnels;
                routerJson["knownRouters"] = router.knownRouters;
                routerJson["floodfills"] = router.floodfills;
                routerJson["connections"] = router.connections;
            }
            return json({
                {"version", kVersion},
                {"uptimeSeconds", runtime_.uptimeSeconds()},
                {"transport", runtime_.settings().transport() == Settings::Transport::eSam
                        ? "sam"
                        : "embedded"},
                {"router", routerJson},
                {"memory", {{"residentBytes", memory.residentBytes},
                               {"peakBytes", memory.peakBytes}}},
                {"disk", {{"stateBytes", disk.stateBytes}, {"freeBytes", disk.freeBytes},
                             {"totalBytes", disk.totalBytes}}},
            });
        });
    });

    server_->get("/ui/system/backup", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            const Bytes archive = runtime_.exportEverything();
            http::Response response;
            response.contentType = "application/zip";
            response.headers["Content-Disposition"]
                = "attachment; filename=\"bazarish-bot-backup.zip\"";
            response.body.assign(archive.begin(), archive.end());
            return response;
        });
    });

    server_->post("/ui/system/restore", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const std::uint64_t offset = std::stoull(call.query("offset"));
            const fs::path upload = runtime_.config().stateDir / kRestoreUpload;
            const std::uint64_t held
                = offset == 0 || !fs::exists(upload) ? 0 : fs::file_size(upload);
            if (offset != held) {
                return failure(409, "the upload holds " + std::to_string(held) + " bytes");
            }
            if (offset + call.body.size() > kMaxBackupBytes) {
                return failure(413, "a backup is at most "
                        + std::to_string(kMaxBackupBytes / kMebibyte) + " MiB");
            }
            std::ofstream out(upload, std::ios::binary
                    | (offset == 0 ? std::ios::trunc : std::ios::app));
            fs::permissions(upload, fs::perms::owner_read | fs::perms::owner_write);
            out.write(call.body.data(), static_cast<std::streamsize>(call.body.size()));
            if (!out) {
                return failure(500, "the upload could not be written");
            }
            return json({{"held", offset + call.body.size()}});
        });
    });

    server_->post("/ui/system/restore/commit", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            const fs::path upload = runtime_.config().stateDir / kRestoreUpload;
            std::ifstream in(upload, std::ios::binary);
            if (!in) {
                return failure(409, "nothing has been uploaded");
            }
            checkBackup(Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
            log::warn("a backup was uploaded: restoring it and restarting");
            restoreReady_();
            return json({{"restarting", true}});
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

    server_->post(base, [this, forAccount](const http::Request& request) {
        return forAccount(request, [this](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string action = body.value("action", std::string());
            if (action == "open") {
                account.open();
                return json({{"status", account.status()}});
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
            if (action == "accept-contacts" && account.kind() == Settings::AccountKind::eBot) {
                const bool accepts = body.at("accepts").get<bool>();
                runtime_.settings().setAcceptsContacts(account.id(), accepts);
                return json({{"acceptsContacts", accepts}});
            }
            return failure(400,
                "the action is open, rename, new-address, retry-routing or accept-contacts");
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

    server_->post(base + "/sign", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string challenge = body.value("challenge", std::string());
            nlohmann::json described = consumerJson(challenge);
            if (body.value("sign", false)) {
                if (!described.at("ok").get<bool>()) {
                    return failure(400, described.at("problem").get<std::string>());
                }
                described["signature"] = account.signLogin(challenge);
            }
            return json(described);
        });
    });

    server_->get(base + "/aliases", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return json(aliasesJson(account.aliases()));
        });
    });

    server_->post(base + "/aliases", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            if (body.value("check", false)) {
                account.checkAliases();
                return json(aliasesJson(account.aliases()));
            }
            if (body.contains("alias") && body.contains("resolving")) {
                account.setAliasBinding(
                    body.at("alias").get<std::string>(), body.at("resolving").get<bool>());
                return json(aliasesJson(account.aliases()));
            }
            return failure(400, "\"check\" asks the registry; \"alias\" with \"resolving\" "
                                "points one here or nowhere");
        });
    });

    server_->get(base + "/group", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            GroupEngine& group = account.group();
            nlohmann::json states = nlohmann::json::object();
            for (const Member& member : group.members()) {
                states[memberStateName(member.state)]
                    = states.value(memberStateName(member.state), 0) + 1;
            }
            return json({{"config", groupConfigJson(group.config())},
                {"memberCount", group.memberCount()}, {"states", states},
                {"requests", group.requests().size()}});
        });
    });

    server_->post(base + "/group", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            GroupEngine& group = account.group();
            GroupConfig config = group.config();
            if (body.contains("kind")) {
                config.kind = groupKindFromName(body.at("kind").get<std::string>());
            }
            if (body.contains("joinMode")) {
                config.joinMode = joinModeFromName(body.at("joinMode").get<std::string>());
            }
            if (body.contains("historySize")) {
                config.historySize = body.at("historySize").get<std::int64_t>();
            }
            if (body.contains("contactRequests")) {
                config.contactRequests = body.at("contactRequests").get<bool>();
            }
            if (body.contains("language")) {
                config.language = body.at("language").get<std::string>();
            }
            if (body.contains("greeting")) {
                config.greeting = body.at("greeting").get<std::string>();
            }
            if (body.contains("welcome")) {
                config.welcome = body.at("welcome").get<std::string>();
            }
            group.setConfig(config);
            return json(groupConfigJson(group.config()));
        });
    });

    server_->get(base + "/group/members", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return json(membersJson(account.group().members()));
        });
    });

    server_->post(base + "/group/members", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            const std::string fingerprint = body.value("fingerprint", std::string());
            const std::string action = body.value("action", std::string());
            GroupEngine& group = account.group();
            if (action == "admin" || action == "unadmin") {
                group.setAdmin(fingerprint, action == "admin");
            } else if (action == "exclude") {
                group.exclude(fingerprint);
            } else if (action == "block") {
                group.block(fingerprint);
            } else if (action == "unblock") {
                group.unblock(fingerprint);
            } else {
                return failure(400, "the action is admin, unadmin, exclude, block or unblock");
            }
            return json(membersJson(group.members()));
        });
    });

    server_->get(base + "/group/requests", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            GroupEngine& group = account.group();
            const std::vector<Member> members = group.members();
            nlohmann::json rows = nlohmann::json::array();
            for (const JoinRequest& asked : group.requests()) {
                rows.push_back({{"number", asked.number}, {"fingerprint", asked.fingerprint},
                    {"name", nameOf(members, asked.fingerprint)}, {"at", asked.at}});
            }
            return json(rows);
        });
    });

    server_->post(base + "/group/requests", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = nlohmann::json::parse(call.body);
            account.group().decide(body.at("number").get<std::int64_t>(), body.at("allow").get<bool>());
            return json({{"ok", true}});
        });
    });

    server_->get(base + "/group/posts", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            GroupEngine& group = account.group();
            const std::vector<Member> members = group.members();
            nlohmann::json rows = nlohmann::json::array();
            for (const Post& post : group.posts(intParam(call, "limit", kDefaultPage))) {
                rows.push_back({{"id", post.id}, {"author", post.author},
                    {"authorName", nameOf(members, post.author)}, {"type", post.type},
                    {"text", post.text}, {"sentAt", post.sentAt}, {"edited", post.edited},
                    {"deleted", post.deleted}, {"reactions", group.reactionCounts(post.id)}});
            }
            return json(rows);
        });
    });

    server_->del(base + "/group/posts/([^/]+)", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            account.group().deletePost(call.captures.at(1));
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
            return addContact(account, nlohmann::json::parse(call.body));
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
