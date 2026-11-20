// Bazarish project (c) 2026
#include "LocalDelivery.hpp"

#include "TestUtil.hpp"

#include <bazarish/Bytes.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

using namespace bazarish;
using bot::LocalDeliveryStream;
using bot::LocalPipe;
using bot::LocalServerEnd;

namespace {

constexpr std::chrono::seconds kShortDeadline{1};

Bytes bytesOf(const std::string& text)
{
    return Bytes(text.begin(), text.end());
}

std::string readExactly(LocalPipe& pipe, const bool forServer, const std::size_t count)
{
    Bytes got;
    while (got.size() < count) {
        Bytes chunk(count - got.size());
        const std::size_t taken = pipe.take(forServer, ByteBuffer(chunk));
        if (taken == 0) {
            break;
        }
        got.insert(got.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(taken));
    }
    return std::string(got.begin(), got.end());
}

void checkBothWays()
{
    LocalPipe pipe;
    const Bytes asked = bytesOf("deliver");
    pipe.give(false, ByteView(asked));
    CHECK(readExactly(pipe, true, asked.size()) == "deliver");
    CHECK(!pipe.answered());

    const Bytes replied = bytesOf("stored");
    pipe.give(true, ByteView(replied));
    CHECK(pipe.answered());
    CHECK(readExactly(pipe, false, replied.size()) == "stored");
}

void checkPartialReads()
{
    LocalPipe pipe;
    const Bytes asked = bytesOf("a frame in pieces");
    pipe.give(false, ByteView(asked));
    Bytes head(4);
    CHECK(pipe.take(true, ByteBuffer(head)) == 4);
    CHECK(std::string(head.begin(), head.end()) == "a fr");
    CHECK(readExactly(pipe, true, asked.size() - 4) == "ame in pieces");
}

void checkReadWaitsForTheOtherEnd()
{
    LocalPipe pipe;
    std::thread writer([&pipe]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const Bytes late = bytesOf("late");
        pipe.give(false, ByteView(late));
    });
    CHECK(readExactly(pipe, true, 4) == "late");
    writer.join();
}

void checkCloseEndsAReader()
{
    LocalPipe pipe;
    std::thread closer([&pipe]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pipe.close();
    });
    Bytes out(8);
    CHECK(pipe.take(true, ByteBuffer(out)) == 0);
    closer.join();
    const Bytes ignored = bytesOf("after");
    pipe.give(false, ByteView(ignored));
    CHECK(pipe.take(true, ByteBuffer(out)) == 0);
}

void checkDeadlineEndsAStalledReader()
{
    LocalPipe pipe;
    pipe.closeAfter(kShortDeadline);
    const std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    Bytes out(8);
    CHECK(pipe.take(true, ByteBuffer(out)) == 0);
    CHECK(std::chrono::steady_clock::now() - began >= kShortDeadline);
}

void checkTheEndsAgree()
{
    const std::shared_ptr<LocalPipe> pipe = std::make_shared<LocalPipe>();
    LocalServerEnd server(pipe);
    LocalDeliveryStream client(pipe, std::jthread{});
    const Bytes asked = bytesOf("header\n");
    client.writeAll(ByteView(asked));
    Bytes seen(asked.size());
    CHECK(server.readSome(ByteBuffer(seen)) == asked.size());
    CHECK(std::string(seen.begin(), seen.end()) == "header\n");
    CHECK(!client.answered());

    const Bytes replied = bytesOf("ok\n");
    server.writeAll(ByteView(replied));
    Bytes back(replied.size());
    CHECK(client.readSome(ByteBuffer(back)) == replied.size());
    CHECK(std::string(back.begin(), back.end()) == "ok\n");
    CHECK(client.answered());
}

}  // namespace

int main()
try {
    checkBothWays();
    checkPartialReads();
    checkReadWaitsForTheOtherEnd();
    checkCloseEndsAReader();
    checkDeadlineEndsAStalledReader();
    checkTheEndsAgree();
    std::printf("TestLocalDelivery OK\n");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "TestLocalDelivery failed: %s\n", error.what());
    return 1;
}
