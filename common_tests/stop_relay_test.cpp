// Behavioural tests for StopRelay: a stop request is never lost, whichever
// side arrives first.
#include "common/stop_relay.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using thewatcher::process::StopRelay;

SCENARIO("StopRelay delivers a stop request to the attached worker")
{
    GIVEN("a worker that is attached before the stop request")
    {
        StopRelay relay;
        int stops = 0;
        relay.attach([&] {
            ++stops;
        });

        WHEN("a stop is requested")
        {
            relay.request_stop();

            THEN("the worker is stopped once and the request is remembered")
            {
                REQUIRE(stops == 1);
                REQUIRE(relay.stop_requested());
            }
        }
    }
}

SCENARIO("StopRelay keeps a stop request that arrives before the worker exists")
{
    GIVEN("a stop requested while the worker is still starting up")
    {
        StopRelay relay;
        relay.request_stop();
        int stops = 0;

        WHEN("the worker attaches")
        {
            relay.attach([&] {
                ++stops;
            });

            THEN("it is stopped immediately")
            {
                REQUIRE(stops == 1);
            }
        }
    }
}

SCENARIO("StopRelay does not touch a worker that has detached")
{
    GIVEN("a worker that attached and then detached")
    {
        StopRelay relay;
        int stops = 0;
        relay.attach([&] {
            ++stops;
        });
        relay.detach();

        WHEN("a stop is requested afterwards")
        {
            relay.request_stop();

            THEN("the detached worker is not called")
            {
                REQUIRE(stops == 0);
                REQUIRE(relay.stop_requested());
            }
        }
    }
}

SCENARIO("StopRelay::attach_scoped detaches when the scope ends")
{
    GIVEN("a worker attached for the length of a scope")
    {
        StopRelay relay;
        int stops = 0;
        {
            const auto attachment = relay.attach_scoped([&] {
                ++stops;
            });
            relay.request_stop();
            REQUIRE(stops == 1);
        }

        WHEN("a second stop is requested after the scope ended")
        {
            relay.request_stop();

            THEN("the finished worker is not called again")
            {
                REQUIRE(stops == 1);
            }
        }
    }
}

SCENARIO("StopRelay survives concurrent attach and request_stop")
{
    GIVEN("a relay used from two threads")
    {
        for (int round = 0; round < 200; ++round)
        {
            StopRelay relay;
            std::atomic<int> stops{0};
            std::jthread requester([&] {
                relay.request_stop();
            });
            relay.attach([&] {
                ++stops;
            });
            requester.join();

            // Whatever the order, the worker must have been stopped.
            REQUIRE(stops.load() == 1);
        }
    }
}
