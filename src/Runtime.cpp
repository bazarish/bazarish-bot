// Bazarish project (c) 2026
#include "Runtime.hpp"

#include <AccountKey.hpp>
#include <ApiClient.hpp>
#include <I2pRouter.hpp>

#include <bazarish/Bytes.hpp>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <bazarish/Log.hpp>
#include <bazarish/ServerDescriptor.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace bazarish::daemon {

namespace fs = std::filesystem;

namespace {

// One account, one file, one name: there is no account picker here, so the name
// is not the operator's to choose.
constexpr const char* kAccountFile = "account.db";
constexpr const char* kTranscriptFile = "transcript.db";
// How often the mailbox is pulled when nothing has woken the poller. The client
// core holds a long poll open on the mailbox, so this is the floor, not the rate.
constexpr int kPollSeconds = 5;
// What a long poll waits at most before answering with nothing: long enough that
// an idle bot makes one request a minute, short enough that a proxy in front of
// the daemon does not time the request out itself.
constexpr int kMaxWaitSeconds = 60;
// A protocol message id: eight random bytes, as hex, which is the shape every
// other client on this protocol writes.
constexpr std::size_t kMessageIdBytes = 8;

std::string freshMessageId()
{
    Bytes id(kMessageIdBytes);
    if (RAND_bytes(id.data(), static_cast<int>(id.size())) != 1) {
        throw std::runtime_error("no randomness for a message id");
    }
    return toHex(id);
}

// The mailbox this daemon serves is reached by the account in this same process.
// It is not a client of the network and never leaves the machine, which is what
// this switch says out loud.
void allowOwnLoopbackFacade()
{
    bazarish::setSelfHostedFacadeOnLoopback(true);
}

}  // namespace

Runtime::Runtime(Config config)
    : config_(std::move(config))
    , settings_(config_.stateDir / "settings.json")
    , status_("no account")
{
    fs::create_directories(config_.stateDir);
    if (hasAccount()) {
        status_ = "closed";
    }
}

Runtime::~Runtime()
{
    stop();
}

fs::path Runtime::accountFile() const
{
    return config_.stateDir / kAccountFile;
}

bool Runtime::hasAccount() const
{
    return fs::exists(accountFile());
}

bool Runtime::running() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return session_ != nullptr;
}

std::string Runtime::status() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return status_;
}

void Runtime::createAccount(const std::string& name, const std::string& passphrase)
{
    if (hasAccount()) {
        throw std::runtime_error("this daemon already has an account");
    }
    allowOwnLoopbackFacade();
    {
        // Created and closed here; bringUp opens it as the running session.
        const client::Session created
            = client::Session::create(accountFile(), passphrase, name);
        log::info("account {} created", log::redact(created.fingerprint()));
    }
    bringUpAsync(passphrase);
}

void Runtime::importAccount(const Bytes& bundle, const std::string& bundlePassphrase,
    const std::string& passphrase)
{
    if (hasAccount()) {
        throw std::runtime_error("this daemon already has an account");
    }
    allowOwnLoopbackFacade();
    // The core takes a bundle as a file, and the operator hands one over through a
    // browser: it is written down beside the account, used, and removed.
    const fs::path scratch = config_.stateDir / "import.bundle";
    {
        std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bundle.data()),
            static_cast<std::streamsize>(bundle.size()));
        out.close();
        if (!out) {
            throw std::runtime_error("cannot write the bundle down to read it");
        }
    }
    try {
        client::Session::importAccount(scratch, accountFile(), bundlePassphrase, passphrase);
    } catch (...) {
        std::error_code ignored;
        fs::remove(scratch, ignored);
        throw;
    }
    std::error_code ignored;
    fs::remove(scratch, ignored);
    bringUpAsync(passphrase);
}

Bytes Runtime::exportAccount(const std::string& bundlePassphrase)
{
    const fs::path scratch = config_.stateDir / "export.bundle";
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().exportAccount(scratch, bundlePassphrase);
    }
    std::ifstream in(scratch, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
    in.close();
    std::error_code ignored;
    fs::remove(scratch, ignored);
    if (text.empty()) {
        throw std::runtime_error("the export produced nothing");
    }
    return Bytes(text.begin(), text.end());
}

void Runtime::rename(const std::string& name)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().setDisplayName(name);
}

void Runtime::rotateServingKey()
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().rotateServingKey(
        [](const std::string& stage) { log::info("serving key: {}", stage); });
}

void Runtime::newAddress()
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    const std::string address = mailbox_->replaceDestinationFor(session.fingerprint());
    cardCarriesDestination_ = false;
    log::info("address replaced: {}", address.empty() ? "building" : address);
}

void Runtime::start(const std::string& passphrase)
{
    if (!hasAccount()) {
        throw std::runtime_error("there is no account to open");
    }
    if (running()) {
        return;
    }
    // A wrong passphrase is the caller's mistake and must come back as one, not
    // as a status they have to go and read.
    if (!client::accountkey::unlocks(accountFile(), passphrase)) {
        throw std::runtime_error("that passphrase does not open the account");
    }
    allowOwnLoopbackFacade();
    bringUpAsync(passphrase);
}

void Runtime::bringUpAsync(const std::string& passphrase)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        if (status_ == "starting") {
            return;
        }
        status_ = "starting";
    }
    if (starter_.joinable()) {
        starter_.join();
    }
    starter_ = std::thread([this, passphrase]() {
        try {
            bringUp(passphrase);
        } catch (const std::exception& error) {
            log::error("cannot bring the account up: {}", error.what());
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            status_ = std::string("failed: ") + error.what();
        }
    });
}

void Runtime::bringUp(const std::string& passphrase)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        status_ = "starting";
    }

    // The client core reaches the transport through one router per process, and
    // the server half of this daemon is handed the same one: two routers here
    // would be two netDbs, two sets of tunnels and two addresses for one account.
    client::setI2pEnabled(true);
    if (settings_.transport() == Settings::Transport::eSam) {
        client::setSamTransport(settings_.samHost(), settings_.samPort());
        log::info("transport: SAM at {}:{}", settings_.samHost(), settings_.samPort());
    } else {
        // Nobody to ask for a netDb: this daemon is the server it would have
        // asked, so it bootstraps the way a server does.
        client::setPublicReseedAllowed(true);
        log::info("transport: a router of our own under {}",
            (config_.stateDir / "i2p").string());
    }
    i2p::Router& router = client::sharedI2pRouter(config_.stateDir / "i2p");

    log::info("transport up; starting the server");
    mailbox_ = std::make_unique<ServerSide>(config_.stateDir, settings_.privacy(), &router);
    mailbox_->start();

    log::info("opening the account");
    auto session = std::make_unique<client::Session>(client::Session::open(accountFile(), passphrase));

    // The destination is this daemon's own, raised by the server from a key it
    // keeps: there is nothing to delegate to a server that is this same process.
    const std::string destination = mailbox_->serveDestinationFor(session->fingerprint());
    log::info("destination {}", destination.empty() ? "still building" : destination);

    // The account subscribes to the server in this process, over loopback. The
    // ports are handed out at every start, so the endpoint is set every time
    // rather than only when there is none.
    client::ServerEndpoint endpoint;
    endpoint.serverFingerprint = mailbox_->fingerprint();
    endpoint.facades.push_back(client::Facade{false, mailbox_->host(), mailbox_->port(), {}});
    log::info("subscribing to the server at {}:{}", mailbox_->host(), mailbox_->port());
    session->connectServer(endpoint);
    session->registerSelfHosted();
    log::info("registered; the account is up");

    auto transcript = std::make_unique<Transcript>(
        config_.stateDir / kTranscriptFile, passphrase);

    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        session_ = std::move(session);
        transcript_ = std::move(transcript);
        status_ = "running";
    }
    stopping_ = false;
    cardCarriesDestination_ = !mailbox_->destination().empty() && session_ != nullptr
        && false;  // proven only by a successful re-issue below
    poller_ = std::thread([this]() { poll(); });
}

client::Session& Runtime::openSession() const
{
    if (session_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return *session_;
}

void Runtime::poll()
{
    while (!stopping_) {
        std::vector<client::IncomingMessage> arrived;
        try {
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            if (session_ == nullptr) {
                return;
            }
            arrived = session_->sync();
        } catch (const std::exception& error) {
            log::warn("mailbox: {}", error.what());
        }

        // The card is published at registration, when the destination may still
        // be building - and a card with no address in it is one a contact cannot
        // reach. It is re-issued once, as soon as there is an address to put in.
        if (!cardCarriesDestination_ && !mailbox_->destination().empty()) {
            try {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (session_ != nullptr) {
                    session_->refreshOwnCard();
                    cardCarriesDestination_ = true;
                    log::info("card re-issued with the destination {}", mailbox_->destination());
                }
            } catch (const std::exception& error) {
                log::warn("card not re-issued yet: {}", error.what());
            }
        }

        bool wrote = false;
        for (const client::IncomingMessage& item : arrived) {
            try {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (transcript_ == nullptr) {
                    continue;
                }
                // An edit, a delete, a reaction and a receipt are about a message
                // that is already here; only the rest are lines of their own.
                if (item.contentType == "edit") {
                    wrote = transcript_->applyEdit(item.fromFingerprint, item.refId, item.text,
                                item.keyboardJson, item.sentByUs)
                        || wrote;
                    continue;
                }
                if (item.contentType == "delete") {
                    wrote = transcript_->applyDelete(
                                item.fromFingerprint, item.refId, item.sentByUs)
                        || wrote;
                    continue;
                }
                if (item.contentType == "reaction") {
                    transcript_->applyReaction(item.fromFingerprint, item.refId,
                        item.sentByUs ? std::string("me") : item.fromFingerprint, item.text);
                    wrote = true;
                    continue;
                }
                if (item.contentType == "receipt") {
                    transcript_->applyReceipt(item.fromFingerprint, item.refId);
                    wrote = true;
                    continue;
                }
                // Control traffic is the core's business, not a conversation's:
                // what is written down is what somebody said.
                if (item.contentType != "text" && item.contentType != "file"
                    && item.contentType != "picture" && item.contentType != "voice"
                    && item.contentType != "bot.command" && item.contentType != "bot.callback"
                    && item.contentType != "contact.request") {
                    continue;
                }
                Line line;
                line.peer = item.fromFingerprint;
                line.outgoing = item.sentByUs;
                line.type = item.contentType;
                line.e2eId = item.e2eId;
                line.text = item.contentType == "bot.callback" ? item.callbackData : item.text;
                line.keyboard = item.keyboardJson;
                line.replyTo = item.contentType == "bot.callback" ? item.refId : item.replyTo;
                line.sentAt = item.sentAt;
                if (transcript_->append(line) != 0) {
                    wrote = true;
                }
            } catch (const std::exception& error) {
                log::warn("transcript: {}", error.what());
            }
        }

        if (wrote) {
            waiters_.notify_all();
        }

        std::unique_lock<std::mutex> lock(waitMutex_);
        waiters_.wait_for(lock, std::chrono::seconds(kPollSeconds),
            [this]() { return stopping_.load(); });
    }
}

void Runtime::stop()
{
    stopping_ = true;
    if (starter_.joinable()) {
        starter_.join();
    }
    waiters_.notify_all();
    if (poller_.joinable()) {
        poller_.join();
    }
    std::unique_ptr<client::Session> session;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        session = std::move(session_);
        transcript_.reset();
        status_ = hasAccount() ? "closed" : "no account";
    }
    session.reset();
    if (mailbox_) {
        mailbox_->stop();
        mailbox_.reset();
    }
}

std::string Runtime::destination() const
{
    return mailbox_ ? mailbox_->destination() : std::string();
}


std::string Runtime::fingerprint() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return session_ == nullptr ? std::string() : session_->fingerprint();
}

std::string Runtime::displayName() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return session_ == nullptr ? std::string() : session_->displayName();
}

std::string Runtime::inviteUri() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().inviteUri();
}

std::vector<Runtime::Peer> Runtime::contacts() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    std::vector<Peer> peers;
    for (const std::string& fingerprint : session.contactFingerprints()) {
        peers.push_back({fingerprint, session.contactDisplayName(fingerprint),
            session.contactIsPending(fingerprint), session.sendCapacity(fingerprint)});
    }
    return peers;
}

std::vector<Line> Runtime::conversation(
    const std::string& peer, const std::int64_t before, const int limit) const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return transcript_->conversation(peer, before, limit);
}

std::vector<Line> Runtime::chats() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return transcript_->latestPerPeer();
}

Runtime::Sent Runtime::sendText(const std::string& peer, const std::string& text)
{
    Line line;
    line.e2eId = freshMessageId();
    bool confirmed = false;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        // False is not a refusal: the mailbox took it and federates it in the
        // background with retries. It says only that the far side had not
        // confirmed storage before the wait was up.
        // The courier answers later and on its own thread; what it says is
        // written onto the line, because a bot sending in bulk has no other way
        // to learn that its messages stopped arriving.
        client::DeliveryWatch watch;
        const std::string messageId = line.e2eId;
        watch.onOutcome = [this, peer, messageId](const client::OutboundCourier::Outcome& outcome) {
            const std::string status = outcome.stored
                ? std::string("delivered")
                : "failed: "
                    + (outcome.errorMessage.empty() ? outcome.errorCode : outcome.errorMessage);
            try {
                const std::lock_guard<std::mutex> held(sessionMutex_);
                if (transcript_ != nullptr) {
                    transcript_->setStatus(peer, messageId, status);
                }
            } catch (const std::exception& error) {
                log::warn("transcript: {}", error.what());
            }
            waiters_.notify_all();
        };
        confirmed = session.sendMessage(peer, text, line.e2eId, watch);
        line.status = "sending";
        line.peer = peer;
        line.outgoing = true;
        line.type = "text";
        line.text = text;
        line.sentAt = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        transcript_->append(line);
    }
    waiters_.notify_all();
    return {line.e2eId, confirmed};
}

Runtime::Sent Runtime::sendKeyboard(
    const std::string& peer, const std::string& text, const std::string& keyboardJson)
{
    client::InlineKeyboard keyboard;
    for (const nlohmann::json& row : nlohmann::json::parse(keyboardJson)) {
        std::vector<client::InlineButton> buttons;
        for (const nlohmann::json& button : row) {
            buttons.push_back({button.value("text", std::string()),
                button.value("data", std::string()), button.value("command", std::string())});
        }
        keyboard.push_back(std::move(buttons));
    }
    Line line;
    line.e2eId = freshMessageId();
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        session.sendInteractive(peer, text, keyboard, line.e2eId);
        line.status = "sending";
        line.peer = peer;
        line.outgoing = true;
        line.type = "text";
        line.text = text;
        line.keyboard = client::inlineKeyboardJson(keyboard);
        line.sentAt = static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        transcript_->append(line);
    }
    waiters_.notify_all();
    // An interactive message is handed to the courier the same way; there is no
    // confirmation to report because the call does not wait for one.
    return {line.e2eId, false};
}

void Runtime::acceptContact(const std::string& peer)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().acceptContactRequest(peer);
}

std::string Runtime::addByInvite(const std::string& invite, const std::string& text)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().addByInvite(invite, text);
}

void Runtime::editMessage(
    const std::string& peer, const std::string& messageId, const std::string& text)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        if (!transcript_->applyEdit(peer, messageId, text, {}, true)) {
            throw std::runtime_error("no message of ours by that id in this conversation");
        }
        (void)session.sendEdit(peer, messageId, text);
    }
    waiters_.notify_all();
}

void Runtime::deleteMessage(const std::string& peer, const std::string& messageId)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        if (!transcript_->applyDelete(peer, messageId, true)) {
            throw std::runtime_error("no message of ours by that id in this conversation");
        }
        session.sendDelete(peer, messageId);
    }
    waiters_.notify_all();
}

void Runtime::react(
    const std::string& peer, const std::string& messageId, const std::string& emoji)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        session.sendReaction(peer, messageId, emoji);
        transcript_->applyReaction(peer, messageId, "me", emoji);
    }
    waiters_.notify_all();
}

void Runtime::confirmReceipt(const std::string& peer, const std::string& messageId)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().sendReceipt(peer, messageId);
}

std::int64_t Runtime::lastEventId() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return transcript_ == nullptr ? 0 : transcript_->lastEventId();
}

std::vector<Event> Runtime::await(const std::int64_t sinceId, const int seconds) const
{
    const auto ready = [this, sinceId]() {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        return transcript_ != nullptr && transcript_->lastEventId() > sinceId;
    };
    if (!ready()) {
        const int wait = seconds > kMaxWaitSeconds ? kMaxWaitSeconds : seconds;
        std::unique_lock<std::mutex> lock(waitMutex_);
        waiters_.wait_for(lock, std::chrono::seconds(wait < 0 ? 0 : wait),
            [&ready, this]() { return stopping_.load() || ready(); });
    }
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        return {};
    }
    return transcript_->since(sinceId, 0);
}

}  // namespace bazarish::daemon
