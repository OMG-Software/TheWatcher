// NotificationWorker: delivery runs off the caller's thread, is bounded,
// retries failures and shuts down cleanly (issue #48, #49).
#include "../server/notification_worker.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace thewatcher::server;
using thewatcher::test::eventually;
using namespace std::chrono_literals;

namespace
{
Notification make_notification(int64_t tag, int max_attempts = 1)
{
    Notification n;
    n.url = "http://hooks.example.net/alert";
    n.target = {"hooks.example.net", 80, "/alert"};
    n.body = "{}";
    n.tag = tag;
    n.max_attempts = max_attempts;
    return n;
}
} // namespace

SCENARIO("Notification status codes decide success (issue #49)")
{
    GIVEN("HTTP status codes")
    {
        THEN("only 2xx counts as delivered")
        {
            REQUIRE(is_success_status(200));
            REQUIRE(is_success_status(204));
            REQUIRE_FALSE(is_success_status(199));
            REQUIRE_FALSE(is_success_status(301));
            REQUIRE_FALSE(is_success_status(404));
            REQUIRE_FALSE(is_success_status(500));
            REQUIRE_FALSE(is_success_status(0));
        }
    }
}

SCENARIO("A webhook that never responds does not block the submitting thread (issue #48)")
{
    GIVEN("a worker whose endpoint hangs until released")
    {
        std::promise<void> release;
        auto released = release.get_future().share();
        std::atomic<int> started{0};
        NotificationWorker worker({}, [&](const Notification&) {
            ++started;
            released.wait();
            return 200;
        });

        WHEN("ten notifications are submitted")
        {
            const auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; ++i)
                REQUIRE(worker.submit(make_notification(i)));
            const auto elapsed = std::chrono::steady_clock::now() - begin;

            THEN("submit returns immediately and the work is still pending")
            {
                REQUIRE(elapsed < 1s);
                REQUIRE(eventually(
                    [&] {
                        return started.load() == 1;
                    },
                    5s, 10ms));
                REQUIRE(worker.pending() == 10);
            }

            release.set_value();
            AND_THEN("all of them are delivered once the endpoint answers")
            {
                REQUIRE(eventually(
                    [&] {
                        return worker.pending() == 0;
                    },
                    5s, 10ms));
                const auto results = worker.drain_results();
                REQUIRE(results.size() == 10);
                for (const auto& r : results)
                    REQUIRE(r.success);
            }
        }
    }
}

SCENARIO("The queue is bounded")
{
    GIVEN("a worker with capacity 2 and a blocked deliverer")
    {
        std::promise<void> release;
        auto released = release.get_future().share();
        std::atomic<bool> busy{false};
        NotificationWorker::Options options;
        options.capacity = 2;
        NotificationWorker worker(options, [&](const Notification&) {
            busy = true;
            released.wait();
            return 200;
        });
        REQUIRE(worker.submit(make_notification(0)));
        REQUIRE(eventually(
            [&] {
                return busy.load();
            },
            5s, 10ms));

        WHEN("more notifications than the queue holds are submitted")
        {
            const bool first = worker.submit(make_notification(1));
            const bool second = worker.submit(make_notification(2));
            const bool third = worker.submit(make_notification(3));

            THEN("the overflow is dropped and counted")
            {
                REQUIRE(first);
                REQUIRE(second);
                REQUIRE_FALSE(third);
                REQUIRE(worker.dropped() == 1);
            }
            release.set_value();
        }
    }
}

SCENARIO("Failed deliveries are retried and reported (issue #49)")
{
    NotificationWorker::Options options;
    options.retry_backoff = 1ms;

    GIVEN("an endpoint that answers 500 twice and then 200")
    {
        std::atomic<int> calls{0};
        NotificationWorker worker(options, [&](const Notification&) {
            return ++calls <= 2 ? 500 : 200;
        });

        WHEN("a notification allowing three attempts is submitted")
        {
            worker.submit(make_notification(7, 3));
            REQUIRE(eventually(
                [&] {
                    return worker.pending() == 0 && calls.load() == 3;
                },
                5s, 10ms));

            THEN("the third attempt succeeds")
            {
                const auto results = worker.drain_results();
                REQUIRE(results.size() == 1);
                REQUIRE(results[0].tag == 7);
                REQUIRE(results[0].success);
                REQUIRE(results[0].attempts == 3);
            }
        }
    }

    GIVEN("an endpoint that always answers 500")
    {
        NotificationWorker worker(options, [](const Notification&) {
            return 500;
        });

        WHEN("a report notification with a single attempt is submitted")
        {
            worker.submit(make_notification(9, 1));
            REQUIRE(eventually(
                [&] {
                    return worker.pending() == 0;
                },
                5s, 10ms));

            THEN("the result is a failure carrying the HTTP status")
            {
                const auto results = worker.drain_results();
                REQUIRE(results.size() == 1);
                REQUIRE_FALSE(results[0].success);
                REQUIRE(results[0].http_status == 500);
                REQUIRE(results[0].attempts == 1);
            }
        }
    }
}

SCENARIO("Destroying the worker does not wait for queued notifications")
{
    GIVEN("a worker with a full queue behind a slow delivery")
    {
        std::atomic<int> calls{0};
        const auto begin = std::chrono::steady_clock::now();
        {
            NotificationWorker worker({}, [&](const Notification&) {
                ++calls;
                std::this_thread::sleep_for(200ms);
                return 200;
            });
            for (int i = 0; i < 20; ++i)
                worker.submit(make_notification(i));
            REQUIRE(eventually(
                [&] {
                    return calls.load() >= 1;
                },
                5s, 10ms));

            WHEN("the worker is destroyed")
            {
                // leaves scope below
            }
        }
        THEN("shutdown abandons the queue instead of delivering all 20")
        {
            REQUIRE(calls.load() < 20);
            REQUIRE(std::chrono::steady_clock::now() - begin < 3s);
        }
    }
}
