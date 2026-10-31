// Bazarish project (c) 2026
#include "Transfers.hpp"

namespace bazarish::bot {

const char* transferStateName(const client::TransferState state)
{
    switch (state) {
    case client::TransferState::eRequested:
        return "requested";
    case client::TransferState::eRunning:
        return "running";
    case client::TransferState::eDone:
        return "done";
    case client::TransferState::eFailed:
        return "failed";
    }
    return "failed";
}

namespace {

const char* transferStageName(const client::TransferStage stage)
{
    switch (stage) {
    case client::TransferStage::eNone:
        return "";
    case client::TransferStage::eAsking:
        return "asking";
    case client::TransferStage::eEncrypting:
        return "encrypting";
    case client::TransferStage::eMakingAddress:
        return "making-address";
    case client::TransferStage::ePublishing:
        return "publishing";
    case client::TransferStage::eWaiting:
        return "waiting";
    case client::TransferStage::eSending:
        return "sending";
    case client::TransferStage::eConnecting:
        return "connecting";
    case client::TransferStage::eReceiving:
        return "receiving";
    }
    return "";
}

}  // namespace

void Transfers::expect(
    const std::string& peer, const std::string& messageId, const std::string& path)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    Transfer& transfer = transfers_[{peer, messageId}];
    transfer = Transfer{};
    transfer.peer = peer;
    transfer.messageId = messageId;
    transfer.outgoing = false;
    transfer.path = path;
}

std::optional<Transfer> Transfers::note(const client::TransferEvent& event)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto [found, fresh] = transfers_.try_emplace({event.peer, event.e2eId});
    Transfer& transfer = found->second;
    if (fresh) {
        transfer.peer = event.peer;
        transfer.messageId = event.e2eId;
    }
    const std::string state = transferStateName(event.state);
    const bool moved = transfer.state != state;
    transfer.state = state;
    if (event.stage != client::TransferStage::eNone) {
        transfer.stage = transferStageName(event.stage);
    }
    if (event.state == client::TransferState::eRequested && moved) {
        transfer.bytes = 0;
        transfer.total = 0;
    }
    if (event.state == client::TransferState::eRunning) {
        transfer.bytes = event.bytes;
        transfer.total = event.total;
    }
    if (event.state == client::TransferState::eDone) {
        transfer.stage.clear();
        if (event.total > 0) {
            transfer.total = event.total;
        }
        transfer.bytes = transfer.total;
    }
    transfer.error = event.error;
    if (!moved) {
        return std::nullopt;
    }
    return transfer;
}

bool Transfers::active(const std::string& peer, const std::string& messageId) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = transfers_.find({peer, messageId});
    return found != transfers_.end() && found->second.state != "done"
        && found->second.state != "failed";
}

std::optional<Transfer> Transfers::find(
    const std::string& peer, const std::string& messageId) const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = transfers_.find({peer, messageId});
    if (found == transfers_.end()) {
        return std::nullopt;
    }
    return found->second;
}

std::vector<Transfer> Transfers::all() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Transfer> listed;
    listed.reserve(transfers_.size());
    for (const auto& [key, transfer] : transfers_) {
        listed.push_back(transfer);
    }
    return listed;
}

}  // namespace bazarish::bot
