// Bounded outbound frame queue shared by the agent's producer threads and its
// IO thread. The lock is only ever held for queue operations, never across a
// ZeroMQ call, so a stalled socket cannot block producers (issue #83).
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace thewatcher::agent
{

enum class FramePriority
{
    Urgent, // heartbeats, command acks, config requests: always sent first
    Bulk,   // metrics and log matches: oldest are dropped when the queue is full
};

class Outbox
{
public:
    using Bytes = std::vector<std::uint8_t>;

    explicit Outbox(std::size_t max_urgent = 64, std::size_t max_bulk = 32)
        : max_urgent_(max_urgent)
        , max_bulk_(max_bulk)
    {
    }

    // Queues `frame`. When its class is full the oldest frame of that class is
    // dropped, so memory stays bounded while the server is unreachable.
    void push(Bytes frame, FramePriority priority)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& queue = priority == FramePriority::Urgent ? urgent_ : bulk_;
        const auto limit = priority == FramePriority::Urgent ? max_urgent_ : max_bulk_;
        if (queue.size() >= limit)
        {
            queue.pop_front();
            ++dropped_;
        }
        queue.push_back(std::move(frame));
    }

    // Removes the next frame to send: urgent frames first, then bulk, each FIFO.
    std::optional<Bytes> pop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto* queue = !urgent_.empty() ? &urgent_ : &bulk_;
        if (queue->empty())
            return std::nullopt;
        Bytes frame = std::move(queue->front());
        queue->pop_front();
        return frame;
    }

    std::size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return urgent_.size() + bulk_.size();
    }

    std::size_t dropped() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return dropped_;
    }

private:
    mutable std::mutex mutex_;
    std::deque<Bytes> urgent_;
    std::deque<Bytes> bulk_;
    std::size_t max_urgent_;
    std::size_t max_bulk_;
    std::size_t dropped_ = 0;
};

} // namespace thewatcher::agent
