#pragma once

#include <functional>
#include <mutex>
#include <utility>

namespace thewatcher::process
{

// Passes a stop request from one thread (for example the Windows Service
// Control Manager callback) to a worker object that another thread creates
// later. The request is remembered, so a stop that arrives before the worker
// has attached is still honoured the moment it does, instead of being lost.
class StopRelay
{
public:
    // Registers the function that stops the worker. If a stop was already
    // requested it runs immediately, on the calling thread.
    void attach(std::function<void()> stopper)
    {
        std::lock_guard lock(mutex_);
        stopper_ = std::move(stopper);
        if (requested_ && stopper_)
            stopper_();
    }

    // Detaches the stopper when it goes out of scope, so a late request cannot
    // reach a worker that is being destroyed (also on an exception).
    class Attachment
    {
    public:
        explicit Attachment(StopRelay& relay)
            : relay_(relay)
        {
        }
        ~Attachment()
        {
            relay_.detach();
        }
        Attachment(const Attachment&) = delete;
        Attachment& operator=(const Attachment&) = delete;

    private:
        StopRelay& relay_;
    };

    // attach() plus a guard that detaches on scope exit.
    [[nodiscard]] Attachment attach_scoped(std::function<void()> stopper)
    {
        attach(std::move(stopper));
        return Attachment(*this);
    }

    // Removes the stopper. Call before the worker object is destroyed so a late
    // request cannot reach a dead object. Blocks while a stopper is running.
    void detach()
    {
        std::lock_guard lock(mutex_);
        stopper_ = nullptr;
    }

    // Records the request and stops the attached worker, if any.
    void request_stop()
    {
        std::lock_guard lock(mutex_);
        requested_ = true;
        if (stopper_)
            stopper_();
    }

    bool stop_requested() const
    {
        std::lock_guard lock(mutex_);
        return requested_;
    }

private:
    mutable std::mutex mutex_;
    std::function<void()> stopper_;
    bool requested_ = false;
};

} // namespace thewatcher::process
