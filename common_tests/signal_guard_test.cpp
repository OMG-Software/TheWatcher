// Behavioural tests for SignalGuard: signals become a stop request and never
// kill the process or land on worker threads.
#include "common/signal_guard.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#if !defined(_WIN32)
#include <signal.h>
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using thewatcher::process::SignalGuard;
using thewatcher::test::eventually;

namespace
{

// Signals the whole process. On POSIX raise() would target only the calling
// thread, whose signals are blocked and never seen by the guard's watcher.
void signal_process(int signal_number)
{
#if defined(_WIN32)
    std::raise(signal_number);
#else
    ::kill(::getpid(), signal_number);
#endif
}

} // namespace

SCENARIO("SignalGuard reports a stop request without terminating the process")
{
    GIVEN("a guard that has seen no signal")
    {
        SignalGuard guard;

        THEN("no stop is requested and waiting times out")
        {
            REQUIRE_FALSE(guard.stop_requested());
            REQUIRE_FALSE(guard.wait_for(20ms));
        }

        WHEN("SIGTERM is raised")
        {
            signal_process(SIGTERM);

            THEN("the stop request appears and wait_for returns true")
            {
                REQUIRE(eventually(
                    [&] {
                        return guard.stop_requested();
                    },
                    5s, 20ms));
                REQUIRE(guard.wait_for(0ms));
            }
        }

        WHEN("SIGINT is raised")
        {
            signal_process(SIGINT);

            THEN("the stop request appears")
            {
                REQUIRE(eventually(
                    [&] {
                        return guard.stop_requested();
                    },
                    5s, 20ms));
            }
        }

        WHEN("a stop is requested programmatically from another thread")
        {
            std::jthread requester([&] {
                std::this_thread::sleep_for(50ms);
                guard.request_stop();
            });

            THEN("a blocked wait_for wakes up with true")
            {
                REQUIRE(guard.wait_for(10s));
            }
        }
    }
}

#if !defined(_WIN32)
SCENARIO("A signal sent to the process is never delivered to a worker thread")
{
    GIVEN("a guard and a worker thread started after it")
    {
        SignalGuard guard;
        std::atomic<bool> release{false};
        sigset_t worker_mask;
        sigemptyset(&worker_mask);
        std::jthread worker([&] {
            pthread_sigmask(SIG_BLOCK, nullptr, &worker_mask);
            while (!release.load())
                std::this_thread::sleep_for(5ms);
        });
        std::this_thread::sleep_for(50ms);

        THEN("the worker inherited the blocked SIGINT and SIGTERM mask, so it cannot see EINTR")
        {
            REQUIRE(sigismember(&worker_mask, SIGINT) == 1);
            REQUIRE(sigismember(&worker_mask, SIGTERM) == 1);
        }

        WHEN("the process is signalled")
        {
            signal_process(SIGTERM);

            THEN("the guard records the stop")
            {
                REQUIRE(eventually(
                    [&] {
                        return guard.stop_requested();
                    },
                    5s, 20ms));
            }
        }
        release.store(true);
    }
}
#endif
