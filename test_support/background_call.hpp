// Runs one call on its own thread so a test can wait for it with a deadline,
// for example to check that stop() or run() returns instead of hanging.
#pragma once

#include <chrono>
#include <exception>
#include <future>
#include <thread>
#include <utility>

namespace thewatcher::test
{

class BackgroundCall
{
public:
    // Starts `call` at once on a worker thread. An exception it throws is
    // kept and rethrown by get().
    template <typename Call>
    explicit BackgroundCall(Call call)
    {
        finished_ = done_.get_future();
        worker_ = std::jthread([this, call = std::move(call)]() mutable {
            try
            {
                call();
                done_.set_value();
            }
            catch (...)
            {
                done_.set_exception(std::current_exception());
            }
        });
    }

    BackgroundCall(const BackgroundCall&) = delete;
    BackgroundCall& operator=(const BackgroundCall&) = delete;

    // Waits up to `timeout` for the call to finish.
    template <typename Rep, typename Period>
    [[nodiscard]] std::future_status wait_for(std::chrono::duration<Rep, Period> timeout) const
    {
        return finished_.wait_for(timeout);
    }

    // Waits for the call and rethrows anything it threw.
    void get()
    {
        finished_.get();
    }

private:
    // Declaration order matters: the promise and future must outlive the
    // worker, which is declared last so it is joined first on destruction.
    // Because destruction joins, a test that gives up waiting must first make
    // the call return (for example by calling stop()).
    std::promise<void> done_;
    std::future<void> finished_;
    std::jthread worker_;
};

} // namespace thewatcher::test
