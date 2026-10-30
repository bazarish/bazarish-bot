// Bazarish project (c) 2026
#include "Transfers.hpp"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#define CHECK(condition)                                                            \
    do {                                                                            \
        if (!(condition)) {                                                         \
            std::fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, \
                #condition);                                                        \
            std::exit(1);                                                           \
        }                                                                           \
    } while (false)

using namespace bazarish::bot;
using bazarish::client::TransferEvent;
using bazarish::client::TransferStage;
using bazarish::client::TransferState;

namespace {

constexpr const char* kPeer = "peer-fingerprint";
constexpr const char* kFile = "0123456789abcdef";
constexpr std::uint64_t kSize = 3000;
constexpr std::uint64_t kHalf = 1500;

TransferEvent event(const TransferState state, const TransferStage stage,
    const std::uint64_t bytes = 0, const std::uint64_t total = 0, const std::string& error = {})
{
    return TransferEvent{kFile, kPeer, state, bytes, total, error, stage};
}

}  // namespace

int main()
{
    Transfers transfers;
    transfers.expect(kPeer, kFile, "/srv/in/report.pdf");
    CHECK(transfers.active(kPeer, kFile));

    const std::optional<Transfer> asked
        = transfers.note(event(TransferState::eRequested, TransferStage::eAsking));
    CHECK(asked.has_value());
    CHECK(asked->state == "requested");
    CHECK(!asked->outgoing);
    CHECK(asked->path == "/srv/in/report.pdf");
    CHECK(!transfers.note(event(TransferState::eRequested, TransferStage::eConnecting)));
    CHECK(transfers.find(kPeer, kFile)->stage == "connecting");

    CHECK(transfers.note(
        event(TransferState::eRunning, TransferStage::eReceiving, kHalf, kSize)));
    CHECK(!transfers.note(
        event(TransferState::eRunning, TransferStage::eReceiving, kSize, kSize)));
    CHECK(transfers.find(kPeer, kFile)->bytes == kSize);

    const std::optional<Transfer> done
        = transfers.note(event(TransferState::eDone, TransferStage::eNone, kSize, kSize));
    CHECK(done.has_value());
    CHECK(done->state == "done");
    CHECK(done->stage.empty());
    CHECK(done->bytes == kSize);
    CHECK(!transfers.active(kPeer, kFile));

    const std::string other = "fedcba9876543210";
    const std::optional<Transfer> served = transfers.note(TransferEvent{other, kPeer,
        TransferState::eRequested, 0, 0, {}, TransferStage::eEncrypting});
    CHECK(served.has_value());
    CHECK(served->outgoing);
    CHECK(transfers.note(TransferEvent{other, kPeer, TransferState::eRunning, kHalf, kSize, {},
        TransferStage::eSending}));
    const std::optional<Transfer> failed = transfers.note(TransferEvent{other, kPeer,
        TransferState::eFailed, 0, 0, "your contact never connected", TransferStage::eNone});
    CHECK(failed->state == "failed");
    CHECK(failed->error == "your contact never connected");
    CHECK(failed->stage == "sending");
    CHECK(failed->bytes == kHalf);
    CHECK(transfers.all().size() == 2);
    CHECK(!transfers.find("someone-else", other).has_value());

    std::printf("TestTransfers OK\n");
    return 0;
}
