// Bazarish project (c) 2026
#include "Account.hpp"

#include <ApiClient.hpp>

#include <bazarish/Bytes.hpp>
#include <bazarish/Clock.hpp>
#include <bazarish/Log.hpp>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace bazarish::bot {

namespace fs = std::filesystem;

namespace {

constexpr int kMaintenanceSeconds = 5;
constexpr std::int64_t kAliasServiceSeconds = 60;
constexpr std::int64_t kRegistryRetrySeconds = 10;
constexpr int kEventWaitSeconds = 30;
constexpr int kMaxWaitSeconds = 60;
constexpr std::size_t kMessageIdBytes = 8;
constexpr const char* kClosed = "closed";
constexpr const char* kStarting = "starting";
constexpr const char* kRunning = "running";

std::string freshMessageId()
{
    Bytes id(kMessageIdBytes);
    if (RAND_bytes(id.data(), static_cast<int>(id.size())) != 1) {
        throw std::runtime_error("no randomness for a message id");
    }
    return toHex(id);
}

std::string stageName(const client::Session::PairingStage stage)
{
    switch (stage) {
    case client::Session::PairingStage::ePublishing:
        return "publishing";
    case client::Session::PairingStage::eWaiting:
        return "waiting";
    case client::Session::PairingStage::eWrongCode:
        return "wrong-code";
    case client::Session::PairingStage::eSending:
        return "sending";
    case client::Session::PairingStage::eDone:
        return "done";
    case client::Session::PairingStage::eRefused:
        return "refused";
    case client::Session::PairingStage::eFailed:
        return "failed";
    }
    return "failed";
}

bool pairingSettled(const client::Session::PairingStage stage)
{
    return stage == client::Session::PairingStage::eDone
        || stage == client::Session::PairingStage::eRefused
        || stage == client::Session::PairingStage::eFailed;
}

}  // namespace

Account::Account(std::string id, fs::path accountFile, fs::path transcriptFile,
    Settings& settings, ServerSideFn serverSide)
    : id_(std::move(id))
    , accountFile_(std::move(accountFile))
    , transcriptFile_(std::move(transcriptFile))
    , settings_(settings)
    , serverSide_(std::move(serverSide))
    , status_(kClosed)
{
}

Account::~Account()
{
    close();
}

bool Account::running() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return status_ == kRunning;
}

std::string Account::status() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return status_;
}

void Account::open()
{
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (status_ == kRunning || status_ == kStarting) {
            return;
        }
        status_ = kStarting;
    }
    if (starter_.joinable()) {
        starter_.join();
    }
    starter_ = std::thread([this]() {
        try {
            bringUp();
        } catch (const std::exception& error) {
            log::error("account {}: cannot bring it up: {}", id_, error.what());
            const std::lock_guard<std::mutex> lock(stateMutex_);
            status_ = std::string("failed: ") + error.what();
        }
    });
}

void Account::bringUp()
{
    ServerSide& mailbox = serverSide_();

    log::info("account {}: opening", id_);
    auto session
        = std::make_unique<client::Session>(client::Session::open(accountFile_));
    applyKind(*session);
    session->setDeviceSync(false);
    const std::string fingerprint = session->fingerprint();
    settings_.rememberAccount(id_, fingerprint, settings_.kind(id_));

    const std::string destination = mailbox.serveDestinationFor(fingerprint);
    log::info("account {}: destination {}", id_,
        destination.empty() ? "still building" : destination);

    client::ServerEndpoint endpoint;
    endpoint.serverFingerprint = mailbox.fingerprint();
    endpoint.facades.push_back(client::Facade{false, mailbox.host(), mailbox.port(), {}});
    session->connectServer(endpoint);
    session->registerSelfHosted();
    log::info("account {}: registered and up", id_);
    const client::Session::ContactFetchContext waiterContext = session->contactFetchContext();
    const std::shared_ptr<client::LoginSigner> signer = session->loginSigner();
    noteAliases(*session);

    auto transcript = std::make_unique<Transcript>(transcriptFile_, std::string());

    stopping_ = false;
    cardCarriesDestination_ = false;
    lastAliasErrand_ = 0;
    aliasesAnswered_ = false;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        fingerprint_ = fingerprint;
        name_ = session->displayName();
        signer_ = signer;
    }
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        mailbox_ = &mailbox;
        session_ = std::move(session);
        transcript_ = std::move(transcript);
    }
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        status_ = kRunning;
    }
    maintainer_ = std::thread([this]() { maintain(); });
    eventWaiter_ = std::thread([this, waiterContext]() { waitForEvents(waiterContext); });
}

void Account::close()
{
    stopping_ = true;
    if (starter_.joinable()) {
        starter_.join();
    }
    waiters_.notify_all();
    if (maintainer_.joinable()) {
        maintainer_.join();
    }
    if (eventWaiter_.joinable()) {
        eventWaiter_.join();
    }
    std::thread errand;
    {
        const std::lock_guard<std::mutex> lock(errandMutex_);
        errand = std::move(aliasErrand_);
    }
    if (errand.joinable()) {
        errand.join();
    }
    std::unique_ptr<client::Session> session;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (status_ == kRunning || status_ == kStarting) {
            status_ = kClosed;
        }
        signer_.reset();
        aliases_ = {};
    }
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        session = std::move(session_);
        transcript_.reset();
    }
    session.reset();
    {
        const std::lock_guard<std::mutex> lock(pairing_->mutex);
        pairing_->state = {};
    }
}

void Account::closeOnServer()
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().closeAccountOnServer();
}

Settings::AccountKind Account::kind() const
{
    return settings_.kind(id_);
}

void Account::applyKind(client::Session& session) const
{
    session.setOutboundAddressing(settings_.kind(id_) == Settings::AccountKind::eGroup
            ? client::OutboundAddressing::eOneForAll
            : client::OutboundAddressing::eOnePerCorrespondent);
}

void Account::setKind(const Settings::AccountKind kind)
{
    settings_.setKind(id_, kind);
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (session_ != nullptr) {
        applyKind(*session_);
    }
}

Bytes Account::exportBundle(const std::string& bundlePassphrase)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().exportAccountBytes(bundlePassphrase);
}

Account::Pairing Account::startPairing()
{
    const std::shared_ptr<PairingSlot> slot = pairing_;
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    const client::Session::PairingOffer offer
        = openSession().startPairing([slot](const client::Session::PairingEvent& event) {
              const std::lock_guard<std::mutex> held(slot->mutex);
              slot->state.stage = stageName(event.stage);
              slot->state.done = event.done;
              slot->state.total = event.total;
              slot->state.wrongCodes = event.wrongCodes;
              slot->state.error = event.error;
              if (pairingSettled(event.stage)) {
                  slot->state.active = false;
                  slot->state.code.clear();
              }
          });
    const std::lock_guard<std::mutex> held(slot->mutex);
    slot->state = {};
    slot->state.active = true;
    slot->state.uri = offer.uri;
    slot->state.code = offer.code;
    slot->state.stage = stageName(client::Session::PairingStage::ePublishing);
    return slot->state;
}

void Account::stopPairing()
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_ != nullptr) {
            session_->stopPairing();
        }
    }
    const std::lock_guard<std::mutex> lock(pairing_->mutex);
    pairing_->state = {};
}

Account::Pairing Account::pairing() const
{
    const std::lock_guard<std::mutex> lock(pairing_->mutex);
    return pairing_->state;
}

void Account::rename(const std::string& name)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().setDisplayName(name);
    }
    const std::lock_guard<std::mutex> lock(stateMutex_);
    name_ = name;
}

void Account::newAddress()
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession();
    const std::string address = mailbox_->replaceDestinationFor(fingerprint());
    cardCarriesDestination_ = false;
    announceRoutingWhenCardIsOut_ = true;
    log::info("account {}: address replaced: {}", id_, address.empty() ? "building" : address);
}

void Account::maintain()
{
    const std::string fingerprint = this->fingerprint();
    while (!stopping_) {
        if (mailbox_->destinationNeedsRenewal(fingerprint, bazarish::nowSeconds())) {
            try {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (session_ != nullptr) {
                    mailbox_->serveDestinationFor(fingerprint);
                    log::info("account {}: delegation re-issued for {}", id_,
                        mailbox_->destination(fingerprint));
                }
            } catch (const std::exception& error) {
                log::warn("account {}: delegation not re-issued: {}", id_, error.what());
            }
        }

        try {
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            if (session_ != nullptr) {
                session_->flushPendingEchoes();
                session_->serviceRoutingFanout();
            }
        } catch (const std::exception& error) {
            log::warn("account {}: the fan-out after the address change will try again: {}", id_,
                error.what());
        }

        try {
            std::optional<client::Session::AliasErrandContext> errand;
            {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (session_ != nullptr) {
                    const std::int64_t every = session_->routingFanout().registry
                        ? kRegistryRetrySeconds
                        : kAliasServiceSeconds;
                    const std::int64_t now = bazarish::nowSeconds();
                    const bool due = !aliasesAnswered_ || session_->aliasServicingDue();
                    if (due && (lastAliasErrand_ == 0 || now - lastAliasErrand_ >= every)) {
                        lastAliasErrand_ = now;
                        errand = session_->aliasErrandContext();
                    }
                }
            }
            if (errand) {
                startAliasErrand(std::move(*errand), false);
            }
        } catch (const std::exception& error) {
            log::warn("account {}: alias servicing not started: {}", id_, error.what());
        }

        if (!cardCarriesDestination_ && !mailbox_->destination(fingerprint).empty()) {
            try {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (session_ != nullptr) {
                    session_->refreshOwnCard();
                    cardCarriesDestination_ = true;
                    log::info("account {}: card re-issued with the destination {}", id_,
                        mailbox_->destination(fingerprint));
                    if (announceRoutingWhenCardIsOut_.exchange(false)) {
                        session_->announceNewRouting();
                    } else {
                        session_->runRoutingFanout();
                    }
                }
            } catch (const std::exception& error) {
                log::warn("account {}: card not re-issued yet: {}", id_, error.what());
            }
        }

        std::unique_lock<std::mutex> lock(waitMutex_);
        waiters_.wait_for(lock, std::chrono::seconds(kMaintenanceSeconds),
            [this]() { return stopping_.load(); });
    }
}

Account::PendingRouting Account::pendingRouting() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (session_ == nullptr) {
        return {};
    }
    const client::Session::RoutingFanout queued = session_->routingFanout();
    return {queued.contacts, queued.registry};
}

void Account::retryRouting(const std::string& peer)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().retryRoutingTo(peer);
}

client::Session& Account::openSession() const
{
    if (session_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return *session_;
}

void Account::waitForEvents(const client::Session::ContactFetchContext& context)
{
    drainMailbox();
    std::unique_ptr<client::Client> waiter;
    while (!stopping_) {
        std::vector<client::PendingEntry> waiting;
        try {
            if (waiter == nullptr) {
                waiter = client::Session::makeEventClient(context);
            }
            waiting = client::Session::waitForMail(*waiter, kEventWaitSeconds);
        } catch (const std::exception& error) {
            log::info("mail wait failed, asking again: {}", error.what());
            continue;
        }
        if (stopping_) {
            return;
        }
        if (!waiting.empty()) {
            drainMailbox();
        }
    }
}

void Account::drainMailbox()
{
    std::vector<client::IncomingMessage> arrived;
    try {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_ == nullptr) {
            return;
        }
        arrived = session_->sync();
    } catch (const std::exception& error) {
        log::warn("account {}: mailbox: {}", id_, error.what());
    }

    bool wrote = false;
    for (const client::IncomingMessage& item : arrived) {
        try {
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            if (transcript_ == nullptr) {
                continue;
            }
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
            if (item.contentType == "chat.clear") {
                transcript_->clearPeer(item.fromFingerprint);
                wrote = true;
                continue;
            }
            if (item.contentType != "text" && item.contentType != "file"
                && item.contentType != "image" && item.contentType != "audio"
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
            log::warn("account {}: transcript: {}", id_, error.what());
        }
    }

    if (wrote) {
        waiters_.notify_all();
    }
}

std::string Account::destination() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return status_ == kRunning ? mailbox_->destination(fingerprint_) : std::string();
}

std::string Account::fingerprint() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return fingerprint_;
}

std::string Account::displayName() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return name_;
}

std::string Account::inviteUri() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().inviteUri();
}

std::vector<Account::Peer> Account::contacts() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    std::vector<Peer> peers;
    for (const std::string& fingerprint : session.contactFingerprints()) {
        peers.push_back({fingerprint, session.contactDisplayName(fingerprint),
            session.contactIsPending(fingerprint), session.canWriteTo(fingerprint)});
    }
    return peers;
}

std::vector<Line> Account::conversation(
    const std::string& peer, const std::int64_t before, const int limit) const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return transcript_->conversation(peer, before, limit);
}

std::vector<Line> Account::chats() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return transcript_->latestPerPeer();
}

Account::Sent Account::sendText(const std::string& peer, const std::string& text)
{
    Line line;
    line.e2eId = freshMessageId();
    bool confirmed = false;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
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
                log::warn("account {}: transcript: {}", id_, error.what());
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

Account::Sent Account::sendKeyboard(
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
    return {line.e2eId, false};
}

void Account::clearChat(const std::string& peer)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        session.sendChatClear(peer);
        transcript_->clearPeer(peer);
    }
    waiters_.notify_all();
}

void Account::acceptContact(const std::string& peer)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().acceptContactRequest(peer);
}

std::string Account::addByInvite(const std::string& invite, const std::string& text)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().addByInvite(invite, text);
}

std::string Account::addByAlias(const std::string& alias, const std::string& text)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().addByAlias(alias, text);
}

std::string Account::signLogin(const std::string& challenge) const
{
    std::shared_ptr<client::LoginSigner> signer;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        signer = signer_;
    }
    if (signer == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return signer->sign(challenge);
}

Account::Aliases Account::aliases() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return aliases_;
}

void Account::noteAliases(const client::Session& session)
{
    std::vector<Alias> held;
    for (const client::Session::AliasHolding& holding : session.aliasNames()) {
        if (!holding.inApp) {
            continue;
        }
        held.push_back({holding.alias, holding.notAfter, holding.autoRenew,
            holding.bindingWanted, holding.bindingWanted && holding.bound});
    }
    const bool depositCovers = session.aliasDepositCovers();
    const std::lock_guard<std::mutex> lock(stateMutex_);
    aliases_.held = std::move(held);
    aliases_.depositCovers = depositCovers;
}

void Account::checkAliases()
{
    client::Session::AliasErrandContext context;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        context = openSession().aliasErrandContext();
    }
    startAliasErrand(std::move(context), true);
}

void Account::setAliasBinding(const std::string& alias, const bool on)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    session.setAliasBinding(alias, on);
    noteAliases(session);
}

void Account::startAliasErrand(client::Session::AliasErrandContext context, const bool byHand)
{
    const std::lock_guard<std::mutex> lock(errandMutex_);
    if (stopping_) {
        return;
    }
    if (byHand) {
        aliasErrandByHand_ = true;
        const std::lock_guard<std::mutex> held(stateMutex_);
        aliases_.checked.reset();
    }
    if (aliasErrandRunning_) {
        return;
    }
    if (aliasErrand_.joinable()) {
        aliasErrand_.join();
    }
    aliasErrandRunning_ = true;
    {
        const std::lock_guard<std::mutex> held(stateMutex_);
        aliases_.checking = true;
    }
    aliasErrand_ = std::thread(
        [this, context = std::move(context)]() { runAliasErrand(context); });
}

void Account::runAliasErrand(const client::Session::AliasErrandContext& context)
{
    const client::Session::AliasErrandResult result = client::Session::runAliasErrand(context);
    bool ok = result.ok;
    std::string note = result.error;
    if (result.ok) {
        try {
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            if (session_ != nullptr) {
                aliasesAnswered_ = true;
                session_->applyAliasErrand(result);
                noteAliases(*session_);
                if (session_->aliasNames().empty()) {
                    note = "No alias is registered to this account";
                } else if (result.pointed) {
                    note = "The aliases now point here";
                } else if (session_->aliasUpdatePending()) {
                    ok = false;
                    note = "The registry did not take the update. It will be tried again.";
                } else {
                    note = "The aliases are up to date";
                }
            }
        } catch (const std::exception& error) {
            ok = false;
            note = error.what();
        }
    }
    bool byHand = false;
    {
        const std::lock_guard<std::mutex> lock(errandMutex_);
        byHand = aliasErrandByHand_;
        aliasErrandByHand_ = false;
        aliasErrandRunning_ = false;
        const std::lock_guard<std::mutex> held(stateMutex_);
        aliases_.checking = false;
        if (byHand) {
            aliases_.checked = AliasCheck{ok, note};
        }
    }
    if (!ok && !byHand) {
        log::info("account {}: alias servicing will try again: {}", id_, note);
    }
}

void Account::editMessage(
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

void Account::deleteMessage(const std::string& peer, const std::string& messageId)
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

void Account::react(
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

void Account::confirmReceipt(const std::string& peer, const std::string& messageId)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().sendReceipt(peer, messageId);
}

std::int64_t Account::lastEventId() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return transcript_ == nullptr ? 0 : transcript_->lastEventId();
}

std::vector<Event> Account::await(const std::int64_t sinceId, const int seconds) const
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

}  // namespace bazarish::bot
