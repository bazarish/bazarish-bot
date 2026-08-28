// Bazarish project (c) 2026
#include "Runtime.hpp"

#include <AccountKey.hpp>
#include <ApiClient.hpp>

#include <nlohmann/json.hpp>

#include <bazarish/Log.hpp>
#include <bazarish/ServerDescriptor.hpp>

#include <chrono>
#include <filesystem>
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

void Runtime::importAccount(const fs::path& bundle, const std::string& bundlePassphrase,
    const std::string& passphrase)
{
    if (hasAccount()) {
        throw std::runtime_error("this daemon already has an account");
    }
    allowOwnLoopbackFacade();
    client::Session::importAccount(bundle, accountFile(), bundlePassphrase, passphrase);
    bringUpAsync(passphrase);
}

void Runtime::exportAccount(const fs::path& bundle, const std::string& bundlePassphrase)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (session_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    session_->exportAccount(bundle, bundlePassphrase);
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

    if (settings_.transport() == Settings::Transport::eSam) {
        i2p::RouterConfig routerConfig;
        routerConfig.dataDir = config_.stateDir / "i2p";
        routerConfig.backend = i2p::Backend::eSam;
        routerConfig.samHost = settings_.samHost();
        routerConfig.samControlPort = settings_.samPort();
        log::info("transport: SAM at {}:{}", routerConfig.samHost, routerConfig.samControlPort);
        router_ = std::make_unique<i2p::Router>(routerConfig);
    } else {
        i2p::RouterConfig routerConfig;
        routerConfig.dataDir = config_.stateDir / "i2p";
        routerConfig.role = i2p::Role::eClient;
        log::info("transport: a router of our own under {}", routerConfig.dataDir.string());
        router_ = std::make_unique<i2p::Router>(routerConfig);
    }

    log::info("transport up; starting the mailbox");
    mailbox_ = std::make_unique<ServerSide>(config_.stateDir, settings_.privacy(), router_.get());
    mailbox_->start();

    log::info("opening the account");
    auto session = std::make_unique<client::Session>(client::Session::open(accountFile(), passphrase));

    // The account subscribes to the mailbox in this process, over loopback.
    if (!session->isConnected()) {
        client::ServerEndpoint endpoint;
        endpoint.serverFingerprint = mailbox_->fingerprint();
        endpoint.facades.push_back(client::Facade{false, mailbox_->host(), mailbox_->port(), {}});
        log::info("subscribing to the mailbox at {}:{}", mailbox_->host(), mailbox_->port());
        session->connectServer(endpoint);
        session->registerAccount();
        log::info("subscribed");
    }

    // The destination is this daemon's own, raised by the mailbox from a key it
    // keeps: there is nothing to delegate to a server that is this same process.
    const std::string destination = mailbox_->serveDestinationFor(session->fingerprint());
    log::info("destination {}", destination.empty() ? "building" : destination);
    session->refreshOwnCard();
    log::info("card refreshed; the account is up");

    auto transcript = std::make_unique<Transcript>(
        config_.stateDir / kTranscriptFile, passphrase);

    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        session_ = std::move(session);
        transcript_ = std::move(transcript);
        status_ = "running";
    }
    stopping_ = false;
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

        bool wrote = false;
        for (const client::IncomingMessage& item : arrived) {
            // Control traffic is the core's business, not a conversation's: what
            // is written down is what somebody said.
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
            line.text = item.text;
            line.keyboard = item.keyboardJson;
            line.replyTo = item.replyTo;
            line.sentAt = item.sentAt;
            try {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (transcript_ != nullptr && transcript_->append(line) != 0) {
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
    router_.reset();
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
            session.contactIsPending(fingerprint)});
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

std::string Runtime::sendText(const std::string& peer, const std::string& text)
{
    Line line;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        if (!session.sendMessage(peer, text)) {
            throw std::runtime_error("the message was not accepted for delivery");
        }
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
    return line.e2eId;
}

std::string Runtime::sendKeyboard(
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
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        session.sendInteractive(peer, text, keyboard);
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
    return line.e2eId;
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

std::int64_t Runtime::lastLineId() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return transcript_ == nullptr ? 0 : transcript_->lastId();
}

std::vector<Line> Runtime::await(const std::int64_t sinceId, const int seconds) const
{
    const auto ready = [this, sinceId]() {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        return transcript_ != nullptr && transcript_->lastId() > sinceId;
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
