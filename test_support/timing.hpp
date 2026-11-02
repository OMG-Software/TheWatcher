// Timing helpers shared by the test suites (no networking dependencies).
#pragma once

#include <chrono>
#include <cstdint>
#include <thread>

namespace thewatcher::test
{

// Wall-clock milliseconds since the Unix epoch.
inline int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Polls `predicate` until it returns true or `timeout` elapses.
template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout,
                std::chrono::milliseconds interval = std::chrono::milliseconds(100))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(interval);
    }
    return predicate();
}

} // namespace thewatcher::test
