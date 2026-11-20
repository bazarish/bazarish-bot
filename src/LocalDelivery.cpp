// Bazarish project (c) 2026
#include "LocalDelivery.hpp"

#include <algorithm>
#include <utility>

namespace bazarish::bot {

Bytes& LocalPipe::queueLocked(const bool forServer)
{
    return forServer ? toServer_ : toClient_;
}

std::size_t LocalPipe::take(const bool forServer, const ByteBuffer out)
{
    std::unique_lock<std::mutex> lock(mutex_);
    const auto ready = [this, forServer]() { return closed_ || !queueLocked(forServer).empty(); };
    while (!ready()) {
        if (!deadline_.has_value()) {
            wake_.wait(lock);
            continue;
        }
        if (wake_.wait_until(lock, *deadline_) == std::cv_status::timeout && !ready()) {
            closed_ = true;
            return 0;
        }
    }
    Bytes& queue = queueLocked(forServer);
    if (queue.empty()) {
        return 0;
    }
    const std::size_t taken = std::min(out.size(), queue.size());
    std::copy(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(taken), out.begin());
    queue.erase(queue.begin(), queue.begin() + static_cast<std::ptrdiff_t>(taken));
    return taken;
}

void LocalPipe::give(const bool fromServer, const ByteView data)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        Bytes& queue = queueLocked(!fromServer);
        queue.insert(queue.end(), data.begin(), data.end());
        answered_ = answered_ || fromServer;
    }
    wake_.notify_all();
}

void LocalPipe::closeAfter(const std::chrono::seconds wait)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        deadline_ = std::chrono::steady_clock::now() + wait;
    }
    wake_.notify_all();
}

void LocalPipe::close()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
    }
    wake_.notify_all();
}

bool LocalPipe::answered() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return answered_;
}

LocalServerEnd::LocalServerEnd(std::shared_ptr<LocalPipe> pipe)
    : pipe_(std::move(pipe))
{
}

std::size_t LocalServerEnd::readSome(const ByteBuffer buffer)
{
    return pipe_->take(true, buffer);
}

void LocalServerEnd::writeAll(const ByteView data)
{
    pipe_->give(true, data);
}

void LocalServerEnd::closeAfter(const std::chrono::seconds wait)
{
    pipe_->closeAfter(wait);
}

LocalDeliveryStream::LocalDeliveryStream(std::shared_ptr<LocalPipe> pipe, std::jthread serving)
    : pipe_(std::move(pipe))
    , serving_(std::move(serving))
{
}

LocalDeliveryStream::~LocalDeliveryStream()
{
    pipe_->close();
}

std::size_t LocalDeliveryStream::readSome(const ByteBuffer buffer)
{
    return pipe_->take(false, buffer);
}

void LocalDeliveryStream::writeAll(const ByteView data)
{
    pipe_->give(false, data);
}

void LocalDeliveryStream::close()
{
    pipe_->close();
}

bool LocalDeliveryStream::answered() const
{
    return pipe_->answered();
}

}  // namespace bazarish::bot
