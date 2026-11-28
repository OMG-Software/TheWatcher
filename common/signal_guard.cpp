#include "signal_guard.hpp"

#include <csignal>

namespace thewatcher::process
{

namespace
{

    // Set from the signal handler; the watcher thread turns it into a stop
    // request. Only async-signal-safe work happens in the handler.
    volatile std::sig_atomic_t g_signal_seen = 0;

    extern "C" void record_signal(int)
    {
        g_signal_seen = 1;
    }

} // namespace

SignalGuard::SignalGuard()
{
    g_signal_seen = 0;
#if !defined(_WIN32)
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &set, &previous_mask_);

    // Safety net for a thread that was started before the guard and so still
    // receives the signal: the handler only records it.
    struct sigaction action{};
    action.sa_handler = record_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    sigaction(SIGINT, &action, &previous_int_);
    sigaction(SIGTERM, &action, &previous_term_);
#else
    previous_int_ = std::signal(SIGINT, record_signal);
    previous_term_ = std::signal(SIGTERM, record_signal);
#endif
    // Started after the mask is set, so the watcher inherits it.
    watcher_ = std::jthread([this](std::stop_token token) {
        watch(token);
    });
}

SignalGuard::~SignalGuard()
{
    watcher_.request_stop();
    if (watcher_.joinable())
        watcher_.join();
#if !defined(_WIN32)
    sigaction(SIGINT, &previous_int_, nullptr);
    sigaction(SIGTERM, &previous_term_, nullptr);
    pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr);
#else
    std::signal(SIGINT, previous_int_);
    std::signal(SIGTERM, previous_term_);
#endif
}

void SignalGuard::request_stop()
{
    {
        std::lock_guard lock(mutex_);
        stop_.store(true);
    }
    cv_.notify_all();
}

bool SignalGuard::wait_for(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [this] {
        return stop_.load();
    });
    return stop_.load();
}

void SignalGuard::watch(std::stop_token token)
{
    constexpr auto poll_interval = std::chrono::milliseconds(50);
#if !defined(_WIN32)
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
#endif
    while (!token.stop_requested())
    {
        bool seen = g_signal_seen != 0;
        g_signal_seen = 0;
#if !defined(_WIN32)
        // Blocked signals stay pending; consume them with sigwait so they are
        // not delivered when the mask is restored.
        sigset_t pending;
        sigemptyset(&pending);
        if (sigpending(&pending) == 0 && (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM)))
        {
            int signal_number = 0;
            sigwait(&set, &signal_number);
            seen = true;
        }
#endif
        if (seen)
        {
            // Keep consuming further signals: a second Ctrl+C must not stay
            // pending forever or be delivered when the guard goes away.
            request_stop();
        }
        std::this_thread::sleep_for(poll_interval);
    }
}

} // namespace thewatcher::process
