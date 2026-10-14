// Bazarish project (c) 2026
#include "GroupEngine.hpp"

#include "NameRules.hpp"

#include <bazarish/Descriptor.hpp>
#include <bazarish/Log.hpp>

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace bazarish::bot {

namespace {

constexpr std::int64_t kPumpIdleMs = 1000;
constexpr std::int64_t kLogBatch = 64;
constexpr std::size_t kMaxUtf8Bytes = 4;
constexpr const char* kDeliveryRejected = "DELIVERY_REJECTED";
constexpr const char* kApprove = "approve:";
constexpr const char* kDecline = "decline:";
constexpr const char* kSettingsCommand = "settings";
constexpr const char* kSettingsTyped = "/settings";
constexpr const char* kContactableCommand = "contactable";
constexpr const char* kRenameCommand = "rename";
constexpr const char* kNameToken = "{name}";
constexpr const char* kParagraph = "\n\n";
constexpr std::size_t kMaxGroupTextBytes = 1024;
constexpr const char* kTypeText = "text";
constexpr const char* kTypeImage = "image";
constexpr const char* kTypeAudio = "audio";

bool isPost(const std::string& type)
{
    return type == kTypeText || type == kTypeImage || type == kTypeAudio;
}

std::pair<std::string, std::string> splitCommand(const std::string& line)
{
    const std::size_t space = line.find(' ');
    if (space == std::string::npos) {
        return {line, {}};
    }
    return {line.substr(0, space), line.substr(space + 1)};
}

std::string_view trimmed(std::string_view text)
{
    constexpr std::string_view kBlank = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kBlank);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(kBlank) - first + 1);
}

bool startsWith(const std::string& text, const std::string& prefix)
{
    return text.rfind(prefix, 0) == 0;
}

}  // namespace

GroupEngine::GroupEngine(const std::filesystem::path& file, const std::string& passphrase,
    GroupLink link, Clock clock)
    : store_(file, passphrase)
    , link_(std::move(link))
    , clock_(std::move(clock))
    , config_(store_.config())
{
    for (const Member& member : store_.members()) {
        members_[member.fingerprint] = member;
    }
    const std::int64_t now = clock_();
    countedAt_ = now;
    flushedAt_ = now;
    memberCount_ = static_cast<std::int64_t>(std::count_if(members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.state == MemberState::eMember; }));
}

GroupEngine::~GroupEngine()
{
    stop();
}

void GroupEngine::start()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = false;
    pump_ = std::thread([this]() { loop(); });
}

void GroupEngine::stop()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (pump_.joinable()) {
        pump_.join();
    }
}

void GroupEngine::nudge()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        nudged_ = true;
    }
    wake_.notify_all();
}

void GroupEngine::loop()
{
    while (true) {
        bool countDue = false;
        bool flushDue = false;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) {
                return;
            }
            const std::int64_t now = clock_();
            countDue = now - countedAt_ >= kMemberCountPeriodMs;
            flushDue = now - flushedAt_ >= kReactionFlushMs;
        }
        if (countDue) {
            refreshMemberCount();
        }
        if (flushDue) {
            flushReactions();
        }
        pumpOnce();
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(kPumpIdleMs),
            [this]() { return stopping_ || nudged_; });
        nudged_ = false;
    }
}

void GroupEngine::run(Actions& actions)
{
    for (const std::function<void()>& action : actions) {
        try {
            action();
        } catch (const std::exception& error) {
            log::warn("group: a send did not leave: {}", error.what());
        }
    }
}

std::string GroupEngine::text(const Copy key, const std::vector<std::string>& values) const
{
    return copyText(config_.language, key, values);
}

void GroupEngine::saveLocked(const Member& member)
{
    members_[member.fingerprint] = member;
    store_.putMember(member);
}

bool GroupEngine::writesLocked(const Member& member) const
{
    if (member.state == MemberState::eNaming || member.state == MemberState::eLeft) {
        return true;
    }
    if (member.state != MemberState::eMember) {
        return false;
    }
    return config_.kind == client::GroupKind::eGroup || member.admin;
}

client::GroupBlock GroupEngine::blockFor(const std::string& peer) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    client::GroupBlock block;
    block.kind = config_.kind;
    block.members = memberCount_;
    const auto found = members_.find(peer);
    if (found != members_.end()) {
        block.admin = found->second.admin;
        block.write = writesLocked(found->second);
    }
    block.at = clock_();
    return block;
}

void GroupEngine::tellLocked(const std::string& peer, const Copy key,
    const std::vector<std::string>& values, Actions& actions,
    const client::InlineKeyboard& keyboard)
{
    sayLocked(peer, text(key, values), actions, keyboard);
}

void GroupEngine::sayLocked(const std::string& peer, const std::string& words, Actions& actions,
    const client::InlineKeyboard& keyboard)
{
    actions.push_back([this, peer, words, keyboard]() {
        (void)link_.tell(peer, words, keyboard, watchRefusal(peer));
    });
}

void GroupEngine::askNameLocked(const std::string& peer, Actions& actions)
{
    sayLocked(peer, config_.greeting.empty() ? text(Copy::eAskName) : config_.greeting, actions);
}

std::string GroupEngine::welcomeTextLocked(const Member& member) const
{
    std::string welcome = config_.welcome;
    if (welcome.empty()) {
        welcome = text(Copy::eWelcome, {member.name});
    }
    const std::string token = kNameToken;
    for (std::size_t at = welcome.find(token); at != std::string::npos;
         at = welcome.find(token, at + member.name.size())) {
        welcome.replace(at, token.size(), member.name);
    }
    return welcome + kParagraph + text(Copy::eSettingsHint);
}

void GroupEngine::offerSettingsLocked(const Member& member, Actions& actions)
{
    sayLocked(member.fingerprint, text(Copy::eSettingsHint), actions, settingsButtonLocked());
}

void GroupEngine::showSettingsLocked(
    const Member& member, const std::string& ref, Actions& actions)
{
    if (ref.empty()) {
        return;
    }
    const std::string words
        = ref == member.welcomeId ? welcomeTextLocked(member) : text(Copy::eSettingsHint);
    actions.push_back([this, peer = member.fingerprint, ref, words,
                          keyboard = settingsKeyboardLocked(member)]() {
        link_.retell(peer, ref, words, keyboard);
    });
}

client::InlineKeyboard GroupEngine::settingsButtonLocked() const
{
    return {{{text(Copy::eButtonSettings), kSettingsCommand, {}}}};
}

client::InlineKeyboard GroupEngine::settingsKeyboardLocked(const Member& member) const
{
    const Copy toggle = member.contactable ? Copy::eButtonContactOff : Copy::eButtonContactOn;
    const std::string contactable = kContactableCommand;
    return {
        {{text(toggle), contactable + (member.contactable ? " off" : " on"), {}}},
        {{text(Copy::eButtonRename), kRenameCommand, {}}},
    };
}

void GroupEngine::onIncoming(const client::IncomingMessage& message)
{
    Prefetched prefetched;
    if (!message.sentByUs && isPost(message.contentType)) {
        if (message.contentType != kTypeText) {
            prefetched.media = link_.media(message.e2eId, message.contentType);
        }
        client::RelayedPost sizing;
        sizing.type = message.contentType;
        sizing.id = message.e2eId;
        sizing.text = message.text;
        sizing.replyTo = message.replyTo;
        sizing.media = prefetched.media.value_or(Bytes());
        sizing.mediaName = message.attachmentName;
        sizing.mime = message.attachmentMime;
        sizing.durationMs = message.attachmentDurationMs;
        sizing.author = std::string(kMaxNameCodepoints * kMaxUtf8Bytes, 'x');
        sizing.sentAt = std::numeric_limits<std::int64_t>::max();
        prefetched.fits = link_.fits(sizing);
    }
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        handleLocked(message, prefetched, actions);
        receiptLocked(message, actions);
    }
    run(actions);
    nudge();
}

void GroupEngine::receiptLocked(const client::IncomingMessage& message, Actions& actions)
{
    if (message.sentByUs || !isPost(message.contentType) || message.e2eId.empty()) {
        return;
    }
    const auto found = members_.find(message.fromFingerprint);
    if (found == members_.end() || found->second.state == MemberState::eBanned) {
        return;
    }
    actions.push_back([this, peer = message.fromFingerprint, id = message.e2eId]() {
        link_.receipt(peer, id);
    });
}

void GroupEngine::handleLocked(
    const client::IncomingMessage& message, const Prefetched& prefetched, Actions& actions)
{
    if (message.sentByUs) {
        return;
    }
    const std::string& peer = message.fromFingerprint;
    const auto found = members_.find(peer);

    if (message.contentType == "contact.request") {
        if (found != members_.end() && found->second.state == MemberState::eBanned) {
            return;
        }
        actions.push_back([this, peer]() { link_.accept(peer); });
        if (found != members_.end() && found->second.state != MemberState::eLeft) {
            return;
        }
        if (found == members_.end() || found->second.name.empty()) {
            Member member = found == members_.end() ? Member{} : found->second;
            member.fingerprint = peer;
            member.state = MemberState::eNaming;
            saveLocked(member);
            askNameLocked(peer, actions);
            return;
        }
        Member member = found->second;
        joinLocked(member, actions);
        return;
    }
    if (found == members_.end()) {
        return;
    }
    Member member = found->second;

    switch (member.state) {
    case MemberState::eBanned:
        return;
    case MemberState::eLeft:
        if (!isPost(message.contentType)) {
            return;
        }
        if (member.name.empty()) {
            member.state = MemberState::eNaming;
            saveLocked(member);
            askNameLocked(peer, actions);
        } else {
            joinLocked(member, actions);
        }
        return;
    case MemberState::eNaming: {
        if (!isPost(message.contentType)) {
            return;
        }
        if (message.contentType != kTypeText) {
            askNameLocked(peer, actions);
            return;
        }
        const std::optional<NameCheck> check = freeNameLocked(peer, message.text, actions);
        if (!check) {
            return;
        }
        member.name = check->name;
        member.nameKey = check->key;
        joinLocked(member, actions);
        return;
    }
    case MemberState::ePending:
        if (isPost(message.contentType)) {
            tellLocked(peer, Copy::eAwaitingApproval, {}, actions);
        }
        return;
    case MemberState::eMember:
        break;
    }

    if (message.contentType == kTypeText && trimmed(message.text) == kSettingsTyped) {
        offerSettingsLocked(member, actions);
        return;
    }
    if (isPost(message.contentType) && renaming_.count(peer) != 0) {
        renameLocked(member, message, actions);
        return;
    }
    if (isPost(message.contentType)) {
        postLocked(member, message, prefetched, actions);
        return;
    }
    if (message.contentType == "edit") {
        std::optional<Post> post = store_.post(message.refId);
        if (post && post->author == peer && !post->deleted && post->type == kTypeText) {
            post->text = message.text;
            post->edited = true;
            store_.updatePost(*post);
            (void)store_.appendLog(LogKind::eEdit, post->id);
        }
        return;
    }
    if (message.contentType == "delete") {
        const std::optional<Post> post = store_.post(message.refId);
        if (post && post->author == peer) {
            deletePostLocked(post->id);
        }
        return;
    }
    if (message.contentType == "reaction") {
        const std::optional<Post> post = store_.post(message.refId);
        if (post && !post->deleted) {
            store_.setReaction(post->id, peer, message.text);
            if (std::find(dirtyReactions_.begin(), dirtyReactions_.end(), post->id)
                == dirtyReactions_.end()) {
                dirtyReactions_.push_back(post->id);
            }
        }
        return;
    }
    if (message.contentType == "bot.command") {
        commandLocked(member,
            message.commandArgs.empty() ? message.commandName
                                        : message.commandName + " " + message.commandArgs,
            actions);
        return;
    }
    if (message.contentType == "bot.callback") {
        const std::string& data = message.callbackData;
        const bool approve = startsWith(data, kApprove);
        if ((approve || startsWith(data, kDecline)) && member.admin) {
            const std::string number
                = data.substr(std::string(approve ? kApprove : kDecline).size());
            decideLocked(std::stoll(number), approve, member.name, actions);
            return;
        }
        if (data == kSettingsCommand) {
            showSettingsLocked(member, message.refId, actions);
            return;
        }
        commandLocked(member, data, actions);
        if (startsWith(data, kContactableCommand)) {
            showSettingsLocked(member, message.refId, actions);
        }
    }
}

void GroupEngine::joinLocked(Member& member, Actions& actions)
{
    if (config_.joinMode == JoinMode::eOpen || member.admin) {
        admitLocked(member, actions);
        return;
    }
    member.state = MemberState::ePending;
    saveLocked(member);
    const std::int64_t number = store_.addRequest(member.fingerprint, clock_());
    tellLocked(member.fingerprint, Copy::eAwaitingApproval, {}, actions);
    const client::InlineKeyboard keyboard{{
        {text(Copy::eButtonAllow), kApprove + std::to_string(number), {}},
        {text(Copy::eButtonDecline), kDecline + std::to_string(number), {}},
    }};
    for (const auto& [fingerprint, admin] : members_) {
        if (!admin.admin || admin.state != MemberState::eMember) {
            continue;
        }
        actions.push_back([this, admin = fingerprint, applicant = member, number, keyboard]() {
            const std::string card = copyText(config().language, Copy::eWantsToJoin,
                {applicant.name, chatDescriptor(applicant)});
            const std::string id = link_.tell(admin, card, keyboard, watchRefusal(admin));
            const std::lock_guard<std::mutex> lock(mutex_);
            store_.addCard(number, admin, id);
        });
    }
}

void GroupEngine::admitLocked(Member& member, Actions& actions)
{
    member.state = MemberState::eMember;
    if (member.joinedAt == 0) {
        member.joinedAt = clock_();
    }
    member.cursor = store_.seqBeforeLastPosts(config_.historySize);
    member.historyFrom = member.cursor;
    saveLocked(member);
    actions.push_back([this, peer = member.fingerprint, words = welcomeTextLocked(member),
                          keyboard = settingsButtonLocked()]() {
        const std::string id = link_.tell(peer, words, keyboard, watchRefusal(peer));
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(peer);
        if (found != members_.end()) {
            Member held = found->second;
            held.welcomeId = id;
            saveLocked(held);
        }
    });
}

void GroupEngine::decideLocked(
    const std::int64_t number, const bool allow, const std::string& byName, Actions& actions)
{
    const std::optional<JoinRequest> request = store_.request(number);
    if (!request) {
        return;
    }
    const auto found = members_.find(request->fingerprint);
    if (found == members_.end()) {
        store_.removeRequest(number);
        return;
    }
    Member member = found->second;
    const Copy verdict = byName.empty() ? (allow ? Copy::eAllowed : Copy::eDeclinedCard)
                                        : (allow ? Copy::eAllowedBy : Copy::eDeclinedBy);
    const std::string words = text(verdict, {member.name, byName});
    for (const RequestCard& card : store_.cards(number)) {
        actions.push_back([this, card, words]() {
            link_.retell(card.admin, card.messageId, words, {});
        });
    }
    store_.removeRequest(number);
    if (allow) {
        admitLocked(member, actions);
        return;
    }
    member.state = MemberState::eLeft;
    saveLocked(member);
    tellLocked(member.fingerprint, Copy::eDeclined, {}, actions);
}

void GroupEngine::postLocked(Member& member, const client::IncomingMessage& message,
    const Prefetched& prefetched, Actions& actions)
{
    if (!writesLocked(member)) {
        tellLocked(member.fingerprint, Copy::ePostingClosed, {}, actions);
        return;
    }
    if (message.e2eId.empty() || store_.post(message.e2eId)) {
        log::warn("group: a post without a fresh id was dropped");
        return;
    }
    if (!prefetched.fits) {
        tellLocked(member.fingerprint, Copy::eTooLarge, {}, actions);
        return;
    }
    Post post;
    post.id = message.e2eId;
    post.author = member.fingerprint;
    post.type = message.contentType;
    post.text = message.text;
    post.replyTo = message.replyTo;
    post.sentAt = clock_();
    if (message.contentType != kTypeText) {
        if (!prefetched.media) {
            log::warn("group: a picture or a voice note arrived without its bytes");
            return;
        }
        post.media = *prefetched.media;
        post.mediaName = message.attachmentName;
        post.mime = message.attachmentMime;
        post.durationMs = message.attachmentDurationMs;
    }
    (void)store_.addPost(std::move(post));
}

void GroupEngine::commandLocked(Member& member, const std::string& line, Actions& actions)
{
    const auto [command, argument] = splitCommand(line);
    if (command == kRenameCommand) {
        renaming_.insert(member.fingerprint);
        tellLocked(member.fingerprint, Copy::eAskNewName, {}, actions);
        return;
    }
    if (command == kContactableCommand) {
        member.contactable = argument == "on";
        saveLocked(member);
        return;
    }
    if (command == kSettingsCommand) {
        offerSettingsLocked(member, actions);
        return;
    }
    if (command == "contact") {
        const std::optional<Post> post = store_.post(argument);
        if (!post || post->deleted || post->author == member.fingerprint
            || !config_.contactRequests) {
            return;
        }
        const auto author = members_.find(post->author);
        if (author == members_.end() || !author->second.contactable
            || author->second.state != MemberState::eMember) {
            return;
        }
        if (!store_.noteContactAsk(member.fingerprint, post->author)) {
            return;
        }
        actions.push_back([this, author = post->author, requester = member]() {
            const std::string words = copyText(config().language, Copy::eAsksYou,
                {requester.name, chatDescriptor(requester)});
            (void)link_.tell(author, words, {}, watchRefusal(author));
        });
        tellLocked(member.fingerprint, Copy::eContactSent, {}, actions);
        return;
    }
    if ((command == "exclude" || command == "block") && member.admin) {
        const std::optional<Post> post = store_.post(argument);
        if (!post || post->author == member.fingerprint) {
            return;
        }
        const auto author = members_.find(post->author);
        if (author == members_.end() || author->second.admin) {
            return;
        }
        if (command == "block") {
            blockLocked(post->author, actions);
        } else {
            excludeLocked(post->author, actions);
        }
        return;
    }
    if (command == "delete" && member.admin) {
        deletePostLocked(argument);
    }
}

void GroupEngine::renameLocked(
    Member& member, const client::IncomingMessage& message, Actions& actions)
{
    if (message.contentType != kTypeText) {
        tellLocked(member.fingerprint, Copy::eAskNewName, {}, actions);
        return;
    }
    const std::optional<NameCheck> check = freeNameLocked(member.fingerprint, message.text, actions);
    if (!check) {
        return;
    }
    renaming_.erase(member.fingerprint);
    member.name = check->name;
    member.nameKey = check->key;
    saveLocked(member);
    tellLocked(member.fingerprint, Copy::eRenamed, {member.name}, actions);
}

std::optional<NameCheck> GroupEngine::freeNameLocked(
    const std::string& peer, const std::string& wanted, Actions& actions)
{
    const NameCheck check = checkName(wanted);
    if (!check.allowed) {
        tellLocked(peer, Copy::eNameRefused, {}, actions);
        return std::nullopt;
    }
    const bool taken = std::any_of(members_.begin(), members_.end(), [&](const auto& entry) {
        return entry.first != peer && entry.second.nameKey == check.key;
    });
    if (taken) {
        tellLocked(peer, Copy::eNameTaken, {}, actions);
        return std::nullopt;
    }
    return check;
}

void GroupEngine::excludeLocked(const std::string& fingerprint, Actions& actions)
{
    const auto found = members_.find(fingerprint);
    if (found == members_.end() || found->second.state == MemberState::eLeft
        || found->second.state == MemberState::eBanned) {
        return;
    }
    Member member = found->second;
    member.state = MemberState::eLeft;
    member.admin = false;
    renaming_.erase(fingerprint);
    saveLocked(member);
    if (const std::optional<JoinRequest> request = store_.requestOf(fingerprint)) {
        store_.removeRequest(request->number);
    }
    tellLocked(fingerprint, Copy::eExcluded, {}, actions);
}

void GroupEngine::blockLocked(const std::string& fingerprint, Actions& actions)
{
    const auto found = members_.find(fingerprint);
    if (found == members_.end() || found->second.state == MemberState::eBanned) {
        return;
    }
    Member member = found->second;
    member.state = MemberState::eBanned;
    member.admin = false;
    renaming_.erase(fingerprint);
    saveLocked(member);
    if (const std::optional<JoinRequest> request = store_.requestOf(fingerprint)) {
        store_.removeRequest(request->number);
    }
    tellLocked(fingerprint, Copy::eBlocked, {}, actions);
    actions.push_back([this, fingerprint]() { link_.forget(fingerprint); });
}

void GroupEngine::deletePostLocked(const std::string& id)
{
    std::optional<Post> post = store_.post(id);
    if (!post || post->deleted) {
        return;
    }
    post->deleted = true;
    store_.updatePost(*post);
    (void)store_.appendLog(LogKind::eDelete, post->id);
}

std::string GroupEngine::chatDescriptor(const Member& member) const
{
    Descriptor descriptor = parseDescriptor(link_.descriptor(member.fingerprint));
    descriptor.name = member.name;
    return encodeDescriptor(descriptor);
}

void GroupEngine::pumpOnce()
{
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const std::int64_t now = clock_();
        std::size_t busy = static_cast<std::size_t>(std::count_if(
            inFlight_.begin(), inFlight_.end(), [](const auto& entry) { return entry.second; }));
        for (auto& [fingerprint, member] : members_) {
            if (busy >= kMaxRelaysInFlight) {
                break;
            }
            if (member.state != MemberState::eMember || inFlight_[fingerprint]) {
                continue;
            }
            const auto retry = retryAt_.find(fingerprint);
            if (retry != retryAt_.end() && retry->second > now) {
                continue;
            }
            Member copy = member;
            std::optional<std::function<void()>> next = nextRelayLocked(copy);
            if (copy.cursor != member.cursor) {
                saveLocked(copy);
            }
            if (next) {
                inFlight_[fingerprint] = true;
                ++busy;
                actions.push_back(std::move(*next));
            }
        }
    }
    run(actions);
}

std::optional<std::function<void()>> GroupEngine::nextRelayLocked(Member& member)
{
    const std::string peer = member.fingerprint;
    const auto done = [this, peer](const std::int64_t seq) {
        return [this, peer, seq](const client::OutboundCourier::Outcome& outcome) {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                deliveredLocked(peer, seq, outcome);
            }
            nudge();
        };
    };
    const auto guarded = [this, peer](std::function<void()> send) {
        return [this, peer, send = std::move(send)]() {
            try {
                send();
            } catch (const std::exception& error) {
                log::warn("group: a relay did not leave, it is tried again: {}", error.what());
                const std::lock_guard<std::mutex> lock(mutex_);
                inFlight_[peer] = false;
                retryAt_[peer] = clock_() + kRelayRetryMs;
            }
        };
    };

    const std::vector<LogEntry> entries = store_.logAfter(member.cursor, kLogBatch);
    nlohmann::json reactions = nlohmann::json::array();
    std::int64_t reactionsSeq = 0;
    for (const LogEntry& entry : entries) {
        const std::optional<Post> post = store_.post(entry.postId);
        const bool behind = !post || post->seq <= member.historyFrom;
        if (entry.kind == LogKind::eReactions) {
            if (!behind && !post->deleted) {
                const std::map<std::string, std::int64_t> counts
                    = store_.reactionCounts(post->id, peer);
                reactions.push_back({{"ref", post->id}, {"counts", counts}});
            }
            reactionsSeq = entry.seq;
            continue;
        }
        if (!reactions.empty()) {
            break;
        }
        if (behind || post->author == peer) {
            member.cursor = entry.seq;
            continue;
        }
        if (entry.kind == LogKind::ePost) {
            if (post->deleted) {
                member.cursor = entry.seq;
                continue;
            }
            const auto author = members_.find(post->author);
            client::RelayedPost relayed;
            relayed.type = post->type;
            relayed.id = post->id;
            relayed.sentAt = post->sentAt;
            relayed.text = post->text;
            relayed.replyTo = post->replyTo;
            relayed.media = post->media;
            relayed.mediaName = post->mediaName;
            relayed.mime = post->mime;
            relayed.durationMs = post->durationMs;
            relayed.author = author == members_.end() ? std::string() : author->second.name;
            relayed.contactable = config_.contactRequests && author != members_.end()
                && author->second.contactable;
            return guarded([this, peer, relayed, outcome = done(entry.seq)]() {
                link_.relay(peer, relayed, outcome);
            });
        }
        if (entry.kind == LogKind::eEdit) {
            if (post->deleted) {
                member.cursor = entry.seq;
                continue;
            }
            return guarded([this, peer, id = post->id, words = post->text,
                               outcome = done(entry.seq)]() {
                link_.edit(peer, id, words, outcome);
            });
        }
        return guarded([this, peer, id = post->id, outcome = done(entry.seq)]() {
            link_.remove(peer, id, outcome);
        });
    }
    if (!reactions.empty()) {
        return guarded([this, peer, reactions, outcome = done(reactionsSeq)]() {
            link_.reactions(peer, reactions, outcome);
        });
    }
    if (reactionsSeq > member.cursor) {
        member.cursor = reactionsSeq;
    }
    return std::nullopt;
}

void GroupEngine::deliveredLocked(const std::string& fingerprint, const std::int64_t seq,
    const client::OutboundCourier::Outcome& outcome)
{
    inFlight_[fingerprint] = false;
    const auto found = members_.find(fingerprint);
    if (found == members_.end()) {
        return;
    }
    Member member = found->second;
    if (outcome.stored) {
        retryAt_.erase(fingerprint);
        member.cursor = std::max(member.cursor, seq);
        saveLocked(member);
        return;
    }
    if (outcome.errorCode == kDeliveryRejected) {
        refusedLocked(fingerprint);
        return;
    }
    retryAt_[fingerprint] = clock_() + kRelayRetryMs;
}

GroupLink::Outcome GroupEngine::watchRefusal(const std::string& peer)
{
    return [this, peer](const client::OutboundCourier::Outcome& outcome) {
        if (outcome.errorCode != kDeliveryRejected) {
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        refusedLocked(peer);
    };
}

void GroupEngine::refusedLocked(const std::string& fingerprint)
{
    const auto found = members_.find(fingerprint);
    if (found == members_.end() || found->second.state == MemberState::eLeft
        || found->second.state == MemberState::eBanned) {
        return;
    }
    Member member = found->second;
    member.state = MemberState::eLeft;
    renaming_.erase(fingerprint);
    saveLocked(member);
}

void GroupEngine::flushReactions()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const std::string& id : dirtyReactions_) {
            (void)store_.appendLog(LogKind::eReactions, id);
        }
        dirtyReactions_.clear();
        flushedAt_ = clock_();
        std::int64_t oldest = store_.seqBeforeLastPosts(config_.historySize);
        for (const auto& [fingerprint, member] : members_) {
            if (member.state == MemberState::eMember) {
                oldest = std::min(oldest, member.cursor);
            }
        }
        store_.dropMediaBefore(oldest);
    }
    nudge();
}

void GroupEngine::refreshMemberCount()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    memberCount_ = static_cast<std::int64_t>(std::count_if(members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.state == MemberState::eMember; }));
    countedAt_ = clock_();
}

std::int64_t GroupEngine::memberCount() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return memberCount_;
}

GroupConfig GroupEngine::config() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void GroupEngine::setConfig(const GroupConfig& config)
{
    if (config.historySize < 0) {
        throw std::runtime_error("the history size cannot be negative");
    }
    if (config.greeting.size() > kMaxGroupTextBytes || config.welcome.size() > kMaxGroupTextBytes) {
        throw std::runtime_error(
            "a group text is at most " + std::to_string(kMaxGroupTextBytes) + " bytes");
    }
    (void)copyText(config.language, Copy::eAskName);
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const bool kindChanged = config.kind != config_.kind;
        store_.setConfig(config);
        config_ = config;
        if (kindChanged) {
            for (const auto& [fingerprint, member] : members_) {
                if (member.state == MemberState::eMember && !member.admin) {
                    actions.push_back(
                        [this, peer = fingerprint]() { link_.state(peer, watchRefusal(peer)); });
                }
            }
        }
    }
    run(actions);
}

std::vector<Member> GroupEngine::members() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Member> members;
    for (const auto& [fingerprint, member] : members_) {
        members.push_back(member);
    }
    return members;
}

void GroupEngine::setAdmin(const std::string& fingerprint, const bool admin)
{
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = members_.find(fingerprint);
        if (found == members_.end() || found->second.state != MemberState::eMember) {
            throw std::runtime_error("only a member can be made an admin");
        }
        Member member = found->second;
        if (member.admin == admin) {
            return;
        }
        member.admin = admin;
        saveLocked(member);
        tellLocked(fingerprint, admin ? Copy::eAdminGranted : Copy::eAdminRevoked, {}, actions);
    }
    run(actions);
}

void GroupEngine::exclude(const std::string& fingerprint)
{
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        excludeLocked(fingerprint, actions);
    }
    run(actions);
}

void GroupEngine::block(const std::string& fingerprint)
{
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        blockLocked(fingerprint, actions);
    }
    run(actions);
}

void GroupEngine::unblock(const std::string& fingerprint)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = members_.find(fingerprint);
    if (found == members_.end() || found->second.state != MemberState::eBanned) {
        throw std::runtime_error("that member is not blocked");
    }
    Member member = found->second;
    member.state = MemberState::eLeft;
    saveLocked(member);
}

std::vector<JoinRequest> GroupEngine::requests() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return store_.requests();
}

void GroupEngine::decide(const std::int64_t number, const bool allow)
{
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        decideLocked(number, allow, {}, actions);
    }
    run(actions);
    nudge();
}

std::vector<Post> GroupEngine::posts(const std::int64_t limit) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return store_.recentPosts(limit);
}

std::map<std::string, std::int64_t> GroupEngine::reactionCounts(const std::string& postId) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return store_.reactionCounts(postId, {});
}

void GroupEngine::deletePost(const std::string& id)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        deletePostLocked(id);
    }
    nudge();
}

}  // namespace bazarish::bot
