#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
#endif

namespace thewatcher::process
{

// Turns SIGINT and SIGTERM (Ctrl+C / Ctrl+Break on Windows) into a stop
// request that the owner polls or waits on, instead of running code inside a
// signal handler.
//
// On POSIX the constructor blocks both signals in the calling thread, so
// every thread started afterwards inherits the mask. No worker thread can be
// interrupted mid zmq_poll / zmq_recv (EINTR), and a watcher thread picks the
// signals up with sigwait. Construct the guard first in main(), before any
// thread (including the logger's writers) exists. Only one guard may be alive
// at a time.
class SignalGuard
{
public:
    SignalGuard();
    ~SignalGuard();

    SignalGuard(const SignalGuard&) = delete;
    SignalGuard& operator=(const SignalGuard&) = delete;

    // True once a signal arrived or request_stop() was called.
    bool stop_requested() const noexcept
    {
        return stop_.load();
    }

    // Marks the stop as requested and wakes every wait_for() caller.
    void request_stop();

    // Waits up to `timeout` for a stop request. Returns stop_requested().
    bool wait_for(std::chrono::milliseconds timeout);

private:
    void watch(std::stop_token token);

    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
#if !defined(_WIN32)
    sigset_t previous_mask_{};
    struct sigaction previous_int_{};
    struct sigaction previous_term_{};
#else
    void (*previous_int_)(int) = nullptr;
    void (*previous_term_)(int) = nullptr;
#endif
    std::jthread watcher_;
};

} // namespace thewatcher::process
