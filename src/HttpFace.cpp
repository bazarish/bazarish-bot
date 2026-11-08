// Bazarish project (c) 2026
#include "HttpFace.hpp"
#include "Backup.hpp"
#include "FileRules.hpp"
#include "PictureRules.hpp"
#include "SystemInfo.hpp"

#include <BuildVersion.hpp>
#include <QrSvg.hpp>
#include <bazarish/Bytes.hpp>
#include <bazarish/Clock.hpp>
#include <bazarish/HttpClient.hpp>
#include <bazarish/HttpResponses.hpp>
#include <bazarish/JsonRead.hpp>
#include <bazarish/Log.hpp>
#include <bazarish/Portal.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
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
constexpr const char* kAccountPath = "/ui/accounts/{hex}";
constexpr const char* kStoragePath = "/v1/storage";
constexpr const char* kAvatarName = "avatar";
constexpr const char* kStatusPath = "/v1/status";
constexpr const char* kTimerEvent = "timer";
constexpr std::uint64_t kMebibyte = 1024 * 1024;
constexpr std::uint64_t kMaxBackupBytes = 512 * kMebibyte;
constexpr char kLoopbackV4[] = "127.0.0.1";
constexpr char kLoopbackV6[] = "::1";
constexpr char kLocalhost[] = "localhost";

std::vector<std::string> answeredHosts(const Config& config)
{
    std::vector<std::string> hosts;
    if (config.listenHost == kLoopbackV6) {
        hosts.push_back("[" + config.listenHost + "]");
    } else {
        hosts.push_back(config.listenHost);
    }
    if (config.listenHost == kLoopbackV4 || config.listenHost == kLoopbackV6) {
        hosts.emplace_back(kLocalhost);
    }
    for (const std::string& place : config.consumer.place) {
        hosts.push_back(http::parseUrl(place).host);
    }
    return hosts;
}

std::string sessionSecret()
{
    return toHex(randomBytes(kSessionSecretBytes));
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
    return http::jsonResponse({{"error", message}}, status);
}

http::Response tokenRefused(const http::Request& request)
{
    return failure(401, request.hasHeader("authorization")
            ? "this token does not open the API"
            : "the API is opened by a token in the Authorization header");
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
        item["keyboard"] = bazarish::parseJsonOrDiscarded(line.keyboard);
    }
    if (!line.reactions.empty()) {
        nlohmann::json reactions = nlohmann::json::array();
        for (const Reaction& reaction : line.reactions) {
            reactions.push_back({{"reactor", reaction.reactor}, {"emoji", reaction.emoji}});
        }
        item["reactions"] = reactions;
    }
    if (line.file.has_value()) {
        nlohmann::json file = {{"name", line.file->name}, {"size", line.file->size}};
        for (const auto& [key, value] : {std::pair{"mime", std::cref(line.file->mime)},
                 std::pair{"sha256", std::cref(line.file->sha256)},
                 std::pair{"path", std::cref(line.file->path)}}) {
            if (!value.get().empty()) {
                file[key] = value.get();
            }
        }
        item["file"] = file;
    }
    return item;
}

nlohmann::json storageJson(const client::DatabaseFootprint& footprint)
{
    return {{"fileBytes", footprint.fileBytes}, {"freeBytes", footprint.freeBytes}};
}

bool clearsForEveryone(const http::Request& request)
{
    const nlohmann::json body = bazarish::parseJson(request.body);
    if (!body.contains("everyone") || !body.at("everyone").is_boolean()) {
        throw std::invalid_argument("\"everyone\" says whether the chat goes on both sides");
    }
    return body.at("everyone").get<bool>();
}

nlohmann::json transferJson(const Transfer& transfer)
{
    nlohmann::json item = {
        {"peer", transfer.peer},
        {"messageId", transfer.messageId},
        {"direction", transfer.outgoing ? "send" : "receive"},
        {"state", transfer.state},
        {"bytes", transfer.bytes},
        {"total", transfer.total},
    };
    for (const auto& [key, value] : {std::pair{"stage", std::cref(transfer.stage)},
             std::pair{"error", std::cref(transfer.error)},
             std::pair{"path", std::cref(transfer.path)}}) {
        if (!value.get().empty()) {
            item[key] = value.get();
        }
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
    if (!event.type.empty()) {
        item["type"] = event.type;
    }
    if (event.kind == kTimerEvent) {
        item["seconds"] = std::stoll(event.text);
    } else if (!event.text.empty()) {
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
        throw std::invalid_argument("\"" + key + "\" is not a number");
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
        throw std::invalid_argument("\"" + key + "\" is not a number");
    }
}

}  // namespace

std::optional<http::Response> HttpFace::answerEvents(const std::shared_ptr<Account>& account,
    const http::Request& request, http::Responder respond)
{
    std::int64_t since = 0;
    int wait = 0;
    try {
        since = longParam(request, "since");
        wait = intParam(request, "wait", kDefaultWaitSeconds);
    } catch (const std::invalid_argument& error) {
        return failure(400, error.what());
    }
    reapPolls(false);
    const std::shared_ptr<std::atomic<bool>> done = std::make_shared<std::atomic<bool>>(false);
    std::thread poll([account, since, wait, respond = std::move(respond), done]() {
        try {
            const std::vector<Event> events = account->await(since, wait);
            respond(http::jsonResponse({{"events", eventsJson(events)},
                {"lastEventId", events.empty() ? since : events.back().id}}));
        } catch (const std::exception& error) {
            respond(failure(503, error.what()));
        }
        done->store(true);
    });
    const std::lock_guard<std::mutex> lock(pollsMutex_);
    polls_.push_back(Poll{std::move(poll), done});
    return std::nullopt;
}

void HttpFace::reapPolls(const bool all)
{
    std::list<Poll> finished;
    {
        const std::lock_guard<std::mutex> lock(pollsMutex_);
        for (auto it = polls_.begin(); it != polls_.end();) {
            if (all || it->done->load()) {
                finished.splice(finished.end(), polls_, it++);
            } else {
                ++it;
            }
        }
    }
    for (Poll& poll : finished) {
        poll.thread.join();
    }
}

namespace {

http::Response addContact(Account& account, const nlohmann::json& body)
{
    const std::string text = body.value("text", std::string());
    if (body.contains("invite")) {
        return http::jsonResponse({{"fingerprint",
            account.addContact(false, body.at("invite").get<std::string>(), text)}});
    }
    if (body.contains("alias")) {
        return http::jsonResponse({{"fingerprint",
            account.addContact(true, body.at("alias").get<std::string>(), text)}});
    }
    if (body.contains("accept")) {
        account.acceptContact(body.at("accept").get<std::string>());
        return http::jsonResponse({{"accepted", body.at("accept")}});
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
    };
}

nlohmann::json linkJson(const Runtime::RouterReport& router)
{
    return {
        {"transport", router.transport == Settings::Transport::eSam ? "sam" : "embedded"},
        {"running", router.running},
        {"online", router.online},
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
            {"joinedAt", member.joinedAt},
            {"offlineSince", member.offlineSince},
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
        {"enabled", settings.enabled(account.id())},
        {"status", account.status()},
        {"pendingRouting", owed.contacts},
        {"pendingRegistry", owed.registry},
        {"tokens", tokens},
    };
}

enum class Verb { eGet, ePost, eDelete };

void addRoute(http::Server& server, const Verb verb, const std::string& path, http::Handler handler)
{
    switch (verb) {
    case Verb::eGet: server.get(path, std::move(handler)); return;
    case Verb::ePost: server.post(path, std::move(handler)); return;
    case Verb::eDelete: server.del(path, std::move(handler)); return;
    }
}

constexpr char kApiPrefix[] = "/v1";
constexpr std::size_t kApiFirstCapture = 0;
constexpr std::size_t kOperatorFirstCapture = 1;

using AccountAction
    = std::function<http::Response(Account&, const http::Request&, std::size_t firstCapture)>;

struct SharedRoute {
    Verb verb;
    std::string tail;
    AccountAction action;
};

std::vector<SharedRoute> sharedRoutes()
{
    return {
        {Verb::eGet, "/contacts",
            [](Account& account, const http::Request&, std::size_t) {
                return http::jsonResponse(peersJson(account.contacts()));
            }},
        {Verb::ePost, "/contacts",
            [](Account& account, const http::Request& call, std::size_t) {
                return addContact(account, bazarish::parseJson(call.body));
            }},
        {Verb::eGet, "/chats",
            [](Account& account, const http::Request&, std::size_t) {
                return http::jsonResponse(linesJson(account.chats()));
            }},
        {Verb::ePost, "/chats/{b32}/clear",
            [](Account& account, const http::Request& call, const std::size_t first) {
                account.clearChat(call.captures.at(first), clearsForEveryone(call));
                return http::jsonResponse({{"cleared", call.captures.at(first)}});
            }},
        {Verb::eGet, "/storage",
            [](Account& account, const http::Request&, std::size_t) {
                return http::jsonResponse(storageJson(account.storage()));
            }},
        {Verb::ePost, "/storage/compact",
            [](Account& account, const http::Request&, std::size_t) {
                return http::jsonResponse(storageJson(account.compact()));
            }},
        {Verb::eGet, "/messages",
            [](Account& account, const http::Request& call, std::size_t) {
                const std::string peer = call.query("peer");
                if (peer.empty()) {
                    return failure(400, "\"peer\" names whose conversation to read");
                }
                return http::jsonResponse(linesJson(account.conversation(
                    peer, longParam(call, "before"), intParam(call, "limit", kDefaultPage))));
            }},
        {Verb::ePost, "/messages",
            [](Account& account, const http::Request& call, std::size_t) {
                const nlohmann::json body = bazarish::parseJson(call.body);
                const std::string peer = body.value("peer", std::string());
                const std::string text = body.value("text", std::string());
                if (peer.empty() || text.empty()) {
                    return failure(400, "a message is a \"peer\" and a \"text\"");
                }
                const std::string replyTo = body.value("replyTo", std::string());
                if (body.contains("keyboard") && !replyTo.empty()) {
                    return failure(400, "a message with a \"keyboard\" takes no \"replyTo\"");
                }
                return http::jsonResponse({{"messageId",
                    body.contains("keyboard")
                        ? account.sendKeyboard(peer, text, body.at("keyboard").dump())
                        : account.sendText(peer, text, replyTo)}});
            }},
        {Verb::ePost, "/messages/{hex}",
            [](Account& account, const http::Request& call, const std::size_t first) {
                const nlohmann::json body = bazarish::parseJson(call.body);
                const std::string peer = body.value("peer", std::string());
                if (peer.empty()) {
                    return failure(400, "\"peer\" names whose conversation the message is in");
                }
                account.editMessage(
                    peer, call.captures.at(first), body.value("text", std::string()));
                return http::jsonResponse({{"edited", call.captures.at(first)}});
            }},
        {Verb::eDelete, "/messages/{hex}",
            [](Account& account, const http::Request& call, const std::size_t first) {
                const std::string peer = call.query("peer");
                if (peer.empty()) {
                    return failure(400, "\"peer\" names whose conversation the message is in");
                }
                account.deleteMessage(peer, call.captures.at(first));
                return http::jsonResponse({{"deleted", call.captures.at(first)}});
            }},
        {Verb::ePost, "/reactions",
            [](Account& account, const http::Request& call, std::size_t) {
                const nlohmann::json body = bazarish::parseJson(call.body);
                const std::string peer = body.value("peer", std::string());
                const std::string messageId = body.value("messageId", std::string());
                if (peer.empty() || messageId.empty()) {
                    return failure(
                        400, "a reaction is a \"peer\", a \"messageId\" and an \"emoji\"");
                }
                account.react(peer, messageId, body.value("emoji", std::string()));
                return http::jsonResponse({{"ok", true}});
            }},
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
    reapPolls(true);
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
    options.hosts = answeredHosts(runtime_.config());
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
            return tokenRefused(request);
        }
        if (!account->running() && request.path != kStatusPath) {
            return failure(503, "the account is not open: " + account->status());
        }
        if (account->kind() == Settings::AccountKind::eGroup && request.path != kStatusPath
            && request.path != "/v1/posts" && !request.path.starts_with(kStoragePath)) {
            return failure(409, "a group takes its own posts at /v1/posts and nothing else");
        }
        try {
            return handler(*account, request);
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    };

    const auto statusOf = [this](Account& account) {
        const std::string fingerprint = account.fingerprint();
        return http::jsonResponse({
            {"fingerprint",
                fingerprint.empty() ? runtime_.settings().fingerprintOf(account.id()) : fingerprint},
            {"name", account.displayName()},
            {"kind", accountKindName(account.kind())},
            {"enabled", runtime_.settings().enabled(account.id())},
            {"status", account.status()},
            {"destination", account.destination()},
            {"lastEventId", account.lastEventId()},
            {"link", linkJson(runtime_.routerReport())},
        });
    };

    server_->get(kStatusPath, [guarded, statusOf](const http::Request& request) {
        return guarded(request, [statusOf](Account& account, const http::Request&) {
            return statusOf(account);
        });
    });

    server_->post(kStatusPath, [this, guarded, statusOf](const http::Request& request) {
        return guarded(request, [this, statusOf](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            if (!body.contains("enabled") || !body.at("enabled").is_boolean()) {
                return failure(400, "\"enabled\" is true or false");
            }
            runtime_.setEnabled(account.id(), body.at("enabled").get<bool>());
            return statusOf(account);
        });
    });

    server_->post("/v1/posts", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            if (account.kind() != Settings::AccountKind::eGroup) {
                return failure(409, "only a group takes posts; a bot writes with /v1/messages");
            }
            const nlohmann::json body = bazarish::parseJson(call.body);
            return http::jsonResponse({{"id", account.postToGroup(body.value("text", std::string()))}});
        });
    });

    server_->get("/v1/invite", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request&) {
            return http::jsonResponse({{"invite", account.inviteUri()}});
        });
    });

    server_->del("/v1/contacts/{b32}", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            account.removeContact(call.captures.at(0));
            return http::jsonResponse({{"removed", call.captures.at(0)}});
        });
    });

    server_->post("/v1/pictures", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string peer = body.value("peer", std::string());
            if (peer.empty()) {
                return failure(400, "a picture is a \"peer\" and the bytes in \"data\"");
            }
            const PictureCheck picture = checkPicture(
                body.value("data", std::string()), body.value("name", std::string()));
            if (!picture.accepted) {
                return failure(picture.refusal, picture.problem);
            }
            return http::jsonResponse({{"messageId",
                account.sendPicture(peer, picture.bytes, picture.name, picture.mime,
                    body.value("replyTo", std::string()))}});
        });
    });

    server_->post("/v1/files", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string path = body.value("path", std::string());
            if (peer.empty() || path.empty()) {
                return failure(400, "a file is a \"peer\" and the \"path\" it is read from");
            }
            const FileCheck source = checkSource(path);
            if (!source.accepted) {
                return failure(source.refusal, source.problem);
            }
            return http::jsonResponse({{"messageId",
                account.sendFile(peer, path, body.value("replyTo", std::string()))}});
        });
    });

    server_->post("/v1/downloads", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string messageId = body.value("messageId", std::string());
            const std::string path = body.value("path", std::string());
            if (peer.empty() || messageId.empty() || path.empty()) {
                return failure(400,
                    "a download is a \"peer\", the \"messageId\" of their file and the"
                    " \"path\" it is written to");
            }
            FileCheck check = checkTarget(path);
            if (check.accepted) {
                check = account.download(peer, messageId, path);
            }
            if (!check.accepted) {
                return failure(check.refusal, check.problem);
            }
            return http::jsonResponse({{"messageId", messageId}, {"path", path}});
        });
    });

    server_->get("/v1/transfers", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request&) {
            nlohmann::json listed = nlohmann::json::array();
            for (const Transfer& transfer : account.transfers()) {
                listed.push_back(transferJson(transfer));
            }
            return http::jsonResponse({{"transfers", listed}});
        });
    });

    server_->get("/v1/transfers/{hex}", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const std::optional<Transfer> transfer
                = account.transfer(call.query("peer"), call.captures.at(0));
            if (!transfer.has_value()) {
                return failure(404, "no transfer of the message " + call.captures.at(0)
                        + " with \"peer\" " + call.query("peer") + " since the account opened");
            }
            return http::jsonResponse(transferJson(*transfer));
        });
    });

    server_->del("/v1/transfers/{hex}", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            if (!account.cancelTransfer(call.query("peer"), call.captures.at(0))) {
                return failure(404, "no transfer of the message " + call.captures.at(0)
                        + " with \"peer\" " + call.query("peer") + " is under way");
            }
            return http::jsonResponse({{"cancelled", call.captures.at(0)}});
        });
    });

    server_->post("/v1/menu", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string peer = body.value("peer", std::string());
            if (peer.empty() || !body.contains("items") || !body.at("items").is_array()) {
                return failure(400, "a menu is a \"peer\" and its \"items\"");
            }
            std::vector<client::BotMenuItem> items;
            for (const nlohmann::json& item : body.at("items")) {
                items.push_back(
                    {item.at("title").get<std::string>(), item.at("command").get<std::string>()});
            }
            return http::jsonResponse({{"sent", account.setMenu(peer, std::move(items))}});
        });
    });

    server_->post("/v1/forward", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string from = body.value("from", std::string());
            const std::string messageId = body.value("messageId", std::string());
            const std::string to = body.value("to", std::string());
            if (from.empty() || messageId.empty() || to.empty()) {
                return failure(400, "a forward is \"from\", \"messageId\" and \"to\"");
            }
            return http::jsonResponse({{"messageId", account.forward(from, messageId, to)}});
        });
    });

    server_->get("/v1/chats/{b32}/timer", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            return http::jsonResponse({{"seconds", account.chatTimer(call.captures.at(0))}});
        });
    });

    server_->post("/v1/chats/{b32}/timer", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            if (!body.contains("seconds") || !body.at("seconds").is_number_integer()) {
                return failure(400, "a timer is \"seconds\", a whole number");
            }
            const std::int64_t seconds = body.at("seconds").get<std::int64_t>();
            account.setChatTimer(call.captures.at(0), seconds);
            return http::jsonResponse({{"seconds", seconds}});
        });
    });

    for (const SharedRoute& shared : sharedRoutes()) {
        addRoute(*server_, shared.verb, kApiPrefix + shared.tail,
            [guarded, action = shared.action](const http::Request& request) {
                return guarded(request, [&action](Account& account, const http::Request& call) {
                    return action(account, call, kApiFirstCapture);
                });
            });
    }

    server_->post("/v1/receipts", [guarded](const http::Request& request) {
        return guarded(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string peer = body.value("peer", std::string());
            const std::string messageId = body.value("messageId", std::string());
            if (peer.empty() || messageId.empty()) {
                return failure(400, "a receipt is a \"peer\" and a \"messageId\"");
            }
            account.confirmReceipt(peer, messageId);
            return http::jsonResponse({{"ok", true}});
        });
    });

    server_->getAsync("/v1/events",
        [this](const http::Request& request, http::Responder respond)
            -> std::optional<http::Response> {
            const std::shared_ptr<Account> account = accountForToken(request);
            if (account == nullptr) {
                return tokenRefused(request);
            }
            if (!account->running()) {
                return failure(503, "the account is not open: " + account->status());
            }
            if (account->kind() == Settings::AccountKind::eGroup) {
                return failure(
                    409, "a group relays its members itself; the API does not write for it");
            }
            return answerEvents(account, request, std::move(respond));
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
            state["version"] = kBuildVersion;
        }
        return http::jsonResponse(state);
    });

    server_->post("/ui/login", [this](const http::Request& request) {
        try {
            const nlohmann::json body = bazarish::parseJson(request.body);
            operator_.signIn(body.value("challenge", std::string()),
                body.value("blob", std::string()), nowSeconds());
            http::Response response = http::jsonResponse({{"ok", true}});
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
        http::Response response = http::jsonResponse({{"ok", true}});
        response.headers["Set-Cookie"]
            = std::string(kSessionCookie) + "=; Path=/; HttpOnly; Max-Age=0";
        return response;
    });

    server_->get("/ui/settings", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            Settings& settings = runtime_.settings();
            return http::jsonResponse({
                {"transport", settings.transport() == Settings::Transport::eSam ? "sam"
                                                                                : "embedded"},
                {"samHost", settings.samHost()},
                {"samPort", settings.samPort()},
                {"privacy", std::string(privacyName(settings.privacy()))},
                {"transit", settings.transit()},
                {"operator", operator_.operatorFingerprint()},
            });
        });
    });

    server_->post("/ui/settings", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
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
            if (body.contains("transit")) {
                runtime_.settings().setTransit(body.at("transit").get<bool>());
            }
            return http::jsonResponse({{"ok", true}});
        });
    });

    server_->get("/ui/link", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            return http::jsonResponse(linkJson(runtime_.routerReport()));
        });
    });

    server_->get("/ui/system", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            const Runtime::RouterReport router = runtime_.routerReport();
            const MemoryUse memory = memoryUse();
            const DiskUse disk = diskUse(runtime_.config().stateDir);
            nlohmann::json routerJson = {{"running", router.running}, {"online", router.online},
                {"counted", router.counted},
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
            return http::jsonResponse({
                {"version", kBuildVersion},
                {"uptimeSeconds", runtime_.uptimeSeconds()},
                {"transport", runtime_.settings().transport() == Settings::Transport::eSam
                        ? "sam"
                        : "embedded"},
                {"router", routerJson},
                {"memory", {{"residentBytes", memory.residentBytes},
                               {"peakBytes", memory.peakBytes}}},
                {"disk", {{"stateBytes", disk.stateBytes}, {"freeBytes", disk.freeBytes}}},
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
            return http::jsonResponse({{"held", offset + call.body.size()}});
        });
    });

    server_->post("/ui/system/restart", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request&) {
            log::warn("the operator asked for a restart");
            restartAsked_();
            return http::jsonResponse({{"restarting", true}});
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
            return http::jsonResponse({{"restarting", true}});
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
                    {"enabled", runtime_.settings().enabled(summary.id)},
                    {"status", summary.status},
                });
            }
            return http::jsonResponse(array);
        });
    });

    server_->post("/ui/accounts", [this, operatorOnly](const http::Request& request) {
        return operatorOnly(request, [this](const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string action = body.value("action", std::string());
            if (action == "create") {
                return http::jsonResponse({{"id",
                    runtime_.createAccount(body.value("name", std::string()), kindParam(body))}});
            }
            if (action == "import") {
                return http::jsonResponse({{"id",
                    runtime_.importAccount(fromBase64(body.value("bundle", std::string())),
                        body.value("bundlePassphrase", std::string()), kindParam(body))}});
            }
            return failure(400, "the action is create or import");
        });
    });

    server_->get("/", [this](const http::Request& request) {
        http::Response response;
        response.contentType = "text/html; charset=utf-8";
        response.body = assets_.file(signedIn(request) ? "index.html" : "login.html").body;
        return response;
    });
    server_->get("/assets/{asset}", [this](const http::Request& request) {
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
            return http::jsonResponse(accountJson(account, runtime_.settings()));
        });
    });

    server_->get(base + "/avatar", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            const Account::Avatar avatar = account.avatar();
            if (avatar.data.empty()) {
                return failure(404, "the account has no avatar");
            }
            http::Response response;
            response.body.assign(avatar.data.begin(), avatar.data.end());
            response.contentType = avatar.mime;
            response.headers["Cache-Control"] = "no-store";
            return response;
        });
    });

    server_->post(base + "/avatar", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const PictureCheck picture
                = checkPicture(body.value("data", std::string()), kAvatarName);
            if (!picture.accepted) {
                return failure(picture.refusal, picture.problem);
            }
            account.setAvatar(picture.bytes, picture.mime);
            return http::jsonResponse({{"ok", true}});
        });
    });

    server_->del(base + "/avatar", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            account.setAvatar({}, {});
            return http::jsonResponse({{"ok", true}});
        });
    });

    server_->get(base + "/invite.svg", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            const std::string invite = account.running() ? account.inviteUri() : std::string();
            if (invite.empty()) {
                return failure(409, "the account has no invite while it is closed");
            }
            http::Response response;
            response.body = qr::svg(invite);
            response.contentType = "image/svg+xml";
            return response;
        });
    });

    server_->post(base, [this, forAccount](const http::Request& request) {
        return forAccount(request, [this](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string action = body.value("action", std::string());
            if (action == "open") {
                if (!runtime_.settings().enabled(account.id())) {
                    return failure(409, "the account is switched off");
                }
                account.open();
                return http::jsonResponse({{"status", account.status()}});
            }
            if (action == "enable" || action == "disable") {
                runtime_.setEnabled(account.id(), action == "enable");
                return http::jsonResponse({{"status", account.status()}});
            }
            if (action == "rename") {
                account.rename(body.value("name", std::string()));
                return http::jsonResponse({{"name", body.value("name", std::string())}});
            }
            if (action == "new-address") {
                account.newAddress();
                return http::jsonResponse({{"address", account.destination()}});
            }
            if (action == "retry-routing") {
                account.retryRouting(body.value("peer", std::string()));
                return http::jsonResponse({{"sending", true}});
            }
            if (action == "accept-contacts" && account.kind() == Settings::AccountKind::eBot) {
                const bool accepts = body.at("accepts").get<bool>();
                runtime_.settings().setAcceptsContacts(account.id(), accepts);
                return http::jsonResponse({{"acceptsContacts", accepts}});
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
            return http::jsonResponse({{"ok", true}});
        } catch (const std::exception& error) {
            return failure(400, error.what());
        }
    });

    server_->post(base + "/export", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const Bytes bundle = account.exportBundle(body.value("bundlePassphrase", std::string()));
            http::Response response;
            response.contentType = "application/octet-stream";
            response.headers["Content-Disposition"]
                = "attachment; filename=\"" + backupFileName(account.displayName()) + "\"";
            response.body.assign(bundle.begin(), bundle.end());
            return response;
        });
    });

    server_->post(base + "/tokens", [this, forAccount](const http::Request& request) {
        return forAccount(request, [this](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string name = body.value("name", std::string());
            if (name.empty()) {
                return failure(400, "a token is issued under a name");
            }
            const ApiToken token = runtime_.settings().issueToken(account.id(), name);
            return http::jsonResponse({{"name", token.name}, {"token", token.secret},
                {"createdAt", token.createdAt}});
        });
    });

    server_->del(base + "/tokens/{segment}", [this, forAccount](const http::Request& request) {
        return forAccount(request, [this](Account& account, const http::Request& call) {
            if (!runtime_.settings().revokeToken(account.id(), call.captures.at(1))) {
                return failure(404, "no token by that name");
            }
            return http::jsonResponse({{"ok", true}});
        });
    });

    server_->post(base + "/sign", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string challenge = body.value("challenge", std::string());
            nlohmann::json described = consumerJson(challenge);
            if (body.value("sign", false)) {
                if (!described.at("ok").get<bool>()) {
                    return failure(400, described.at("problem").get<std::string>());
                }
                described["signature"] = account.signLogin(challenge);
            }
            return http::jsonResponse(described);
        });
    });

    server_->get(base + "/aliases", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return http::jsonResponse(aliasesJson(account.aliases()));
        });
    });

    server_->post(base + "/aliases", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            if (body.value("check", false)) {
                account.checkAliases();
                return http::jsonResponse(aliasesJson(account.aliases()));
            }
            if (body.contains("alias") && body.contains("resolving")) {
                account.setAliasBinding(
                    body.at("alias").get<std::string>(), body.at("resolving").get<bool>());
                return http::jsonResponse(aliasesJson(account.aliases()));
            }
            return failure(400, "\"check\" asks the registry; \"alias\" with \"resolving\" "
                                "points one here or nowhere");
        });
    });

    server_->get(base + "/group", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            const std::shared_ptr<GroupEngine> group = account.group();
            nlohmann::json states = nlohmann::json::object();
            for (const Member& member : group->members()) {
                states[memberStateName(member.state)]
                    = states.value(memberStateName(member.state), 0) + 1;
            }
            return http::jsonResponse({{"config", groupConfigJson(group->config())}, {"states", states},
                {"requests", group->requests().size()}});
        });
    });

    server_->post(base + "/group", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::shared_ptr<GroupEngine> group = account.group();
            GroupConfig config = group->config();
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
            group->setConfig(config);
            return http::jsonResponse(groupConfigJson(group->config()));
        });
    });

    server_->get(base + "/group/members", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            return http::jsonResponse(membersJson(account.group()->members()));
        });
    });

    server_->post(base + "/group/members", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            const std::string fingerprint = body.value("fingerprint", std::string());
            const std::string action = body.value("action", std::string());
            const std::shared_ptr<GroupEngine> group = account.group();
            if (action == "admin" || action == "unadmin") {
                group->setAdmin(fingerprint, action == "admin");
            } else if (action == "exclude") {
                group->exclude(fingerprint);
            } else if (action == "block") {
                group->block(fingerprint);
            } else if (action == "unblock") {
                group->unblock(fingerprint);
            } else {
                return failure(400, "the action is admin, unadmin, exclude, block or unblock");
            }
            return http::jsonResponse(membersJson(group->members()));
        });
    });

    server_->get(base + "/group/requests", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request&) {
            const std::shared_ptr<GroupEngine> group = account.group();
            const std::vector<Member> members = group->members();
            nlohmann::json rows = nlohmann::json::array();
            for (const JoinRequest& asked : group->requests()) {
                rows.push_back({{"number", asked.number}, {"fingerprint", asked.fingerprint},
                    {"name", nameOf(members, asked.fingerprint)}, {"at", asked.at}});
            }
            return http::jsonResponse(rows);
        });
    });

    server_->post(base + "/group/requests", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            account.group()->decide(body.at("number").get<std::int64_t>(), body.at("allow").get<bool>());
            return http::jsonResponse({{"ok", true}});
        });
    });

    server_->get(base + "/group/posts", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const std::shared_ptr<GroupEngine> group = account.group();
            const std::vector<Member> members = group->members();
            nlohmann::json rows = nlohmann::json::array();
            for (const Post& post : group->posts(intParam(call, "limit", kDefaultPage))) {
                rows.push_back({{"id", post.id},
                    {"authorName", nameOf(members, post.author)}, {"type", post.type},
                    {"text", post.text}, {"sentAt", post.sentAt}, {"edited", post.edited},
                    {"deleted", post.deleted}, {"fromHost", post.author.empty()},
                    {"reactions", group->reactionCounts(post.id)}});
            }
            return http::jsonResponse(rows);
        });
    });

    server_->post(base + "/group/posts", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            const nlohmann::json body = bazarish::parseJson(call.body);
            return http::jsonResponse({{"id", account.postToGroup(body.value("text", std::string()))}});
        });
    });

    server_->del(base + "/group/posts/{segment}", [forAccount](const http::Request& request) {
        return forAccount(request, [](Account& account, const http::Request& call) {
            account.group()->deletePost(call.captures.at(1));
            return http::jsonResponse({{"ok", true}});
        });
    });

    for (const SharedRoute& shared : sharedRoutes()) {
        addRoute(*server_, shared.verb, base + shared.tail,
            [forAccount, action = shared.action](const http::Request& request) {
                return forAccount(request, [&action](Account& account, const http::Request& call) {
                    return action(account, call, kOperatorFirstCapture);
                });
            });
    }

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
            return answerEvents(account, request, std::move(respond));
        });
}

}  // namespace bazarish::bot
