// Bazarish project (c) 2026
#pragma once

#include <OutboundCourier.hpp>

#include <bazarish/Bytes.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace bazarish::bot {

// One federation exchange between two accounts of this process, carried in memory.
class LocalPipe {
public:
    std::size_t take(bool forServer, ByteBuffer out);
    void give(bool fromServer, ByteView data);
    void closeAfter(std::chrono::seconds wait);
    void close();
    bool answered() const;

private:
    Bytes& queueLocked(bool forServer);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    Bytes toServer_;
    Bytes toClient_;
    bool closed_ = false;
    bool answered_ = false;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};

// The end federationServeOnce reads a frame from and answers on.
class LocalServerEnd {
public:
    explicit LocalServerEnd(std::shared_ptr<LocalPipe> pipe);

    std::size_t readSome(ByteBuffer buffer);
    void writeAll(ByteView data);
    void closeAfter(std::chrono::seconds wait);

private:
    const std::shared_ptr<LocalPipe> pipe_;
};

// The end the courier delivers on, with the serving thread held for its lifetime.
class LocalDeliveryStream final : public client::DeliveryStream {
public:
    LocalDeliveryStream(std::shared_ptr<LocalPipe> pipe, std::jthread serving);
    ~LocalDeliveryStream() override;

    std::size_t readSome(ByteBuffer buffer) override;
    void writeAll(ByteView data) override;
    void close() override;
    bool answered() const override;

private:
    const std::shared_ptr<LocalPipe> pipe_;
    std::jthread serving_;
};

}  // namespace bazarish::bot
