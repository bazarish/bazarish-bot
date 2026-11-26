// Bazarish project (c) 2026
#include "GroupEngine.hpp"


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

constexpr std::int64_t kLogBatch = 64;
constexpr std::size_t kMaxUtf8Bytes = 4;
constexpr const char* kDeliveryRejected = "DELIVERY_REJECTED";
constexpr const char* kStorageFull = "STORAGE_FULL";
constexpr const char* kApprove = "approve:";
constexpr const char* kDecline = "decline:";
constexpr const char* kSettingsCommand = "settings";
constexpr const char* kContactableCommand = "contactable";
constexpr const char* kRenameCommand = "rename";
constexpr const char* kTypeText = "text";
constexpr const char* kTypeImage = "image";
constexpr const char* kTypeAudio = "audio";
constexpr const char* kTypeFile = "file";
constexpr const char* kTypeMembersAsk = "group.members.ask";

bool isPost(const std::string& type)
{
    return type == kTypeText || type == kTypeImage || type == kTypeAudio || type == kTypeFile;
}

std::pair<std::string, std::string> splitCommand(const std::string& line)
{
    const std::size_t space = line.find(' ');
    if (space == std::string::npos) {
        return {line, {}};
    }
    return {line.substr(0, space), line.substr(space + 1)};
}

}  // namespace

GroupEngine::GroupEngine(const std::filesystem::path& file, GroupLink link, Clock clock)
    : store_(file)
    , link_(std::move(link))
    , clock_(std::move(clock))
    , config_(store_.config())
{
    for (const Member& member : store_.members()) {
        members_[member.fingerprint] = member;
    }
    const std::int64_t now = clock_();
    countedAt_ = now;
    sweptAt_ = now;
    sweepStale_ = true;
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
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        nudged_ = false;
        const std::int64_t now = clock_();
        const bool countDue = countStale_ && now - countedAt_ >= kMemberCountPeriodMs;
        const bool sweepDue = sweepStale_ && now - sweptAt_ >= kSweepMs;
        lock.unlock();
        if (countDue) {
            refreshMemberCount();
        }
        if (sweepDue) {
            dropSpent();
        }
        pumpOnce();
        lock.lock();
        const auto woken = [this]() { return stopping_ || nudged_; };
        if (const std::optional<std::int64_t> waitMs = untilDueLocked(); waitMs.has_value()) {
            wake_.wait_for(lock, std::chrono::milliseconds(*waitMs), woken);
        } else {
            wake_.wait(lock, woken);
        }
    }
}

std::optional<std::int64_t> GroupEngine::untilDueLocked() const
{
    std::optional<std::int64_t> dueAt;
    const auto sooner = [&dueAt](const std::int64_t at) {
        if (!dueAt.has_value() || at < *dueAt) {
            dueAt = at;
        }
    };
    if (countStale_) {
        sooner(countedAt_ + kMemberCountPeriodMs);
    }
    if (sweepStale_) {
        sooner(sweptAt_ + kSweepMs);
    }
    if (!dueAt.has_value()) {
        return std::nullopt;
    }
    return std::max<std::int64_t>(0, *dueAt - clock_());
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
    const auto found = members_.find(member.fingerprint);
    const Member before = found == members_.end() ? Member{} : found->second;
    members_[member.fingerprint] = member;
    store_.putMember(member);
    noteRosterLocked(before, member);
    markStaleLocked(true);
}

bool GroupEngine::contactableLocked(const std::string& fingerprint) const
{
    const auto found = members_.find(fingerprint);
    return config_.contactRequests && found != members_.end() && found->second.contactable;
}

std::int64_t GroupEngine::memberTotalLocked() const
{
    return static_cast<std::int64_t>(std::count_if(members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.state == MemberState::eMember; }));
}

void GroupEngine::noteRosterLocked(const Member& before, const Member& after)
{
    if (config_.kind != client::GroupKind::eGroup) {
        return;
    }
    const bool was = before.state == MemberState::eMember;
    const bool is = after.state == MemberState::eMember;
    const std::int64_t total = memberTotalLocked();
    if (was && is && before.name == after.name) {
        if (before.contactable != after.contactable) {
            (void)store_.appendRoster({RosterKind::eChanged, after.name, after.fingerprint, total});
        }
        return;
    }
    if (was) {
        (void)store_.appendRoster(
            {RosterKind::eRemoved, before.name, before.fingerprint, is ? total - 1 : total});
    }
    if (is) {
        (void)store_.appendRoster({RosterKind::eAdded, after.name, after.fingerprint, total});
    }
}

void GroupEngine::queueListLocked(const std::string& peer)
{
    std::vector<client::RelayedPost> letters;
    const auto letter = [](client::MemberListLetter members) {
        client::RelayedPost post;
        post.kind = client::RelayKind::eMembers;
        post.id = client::newMessageId();
        post.members = std::move(members);
        return post;
    };
    if (config_.kind != client::GroupKind::eGroup) {
        letters.push_back(letter({}));
    } else {
        struct Listed {
            std::string name;
            bool you = false;
            bool contactable = false;
        };
        std::vector<Listed> in;
        for (const auto& [fingerprint, member] : members_) {
            if (member.state == MemberState::eMember) {
                in.push_back({member.name, fingerprint == peer, contactableLocked(fingerprint)});
            }
        }
        const std::size_t perPart = client::kMaxMemberNamesPerLetter;
        const std::size_t partCount = std::max<std::size_t>(1, (in.size() + perPart - 1) / perPart);
        const auto parts = static_cast<std::int64_t>(partCount);
        for (std::int64_t part = 0; part < parts; ++part) {
            client::MemberListLetter full;
            full.op = client::MemberListOp::eFull;
            full.part = part + 1;
            full.parts = parts;
            full.total = static_cast<std::int64_t>(in.size());
            const std::size_t from = static_cast<std::size_t>(part) * perPart;
            for (std::size_t at = from; at < in.size() && at < from + perPart; ++at) {
                full.names.push_back(in[at].name);
                if (in[at].contactable) {
                    full.contactable.push_back(in[at].name);
                }
                if (in[at].you) {
                    full.you = in[at].name;
                }
            }
            letters.push_back(letter(std::move(full)));
        }
    }
    listOut_[peer] = std::move(letters);
    listThrough_[peer] = store_.lastSeq();
    markStaleLocked(false);
}

void GroupEngine::markStaleLocked(const bool members)
{
    countStale_ = countStale_ || members;
    sweepStale_ = true;
    nudged_ = true;
    wake_.notify_all();
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
    // A group shows its member list, so its count hides no join; a channel's count stays hourly.
    block.members = config_.kind == client::GroupKind::eGroup ? memberTotalLocked() : memberCount_;
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

void GroupEngine::nameAndJoinLocked(Member& member, const std::string& offered, Actions& actions)
{
    const std::string& peer = member.fingerprint;
    member.state = MemberState::eNaming;
    if (offered.empty()) {
        saveLocked(member);
        tellLocked(peer, Copy::eAskName, {}, actions);
        return;
    }
    const NameCheck check = checkName(offered);
    if (!check.allowed) {
        saveLocked(member);
        tellLocked(peer, Copy::eOfferedNameRefused, {offered}, actions);
        return;
    }
    const bool taken = std::any_of(members_.begin(), members_.end(), [&](const auto& entry) {
        return entry.first != peer && entry.second.nameKey == check.key;
    });
    if (taken) {
        saveLocked(member);
        tellLocked(peer, Copy::eOfferedNameTaken, {check.name}, actions);
        return;
    }
    member.name = check.name;
    member.nameKey = check.key;
    joinLocked(member, actions);
}

void GroupEngine::offerSettingsLocked(const Member& member, Actions& actions)
{
    tellLocked(member.fingerprint, Copy::eSettings, {member.name}, actions,
        settingsKeyboardLocked(member));
}

void GroupEngine::showSettingsLocked(
    const Member& member, const std::string& ref, Actions& actions)
{
    if (ref.empty()) {
        return;
    }
    const std::string words = text(Copy::eSettings, {member.name});
    actions.push_back([this, peer = member.fingerprint, ref, words,
                          keyboard = settingsKeyboardLocked(member)]() {
        link_.retell(peer, ref, words, keyboard);
    });
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
    if (!message.sentByUs && message.contentType == "contact.request") {
        prefetched.offeredName = link_.name(message.fromFingerprint);
    }
    if (!message.sentByUs && isPost(message.contentType)) {
        if (message.contentType != kTypeText) {
            prefetched.media = link_.media(message.e2eId, message.contentType);
        }
        client::RelayedPost sizing;
        sizing.type = message.contentType;
        sizing.id = message.e2eId;
        sizing.text = message.text;
        sizing.replyTo = message.replyTo;
        sizing.after = std::string(kMaxMessageIdBytes, 'x');
        sizing.media = prefetched.media.value_or(Bytes());
        sizing.mediaName = message.attachmentName;
        sizing.mime = message.attachmentMime;
        sizing.durationMs = message.attachmentDurationMs;
        sizing.author = std::string(kMaxNameCodepoints * kMaxUtf8Bytes, 'x');
        sizing.sentAt = std::numeric_limits<std::int64_t>::max();
        prefetched.fits = (message.contentType == kTypeText || prefetched.media.has_value())
            && link_.fitting({sizing}) == 1;
    }
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!message.sentByUs) {
            held_.erase(message.fromFingerprint);
        }
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
        const auto asked = joinRequests_.find(peer);
        const bool again = found != members_.end() && found->second.state != MemberState::eLeft
            && (asked == joinRequests_.end() || asked->second == message.e2eId);
        joinRequests_[peer] = message.e2eId;
        if (again) {
            return;
        }
        store_.setIntro(peer, message.text);
        if (found == members_.end() || found->second.name.empty()) {
            Member member = found == members_.end() ? Member{} : found->second;
            member.fingerprint = peer;
            nameAndJoinLocked(member, prefetched.offeredName, actions);
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
            tellLocked(peer, Copy::eAskName, {}, actions);
        } else {
            joinLocked(member, actions);
        }
        return;
    case MemberState::eNaming: {
        if (!isPost(message.contentType)) {
            return;
        }
        if (message.contentType != kTypeText) {
            tellLocked(peer, Copy::eAskName, {}, actions);
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

    if (message.contentType == kTypeMembersAsk) {
        queueListLocked(peer);
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
            (void)store_.appendLog(LogKind::eReactions, post->id);
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
        const bool approve = data.starts_with(kApprove);
        if ((approve || data.starts_with(kDecline)) && member.admin) {
            const std::string number
                = data.substr(std::string(approve ? kApprove : kDecline).size());
            decideLocked(std::stoll(number), approve, member.name, actions);
            return;
        }
        commandLocked(member, data, actions);
        if (data.starts_with(kContactableCommand)) {
            showSettingsLocked(member, message.refId, actions);
        }
    }
}

void GroupEngine::joinLocked(Member& member, Actions& actions)
{
    if (config_.joinMode == JoinMode::eOpen || member.admin) {
        admitLocked(member);
        return;
    }
    member.state = MemberState::ePending;
    saveLocked(member);
    const JoinRequest request = store_.addRequest(member.fingerprint, clock_());
    tellLocked(member.fingerprint, Copy::eAwaitingApproval, {}, actions);
    const client::InlineKeyboard keyboard{{
        {text(Copy::eButtonAllow), kApprove + std::to_string(request.number), {}},
        {text(Copy::eButtonDecline), kDecline + std::to_string(request.number), {}},
    }};
    for (const auto& [fingerprint, admin] : members_) {
        if (!admin.admin || admin.state != MemberState::eMember) {
            continue;
        }
        actions.push_back([this, admin = fingerprint, applicant = member, request, keyboard]() {
            std::string card = copyText(config().language, Copy::eWantsToJoin,
                {applicant.name, chatDescriptor(applicant)});
            if (!request.intro.empty()) {
                card += "\n\n" + request.intro;
            }
            const std::string id = link_.tell(admin, card, keyboard, watchRefusal(admin));
            const std::lock_guard<std::mutex> lock(mutex_);
            store_.addCard(request.number, admin, id);
        });
    }
}

void GroupEngine::admitLocked(Member& member)
{
    member.state = MemberState::eMember;
    store_.forgetIntro(member.fingerprint);
    if (member.joinedAt == 0) {
        member.joinedAt = clock_();
    }
    member.cursor = store_.seqBeforeLastPosts(config_.historySize);
    member.historyFrom = member.cursor;
    saveLocked(member);
    queueListLocked(member.fingerprint);
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
        admitLocked(member);
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
    markStaleLocked(false);
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
        const std::string key = checkName(argument).key;
        const auto author = std::find_if(members_.begin(), members_.end(), [&key](const auto& entry) {
            return entry.second.state == MemberState::eMember && entry.second.nameKey == key;
        });
        if (author == members_.end() || author->first == member.fingerprint
            || !config_.contactRequests) {
            return;
        }
        if (!author->second.contactable) {
            tellLocked(member.fingerprint, Copy::eContactRefused, {author->second.name}, actions);
            return;
        }
        const std::int64_t now = clock_();
        const std::optional<std::int64_t> asked
            = store_.contactAskAt(member.fingerprint, author->first);
        if (asked.has_value() && now - *asked < kContactAskEveryMs) {
            return;
        }
        store_.noteContactAsk(member.fingerprint, author->first, now);
        actions.push_back([this, author = author->first, requester = member]() {
            const std::string words = copyText(config().language, Copy::eAsksYou,
                {requester.name, chatDescriptor(requester)});
            (void)link_.tell(author, words, {}, watchRefusal(author));
        });
        tellLocked(member.fingerprint, Copy::eContactSent, {author->second.name}, actions);
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
    const std::optional<NameCheck> check
        = freeNameLocked(member.fingerprint, message.text, actions);
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
    store_.dropRequestOf(fingerprint);
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
    store_.dropRequestOf(fingerprint);
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
        std::size_t busy = static_cast<std::size_t>(
            std::count_if(inFlight_.begin(), inFlight_.end(), [this](const auto& entry) {
                return entry.second && waiting_.count(entry.first) == 0;
            }));
        for (auto& [fingerprint, member] : members_) {
            if (busy >= kMaxRelaysInFlight) {
                break;
            }
            if (member.state != MemberState::eMember || inFlight_[fingerprint]) {
                continue;
            }
            if (held_.count(fingerprint) != 0) {
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

client::RelayedPost GroupEngine::relayedLocked(const Post& post) const
{
    const auto author = members_.find(post.author);
    client::RelayedPost relayed;
    relayed.type = post.type;
    relayed.id = post.id;
    relayed.sentAt = post.sentAt;
    relayed.text = post.text;
    relayed.replyTo = post.replyTo;
    relayed.after = store_.postBefore(post.seq);
    relayed.media = post.media;
    relayed.mediaName = post.mediaName;
    relayed.mime = post.mime;
    relayed.durationMs = post.durationMs;
    relayed.fromHost = post.author.empty();
    relayed.author = author == members_.end() ? std::string() : author->second.name;
    relayed.contactable = config_.contactRequests && author != members_.end()
        && author->second.contactable;
    return relayed;
}

std::optional<std::function<void()>> GroupEngine::nextRelayLocked(Member& member)
{
    const std::string peer = member.fingerprint;
    const auto done = [this, peer](const std::int64_t seq) {
        client::DeliveryWatch watch;
        watch.onPhase = [this, peer](const std::string& phase) {
            if (phase != client::kPhaseWaiting) {
                return;
            }
            bool gone = false;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                gone = waitingLocked(peer);
            }
            if (gone) {
                link_.withdraw(peer);
            }
            nudge();
        };
        watch.onOutcome = [this, peer, seq](const client::OutboundCourier::Outcome& outcome) {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                deliveredLocked(peer, seq, outcome);
            }
            nudge();
        };
        return watch;
    };
    const auto guarded = [this, peer](std::function<void()> send) {
        return [this, peer, send = std::move(send)]() {
            try {
                send();
            } catch (const std::exception& error) {
                log::error("group: a relay to {} did not leave; it waits until they are heard"
                           " from: {}",
                    log::redact(peer), error.what());
                const std::lock_guard<std::mutex> lock(mutex_);
                inFlight_[peer] = false;
                held_.insert(peer);
            }
        };
    };

    const std::int64_t sentThrough = member.cursor;
    std::int64_t scanned = member.cursor;
    std::vector<client::RelayedPost> items;
    std::vector<std::int64_t> seqs;
    std::set<std::string> carried;
    std::set<std::string> counted;
    const auto add = [&items, &seqs](client::RelayedPost item, const std::int64_t seq) {
        items.push_back(std::move(item));
        seqs.push_back(seq);
    };
    const auto about = [](const client::RelayKind kind, const std::string& ref) {
        client::RelayedPost item;
        item.kind = kind;
        item.id = client::newMessageId();
        item.ref = ref;
        return item;
    };
    const std::int64_t listThrough = listThrough_[peer];
    const auto rosterEntry = [&](const LogEntry& entry) {
        const std::optional<RosterChange> change = store_.roster(entry.seq);
        if (!change || entry.seq <= listThrough || entry.seq <= member.historyFrom) {
            return;
        }
        client::RelayedPost letter = about(client::RelayKind::eMembers, {});
        letter.members.op = change->kind == RosterKind::eAdded ? client::MemberListOp::eAdded
            : change->kind == RosterKind::eChanged             ? client::MemberListOp::eChanged
                                                               : client::MemberListOp::eRemoved;
        letter.members.names = {change->name};
        letter.members.total = change->total;
        if (change->kind != RosterKind::eRemoved && contactableLocked(change->fingerprint)) {
            letter.members.contactable = {change->name};
        }
        if (change->kind == RosterKind::eAdded && change->fingerprint == peer) {
            letter.members.you = change->name;
        }
        add(std::move(letter), entry.seq);
    };
    const std::vector<client::RelayedPost>& waitingList = listOut_[peer];
    const std::size_t listed = waitingList.size();
    for (const client::RelayedPost& letter : waitingList) {
        add(letter, sentThrough);
    }
    const auto relayEntry = [&](const LogEntry& entry) {
        if (entry.kind == LogKind::eRoster) {
            rosterEntry(entry);
            return;
        }
        const std::optional<Post> post = store_.post(entry.postId);
        if (!post || post->seq <= member.historyFrom) {
            return;
        }
        const bool own = post->author == peer;
        switch (entry.kind) {
        case LogKind::ePost:
            if (!own && !post->deleted) {
                add(relayedLocked(*post), entry.seq);
                carried.insert(post->id);
            }
            break;
        case LogKind::eEdit:
            if (!own && !post->deleted && carried.count(post->id) == 0) {
                client::RelayedPost edit = about(client::RelayKind::eEdit, post->id);
                edit.text = post->text;
                add(std::move(edit), entry.seq);
            }
            break;
        case LogKind::eDelete:
            if (!own && post->seq <= sentThrough) {
                add(about(client::RelayKind::eDelete, post->id), entry.seq);
            }
            break;
        case LogKind::eRoster:
            break;
        case LogKind::eReactions:
            if (!post->deleted && counted.insert(post->id).second) {
                client::RelayedPost counts = about(client::RelayKind::eReactions, post->id);
                counts.reactions = nlohmann::json::array({{{"ref", post->id},
                    {"counts", store_.reactionCounts(post->id, peer)}}});
                add(std::move(counts), entry.seq);
            }
            break;
        }
    };
    for (std::vector<LogEntry> entries = store_.logAfter(scanned, kLogBatch); !entries.empty();
         entries = store_.logAfter(scanned, kLogBatch)) {
        for (const LogEntry& entry : entries) {
            scanned = entry.seq;
            const std::size_t before = items.size();
            relayEntry(entry);
            if (items.size() == before && !seqs.empty()) {
                seqs.back() = entry.seq;
            }
        }
        if (!items.empty() && link_.fitting(items) < items.size()) {
            break;
        }
    }
    if (items.empty()) {
        member.cursor = scanned;
        return std::nullopt;
    }
    const std::size_t fit = link_.fitting(items);
    if (fit == 0) {
        throw std::runtime_error("a group log entry does not fit an envelope by itself");
    }
    items.resize(fit);
    std::vector<client::RelayedPost>& sentList = listOut_[peer];
    sentList.erase(
        sentList.begin(), sentList.begin() + static_cast<std::ptrdiff_t>(std::min(fit, listed)));
    return guarded([this, peer, items, watch = done(seqs[fit - 1])]() {
        link_.relay(peer, items, watch);
    });
}

void GroupEngine::deliveredLocked(const std::string& fingerprint, const std::int64_t seq,
    const client::OutboundCourier::Outcome& outcome)
{
    inFlight_[fingerprint] = false;
    waiting_.erase(fingerprint);
    const auto found = members_.find(fingerprint);
    if (found == members_.end()) {
        return;
    }
    Member member = found->second;
    if (outcome.stored) {
        store_.clearOffline(fingerprint);
        member.cursor = std::max(member.cursor, seq);
        saveLocked(member);
        return;
    }
    if (outcome.errorCode == kDeliveryRejected) {
        store_.clearOffline(fingerprint);
        refusedLocked(fingerprint);
        return;
    }
    if (member.state != MemberState::eMember) {
        return;
    }
    if (outcome.errorCode == kStorageFull) {
        held_.insert(fingerprint);
        log::info("group: {}'s mailbox is full; relays wait until they are heard from",
            log::redact(fingerprint));
        return;
    }
    log::warn("group: {}'s server refused a relay for good, those rows are passed: {}",
        log::redact(fingerprint),
        outcome.errorMessage.empty() ? outcome.errorCode : outcome.errorMessage);
    member.cursor = std::max(member.cursor, seq);
    saveLocked(member);
}

bool GroupEngine::waitingLocked(const std::string& fingerprint)
{
    const auto found = members_.find(fingerprint);
    if (found == members_.end() || found->second.state != MemberState::eMember) {
        return false;
    }
    waiting_.insert(fingerprint);
    const std::int64_t now = clock_();
    const std::optional<std::int64_t> since = store_.offlineSince(fingerprint);
    if (!since) {
        store_.markOffline(fingerprint, now);
        return false;
    }
    if (now - *since < kDeadAfterMs) {
        return false;
    }
    store_.clearOffline(fingerprint);
    held_.erase(fingerprint);
    goneLocked(fingerprint);
    return true;
}

GroupLink::Outcome GroupEngine::watchRefusal(const std::string& peer)
{
    return [this, peer](const client::OutboundCourier::Outcome& outcome) {
        if (outcome.stored) {
            return;
        }
        if (outcome.errorCode != kDeliveryRejected) {
            log::warn("group: a message to {} did not arrive: {}", log::redact(peer),
                outcome.errorMessage.empty() ? outcome.errorCode : outcome.errorMessage);
            return;
        }
        const std::lock_guard<std::mutex> lock(mutex_);
        refusedLocked(peer);
    };
}

void GroupEngine::goneLocked(const std::string& fingerprint)
{
    Member member = members_.at(fingerprint);
    member.state = MemberState::eLeft;
    member.admin = false;
    renaming_.erase(fingerprint);
    saveLocked(member);
    store_.dropRequestOf(fingerprint);
    log::info("group: {} took no delivery for {} days, so they left",
        log::redact(fingerprint), kDeadAfterDays);
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
    log::info("group: {}'s server refused delivery, so they left", log::redact(fingerprint));
}

void GroupEngine::dropSpent()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    sweptAt_ = clock_();
    sweepStale_ = false;
    std::int64_t behind = std::numeric_limits<std::int64_t>::max();
    for (const auto& [fingerprint, member] : members_) {
        if (member.state == MemberState::eMember) {
            behind = std::min(behind, member.cursor);
        }
    }
    const std::int64_t shown = std::min(store_.seqBeforeLastPosts(config_.historySize), behind);
    store_.dropMediaBefore(shown);
    if (keepsHistory_) {
        return;
    }
    const std::int64_t kept = std::min(
        store_.seqBeforeLastPosts(std::max(config_.historySize, kLeastPostsKept)), behind);
    store_.dropSpentBefore(kept);
}

void GroupEngine::refreshMemberCount()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    memberCount_ = static_cast<std::int64_t>(std::count_if(members_.begin(), members_.end(),
        [](const auto& entry) { return entry.second.state == MemberState::eMember; }));
    countedAt_ = clock_();
    countStale_ = false;
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
    (void)copyText(config.language, Copy::eAskName);
    Actions actions;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const bool kindChanged = config.kind != config_.kind;
        const bool listChanged = kindChanged || config.contactRequests != config_.contactRequests;
        store_.setConfig(config);
        config_ = config;
        markStaleLocked(false);
        if (listChanged) {
            for (const auto& [fingerprint, member] : members_) {
                if (member.state == MemberState::eMember) {
                    queueListLocked(fingerprint);
                }
            }
        }
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

void GroupEngine::setKeepsHistory(const bool keeps)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    keepsHistory_ = keeps;
    markStaleLocked(false);
}

std::vector<Member> GroupEngine::members() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Member> members;
    for (const auto& [fingerprint, member] : members_) {
        members.push_back(member);
        members.back().offlineSince = store_.offlineSince(fingerprint).value_or(0);
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

void GroupEngine::setContactable(const std::string& fingerprint, const bool contactable)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = members_.find(fingerprint);
    if (found == members_.end() || found->second.state != MemberState::eMember) {
        throw std::runtime_error("only a member refuses or accepts contact requests");
    }
    Member member = found->second;
    if (member.contactable == contactable) {
        return;
    }
    member.contactable = contactable;
    saveLocked(member);
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

void GroupEngine::hostPost(const std::string& id, const std::string& text)
{
    if (text.empty()) {
        throw std::invalid_argument("a post needs text");
    }
    client::RelayedPost sizing;
    sizing.type = kTypeText;
    sizing.id = id;
    sizing.text = text;
    sizing.after = std::string(kMaxMessageIdBytes, 'x');
    sizing.fromHost = true;
    sizing.sentAt = std::numeric_limits<std::int64_t>::max();
    if (link_.fitting({sizing}) != 1) {
        throw std::invalid_argument("this post does not fit one message");
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        Post post;
        post.id = id;
        post.type = kTypeText;
        post.text = text;
        post.sentAt = clock_();
        (void)store_.addPost(std::move(post));
        markStaleLocked(false);
    }
    nudge();
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
