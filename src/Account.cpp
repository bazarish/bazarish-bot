// Bazarish project (c) 2026
#include "Account.hpp"
#include "MenuRules.hpp"

#include <ApiClient.hpp>

#include <bazarish/Bytes.hpp>
#include <bazarish/Clock.hpp>
#include <bazarish/JsonRead.hpp>
#include <bazarish/Log.hpp>
#include <bazarish/PathText.hpp>

#include <nlohmann/json.hpp>

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
constexpr const char* kClosed = "closed";
constexpr const char* kStarting = "starting";
constexpr const char* kRunning = "running";

client::TransferEventFn transferHandler(const std::shared_ptr<Transfers>& transfers,
    const std::shared_ptr<Transcript>& transcript,
    const std::shared_ptr<Wakeup>& wakeup)
{
    return [transfers, transcript, wakeup](const client::TransferEvent& event) {
        const std::optional<Transfer> moved = transfers->note(event);
        if (!moved.has_value()) {
            return;
        }
        if (!moved->outgoing && moved->state == transferStateName(client::TransferState::eDone)) {
            transcript->keepFileAt(moved->peer, moved->messageId, moved->path);
        }
        transcript->noteTransfer(moved->peer, moved->messageId, moved->state);
        wakeup->wake();
    };
}

}  // namespace

Account::Account(
    std::string id, fs::path accountFile, Settings& settings, ServerSideFn serverSide)
    : id_(std::move(id))
    , accountFile_(std::move(accountFile))
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

bool Account::live() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return status_ == kRunning || status_ == kStarting;
}

std::string Account::status() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return status_;
}

void Account::open()
{
    const std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
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
    stopping_ = false;
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
    client::Session::assignHostedKind(accountFile_, storedAccountKind(settings_.kind(id_)));
    std::unique_ptr<client::Session> session = client::Session::open(accountFile_);
    applyKind(*session);
    session->setDeviceSync(false);
    const std::string fingerprint = session->fingerprint();
    settings_.rememberAccount(id_, fingerprint, settings_.kind(id_));
    mailbox.admit(fingerprint);

    const std::string destination = mailbox.serveDestinationFor(fingerprint, session->addressKeys());
    log::info("account {}: destination {}", id_,
        destination.empty() ? "still building" : destination);

    client::ServerEndpoint endpoint;
    endpoint.serverFingerprint = mailbox.fingerprint();
    endpoint.facades.push_back(client::Facade{mailbox.host(), mailbox.port()});
    session->connectServer(endpoint);
    session->registerSelfHosted();
    session->setAcceptCalls(false);
    log::info("account {}: registered and up", id_);
    const client::Session::ContactFetchContext waiterContext = session->contactFetchContext();
    const std::shared_ptr<client::LoginSigner> signer = session->loginSigner();
    noteAliases(*session);

    auto transcript
        = std::make_shared<Transcript>(accountFile_, fingerprint, settings_.keepsHistory(id_));
    transcript->failUnfinished();
    auto transfers = std::make_shared<Transfers>();
    session->setFileSource(files_);
    session->setTransferHandler(transferHandler(transfers, transcript, wakeup_));
    std::shared_ptr<GroupEngine> group;
    if (settings_.kind(id_) == Settings::AccountKind::eGroup) {
        group = std::make_shared<GroupEngine>(
            accountFile_, groupLink(session->relayFitter()), []() { return nowMillis(); });
        session->setGroupBlockSource(
            [engine = std::weak_ptr<GroupEngine>(group)](const std::string& peer) {
                const std::shared_ptr<GroupEngine> held = engine.lock();
                if (held == nullptr) {
                    throw std::runtime_error("the group is closed");
                }
                return held->blockFor(peer);
            });
    }

    cardCarriesDestination_ = false;
    lastAliasErrand_ = 0;
    aliasesAnswered_ = false;
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        fingerprint_ = fingerprint;
        name_ = session->displayName();
        signer_ = signer;
    }
    std::shared_ptr<GroupEngine> started;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        mailbox_ = std::ref(mailbox);
        session_ = std::move(session);
        transcript_ = std::move(transcript);
        transfers_ = std::move(transfers);
        group_ = std::move(group);
        started = group_;
    }
    if (started != nullptr) {
        started->start();
    }
    {
        const std::lock_guard<std::mutex> lock(stateMutex_);
        if (!stopping_) {
            status_ = kRunning;
        }
    }
    maintainer_ = std::thread([this]() { maintain(); });
    eventWaiter_ = std::thread([this, waiterContext]() { waitForEvents(waiterContext); });
}

void Account::close()
{
    const std::lock_guard<std::mutex> lifecycle(lifecycleMutex_);
    {
        const std::lock_guard<std::mutex> lock(wakeup_->mutex);
        stopping_ = true;
    }
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        if (mailbox_.has_value() && session_ != nullptr) {
            mailbox_->get().endMailWait(session_->fingerprint());
        }
    }
    if (starter_.joinable()) {
        starter_.join();
    }
    wakeup_->wake();
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
    std::shared_ptr<GroupEngine> group;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        group = group_;
    }
    if (group != nullptr) {
        group->stop();
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
        transfers_.reset();
    }
    session.reset();
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        group_.reset();
    }
}

GroupLink Account::groupLink(client::RelayFitter fitting)
{
    const auto watching = [](GroupLink::Outcome done) {
        client::DeliveryWatch watch;
        watch.onOutcome = std::move(done);
        return watch;
    };
    GroupLink link;
    link.accept = [this](const std::string& peer) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().acceptContactRequest(peer);
    };
    link.tell = [this, watching](const std::string& peer, const std::string& text,
                    const client::InlineKeyboard& keyboard, GroupLink::Outcome done) {
        const std::string id = client::newMessageId();
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        if (keyboard.empty()) {
            (void)openSession().sendMessage(peer, text, id, watching(std::move(done)));
        } else {
            openSession().sendInteractive(peer, text, keyboard, id, watching(std::move(done)));
        }
        return id;
    };
    link.retell = [this](const std::string& peer, const std::string& id, const std::string& text,
                      const client::InlineKeyboard& keyboard) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        (void)openSession().sendEdit(peer, id, text, keyboard);
    };
    link.relay = [this, watching](const std::string& peer,
                     const std::vector<client::RelayedPost>& posts, GroupLink::Outcome done) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        (void)openSession().relayPosts(peer, posts, watching(std::move(done)));
    };
    link.receipt = [this](const std::string& peer, const std::string& messageId) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().sendReceipt(peer, messageId);
    };
    link.state = [this, watching](const std::string& peer, GroupLink::Outcome done) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().sendGroupState(peer, watching(std::move(done)));
    };
    link.descriptor = [this](const std::string& peer) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        return openSession().contactInviteUri(peer);
    };
    link.name = [this](const std::string& peer) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        return openSession().contactDisplayName(peer);
    };
    link.forget = [this](const std::string& peer) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().removeContact(peer);
    };
    link.fitting = std::move(fitting);
    link.media = [this](const std::string& id, const std::string& type) {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        return type == "image" ? openSession().picture(id) : openSession().voice(id);
    };
    return link;
}

std::string Account::postToGroup(const std::string& text)
{
    const std::string id = client::newMessageId();
    group()->hostPost(id, text);
    return id;
}

std::shared_ptr<GroupEngine> Account::group() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (group_ == nullptr) {
        throw std::runtime_error("this account is not a group, or it is not open");
    }
    return group_;
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
    const bool group = settings_.kind(id_) == Settings::AccountKind::eGroup;
    session.setOutboundAddressing(group ? client::OutboundAddressing::eOneForAll
                                        : client::OutboundAddressing::eOnePerCorrespondent);
    if (group) {
        session.setDeliveryWorkers(kMaxRelaysInFlight + client::kDeliveryWorkers);
    }
}

Bytes Account::exportBundle(const std::string& bundlePassphrase)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().exportAccountBytes(bundlePassphrase);
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

void Account::setAvatar(const Bytes& data, const std::string& mime)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().setAvatar(data, mime);
}

Account::Avatar Account::avatar() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    const client::Session& session = openSession();
    return Avatar{session.avatar(), session.avatarMime()};
}

void Account::newAddress()
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    session.issueAddressKeys();
    const std::string address = mailbox_->get().replaceDestinationFor(fingerprint(), session.addressKeys());
    cardCarriesDestination_ = false;
    log::info("account {}: address replaced: {}", id_, address.empty() ? "building" : address);
}

void Account::maintain()
{
    const std::string fingerprint = this->fingerprint();
    while (!stopping_) {
        if (mailbox_->get().destinationNeedsRenewal(fingerprint, bazarish::nowSeconds())) {
            try {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (session_ != nullptr) {
                    mailbox_->get().serveDestinationFor(fingerprint, session_->addressKeys());
                    log::info("account {}: delegation re-issued for {}", id_,
                        mailbox_->get().destination(fingerprint));
                }
            } catch (const std::exception& error) {
                log::warn("account {}: delegation not re-issued: {}", id_, error.what());
            }
        }

        try {
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            if (session_ != nullptr) {
                session_->flushPendingEchoes();
            }
        } catch (const std::exception& error) {
            log::warn("account {}: echoes not flushed: {}", id_, error.what());
        }

        {
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            if (session_ != nullptr) {
                session_->expireFileRequests(nowMillis());
            }
        }

        try {
            bool swept = false;
            {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                swept = transcript_ != nullptr && transcript_->sweepExpired(nowMillis());
            }
            if (swept) {
                wakeup_->wake();
            }
        } catch (const std::exception& error) {
            log::warn("account {}: expired messages not removed: {}", id_, error.what());
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

        if (!cardCarriesDestination_ && !mailbox_->get().destination(fingerprint).empty()) {
            try {
                const std::lock_guard<std::mutex> lock(sessionMutex_);
                if (session_ != nullptr) {
                    const bool moved = session_->refreshOwnCard();
                    cardCarriesDestination_ = true;
                    log::info("account {}: card re-issued with the destination {}", id_,
                        mailbox_->get().destination(fingerprint));
                    if (moved) {
                        session_->announceNewRouting();
                    } else {
                        session_->runRoutingFanout();
                    }
                }
            } catch (const std::exception& error) {
                log::warn("account {}: card not re-issued yet: {}", id_, error.what());
            }
        }

        std::unique_lock<std::mutex> lock(wakeup_->mutex);
        wakeup_->changed.wait_for(lock, std::chrono::seconds(kMaintenanceSeconds),
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
        const auto askedAt = std::chrono::steady_clock::now();
        try {
            if (waiter == nullptr) {
                waiter = client::Session::makeEventClient(context);
            }
            waiting = client::Session::waitForMail(*waiter, kEventWaitSeconds);
        } catch (const std::exception& error) {
            log::info("mail wait failed, asking again: {}", error.what());
            if (const auto* refusal = dynamic_cast<const client::ApiError*>(&error);
                refusal != nullptr && refusal->code == ErrorCode::eClientUnregistered) {
                registerAgain();
            }
            std::unique_lock<std::mutex> lock(wakeup_->mutex);
            wakeup_->changed.wait_until(lock, askedAt + std::chrono::seconds(kEventWaitSeconds),
                [this]() { return stopping_.load(); });
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
        arrived = session_->sync(false);
    } catch (const std::exception& error) {
        log::warn("account {}: mailbox: {}", id_, error.what());
    }

    std::shared_ptr<GroupEngine> group;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        group = group_;
    }
    if (group != nullptr) {
        for (const client::IncomingMessage& item : arrived) {
            try {
                group->onIncoming(item);
                acknowledge(item);
            } catch (const std::exception& error) {
                log::warn("account {}: group: {}", id_, error.what());
            }
        }
        return;
    }

    bool wrote = false;
    for (const client::IncomingMessage& item : arrived) {
        try {
            const std::lock_guard<std::mutex> lock(sessionMutex_);
            if (transcript_ == nullptr) {
                continue;
            }
            wrote = storeIncoming(item) || wrote;
            if (!item.pendingId.empty()) {
                session_->ackPending(item.pendingId);
            }
        } catch (const std::exception& error) {
            log::warn("account {}: transcript: {}", id_, error.what());
        }
    }

    if (wrote) {
        wakeup_->wake();
    }
}

void Account::acknowledge(const client::IncomingMessage& item)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (session_ != nullptr && !item.pendingId.empty()) {
        session_->ackPending(item.pendingId);
    }
}

void Account::registerAgain()
{
    try {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().registerSelfHosted();
        log::info("account {}: registered again", id_);
    } catch (const std::exception& error) {
        log::warn("account {}: registering again: {}", id_, error.what());
    }
}

bool Account::storeIncoming(const client::IncomingMessage& item)
{
    const Transcript::Taken taken = transcript_->takeIn(item);
    if (taken.added && !item.sentByUs) {
        const std::optional<client::BotMenu> menu = transcript_->menu(item.fromFingerprint);
        if (owesMenu(menu, item)) {
            openSession().sendMenu(item.fromFingerprint, *menu);
            log::info("account {}: menu {} sent again to {}", id_, menu->id,
                log::redact(item.fromFingerprint));
        }
    }
    if (item.contentType == client::kTypeContactRequest && !item.sentByUs
        && settings_.acceptsContacts(id_)) {
        openSession().acceptContactRequest(item.fromFingerprint);
    }
    return taken.wrote;
}

std::string Account::destination() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return status_ == kRunning ? mailbox_->get().destination(fingerprint_) : std::string();
}

std::string Account::fingerprint() const
{
    const std::lock_guard<std::mutex> lock(stateMutex_);
    return fingerprint_;
}

void Account::refuseOwnFingerprint(const std::string& peer) const
{
    if (peer == fingerprint()) {
        throw std::invalid_argument("a bot account has no chat with itself");
    }
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
            session.contactIsPending(fingerprint), session.contactAcceptInFlight(fingerprint),
            session.canWriteTo(fingerprint)});
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

client::DeliveryWatch Account::statusWatch(const std::string& peer, const std::string& messageId)
{
    client::DeliveryWatch watch;
    watch.onOutcome = [this, transcript = std::weak_ptr<Transcript>(transcript_), peer, messageId](
                          const client::OutboundCourier::Outcome& outcome) {
        const std::string status = outcome.stored
            ? std::string("delivered")
            : "failed: "
                + (outcome.errorMessage.empty() ? outcome.errorCode : outcome.errorMessage);
        try {
            if (const std::shared_ptr<Transcript> held = transcript.lock()) {
                held->setStatus(peer, messageId, status);
            }
        } catch (const std::exception& error) {
            log::warn("account {}: transcript: {}", id_, error.what());
        }
        wakeup_->wake();
    };
    return watch;
}

std::string Account::sendText(const std::string& peer, const std::string& text,
    const std::string& replyTo, const bool forwarded)
{
    refuseOwnFingerprint(peer);
    Line line;
    line.e2eId = client::newMessageId();
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        (void)session.sendMessage(
            peer, text, line.e2eId, statusWatch(peer, line.e2eId), replyTo, forwarded);
        line.status = "sending";
        line.peer = peer;
        line.outgoing = true;
        line.type = "text";
        line.text = text;
        line.replyTo = replyTo;
        line.sentAt = nowMillis();
        transcript_->append(line);
    }
    wakeup_->wake();
    return line.e2eId;
}

std::string Account::forward(
    const std::string& fromPeer, const std::string& messageId, const std::string& toPeer)
{
    refuseOwnFingerprint(toPeer);
    std::optional<Line> original;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        (void)openSession();
        original = transcript_->find(fromPeer, messageId);
    }
    if (!original.has_value()) {
        throw std::invalid_argument("no message " + messageId + " in that conversation");
    }
    if (original->type != "text") {
        throw std::invalid_argument("only a text message can be forwarded");
    }
    return sendText(toPeer, original->text, {}, true);
}

std::string Account::sendKeyboard(
    const std::string& peer, const std::string& text, const std::string& keyboardJson)
{
    refuseOwnFingerprint(peer);
    client::InlineKeyboard keyboard;
    for (const nlohmann::json& row : bazarish::parseJson(keyboardJson)) {
        std::vector<client::InlineButton> buttons;
        for (const nlohmann::json& button : row) {
            buttons.push_back({button.value("text", std::string()),
                button.value("data", std::string()), button.value("command", std::string())});
        }
        keyboard.push_back(std::move(buttons));
    }
    Line line;
    line.e2eId = client::newMessageId();
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        session.sendInteractive(peer, text, keyboard, line.e2eId, statusWatch(peer, line.e2eId));
        line.status = "sending";
        line.peer = peer;
        line.outgoing = true;
        line.type = "text";
        line.text = text;
        line.keyboard = client::inlineKeyboardJson(keyboard);
        line.sentAt = nowMillis();
        transcript_->append(line);
    }
    wakeup_->wake();
    return line.e2eId;
}

std::string Account::sendPicture(const std::string& peer, const Bytes& bytes,
    const std::string& name, const std::string& mime, const std::string& replyTo)
{
    refuseOwnFingerprint(peer);
    Line line;
    line.e2eId = client::newMessageId();
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        (void)session.sendPicture(
            peer, bytes, name, mime, line.e2eId, statusWatch(peer, line.e2eId), replyTo);
        line.status = "sending";
        line.peer = peer;
        line.outgoing = true;
        line.type = "image";
        line.text = name;
        line.replyTo = replyTo;
        line.sentAt = nowMillis();
        transcript_->append(line);
    }
    wakeup_->wake();
    return line.e2eId;
}

std::string Account::sendFile(
    const std::string& peer, const std::string& path, const std::string& replyTo)
{
    refuseOwnFingerprint(peer);
    Line line;
    line.e2eId = client::newMessageId();
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        (void)session.sendFile(peer, path, line.e2eId, statusWatch(peer, line.e2eId), replyTo);
        line.status = "sending";
        line.peer = peer;
        line.outgoing = true;
        line.type = client::kTypeFile;
        line.text = files_->name(path);
        line.file = FileInfo{
            line.text, std::filesystem::file_size(pathFromUtf8(path)), {}, {}, path};
        line.replyTo = replyTo;
        line.sentAt = nowMillis();
        transcript_->append(line);
    }
    wakeup_->wake();
    return line.e2eId;
}

FileCheck Account::download(
    const std::string& peer, const std::string& messageId, const std::string& path)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    const std::optional<Line> line = transcript_->find(peer, messageId);
    if (!line.has_value() || line->outgoing || !line->file.has_value()) {
        return FileCheck{false, kFileMissing, "no file came in the message " + messageId};
    }
    if (transfers_->active(peer, messageId)) {
        return FileCheck{false, kFileInTheWay, "this file is already on its way"};
    }
    transfers_->expect(peer, messageId, path);
    try {
        session.requestFile(
            peer, messageId, line->file->sha256, client::fileTargetAt(pathFromUtf8(path)));
    } catch (const std::exception& error) {
        (void)transfers_->note(client::TransferEvent{messageId, peer,
            client::TransferState::eFailed, 0, 0, error.what(), client::TransferStage::eNone});
        throw;
    }
    return FileCheck{true, 0, {}};
}

std::vector<Transfer> Account::transfers() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return transfers_ == nullptr ? std::vector<Transfer>() : transfers_->all();
}

std::optional<Transfer> Account::transfer(
    const std::string& peer, const std::string& messageId) const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return transfers_ == nullptr ? std::nullopt : transfers_->find(peer, messageId);
}

bool Account::cancelTransfer(const std::string& peer, const std::string& messageId)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    if (!transfers_->active(peer, messageId)) {
        return false;
    }
    session.cancelTransfer(messageId);
    return true;
}

bool Account::setMenu(const std::string& peer, std::vector<client::BotMenuItem> items)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    client::Session& session = openSession();
    const std::optional<client::BotMenu> menu
        = replacingMenu(transcript_->menu(peer), std::move(items));
    if (!menu) {
        return false;
    }
    session.sendMenu(peer, *menu);
    transcript_->setMenu(peer, *menu);
    return true;
}

void Account::clearChat(const std::string& peer, const bool forEveryone)
{
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        client::Session& session = openSession();
        if (forEveryone) {
            session.sendChatClear(peer);
        }
        transcript_->clearPeer(peer);
    }
    wakeup_->wake();
}

void Account::setChatTimer(const std::string& peer, const std::int64_t seconds)
{
    if (kind() == Settings::AccountKind::eGroup) {
        throw std::invalid_argument("a group carries no auto-delete timer");
    }
    if (!client::isChatTimerChoice(seconds)) {
        throw std::invalid_argument("not an auto-delete timer a chat can carry");
    }
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        openSession().sendChatTimer(peer, seconds);
        transcript_->setTimer(peer, seconds);
    }
    wakeup_->wake();
}

std::int64_t Account::chatTimer(const std::string& peer) const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return transcript_->timer(peer);
}

client::DatabaseFootprint Account::storage() const
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    return transcript_->footprint();
}

client::DatabaseFootprint Account::compact()
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        throw std::runtime_error("the account is not open");
    }
    transcript_->rebuild();
    return transcript_->footprint();
}

void Account::acceptContact(const std::string& peer)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().acceptContactRequest(peer);
}

void Account::removeContact(const std::string& peer)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().removeContactEverywhere(peer);
}

std::string Account::addContact(
    const bool byAlias, const std::string& inviteOrAlias, const std::string& text)
{
    client::Session::ContactFetchContext context;
    {
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        context = openSession().contactFetchContext();
    }
    const client::Session::ContactCardResolved resolved = client::Session::resolveContactCard(
        context, {byAlias, inviteOrAlias, text, {}}, [](client::Session::CardStep) {});
    if (!resolved.ok) {
        throw std::runtime_error(resolved.error);
    }
    client::DeliveryWatch watch;
    watch.onOutcome = [this, peer = resolved.fingerprint](
                          const client::OutboundCourier::Outcome& outcome) {
        if (!outcome.stored) {
            log::info("account {}: contact request to {} not taken: {}", id_, log::redact(peer),
                outcome.errorMessage.empty() ? outcome.errorCode : outcome.errorMessage);
        }
        const std::lock_guard<std::mutex> lock(sessionMutex_);
        if (session_ != nullptr) {
            session_->settleContactRequest(peer, outcome);
        }
    };
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    return openSession().commitContactAdd(resolved, watch);
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
    wakeup_->wake();
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
    wakeup_->wake();
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
    wakeup_->wake();
}

void Account::confirmReceipt(const std::string& peer, const std::string& messageId)
{
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    openSession().sendReceipt(peer, messageId);
    transcript_->markRead(peer, messageId);
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
    const int wait = seconds > kMaxWaitSeconds ? kMaxWaitSeconds : seconds;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(wait < 0 ? 0 : wait);
    while (true) {
        std::uint64_t seen = 0;
        {
            const std::lock_guard<std::mutex> lock(wakeup_->mutex);
            seen = wakeup_->events;
        }
        if (stopping_.load() || ready()) {
            break;
        }
        std::unique_lock<std::mutex> lock(wakeup_->mutex);
        if (!wakeup_->changed.wait_until(lock, until,
                [this, seen]() { return stopping_.load() || wakeup_->events != seen; })) {
            break;
        }
    }
    const std::lock_guard<std::mutex> lock(sessionMutex_);
    if (transcript_ == nullptr) {
        return {};
    }
    return transcript_->since(sinceId);
}

void Wakeup::wake()
{
    {
        const std::lock_guard<std::mutex> lock(mutex);
        ++events;
    }
    changed.notify_all();
}

}  // namespace bazarish::bot
