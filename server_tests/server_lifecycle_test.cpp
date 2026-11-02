#include "test_support/enrollment_client.hpp"
#include "test_support/server_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <zmq.h>

using namespace thewatcher::test;

SCENARIO("The server build provides ZeroMQ CURVE support")
{
    GIVEN("the linked ZeroMQ library")
    {
        WHEN("CURVE support is queried")
        {
            int has_curve = zmq_has("curve");

            THEN("CURVE is available for encrypted agent traffic")
            {
                REQUIRE(has_curve == 1);
            }
        }
    }
}

SCENARIO("Pending enrollment responses do not approve agents before operator approval")
{
    GIVEN("a running server with an empty store")
    {
        ServerRunner runner(make_in_memory_server_config());
        REQUIRE(runner.startup.wait().started);
        REQUIRE(runner.error == nullptr);

        WHEN("an unknown agent sends its first enrollment request")
        {
            auto response = enroll(
                runner.config.enrollment_address,
                make_enroll_request("pending-agent", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "pending-host"));

            THEN("the response keeps the agent pending")
            {
                REQUIRE(response.has_value());
                REQUIRE(response->approved == false);
                REQUIRE(response->message == "pending approval");
            }
        }
    }
}

SCENARIO("Server startup failures are reported before foreground mode is declared running")
{
    GIVEN("a server configured with an invalid ZMQ bind address")
    {
        auto config = make_in_memory_server_config();
        config.bind_address = "tcp://127.0.0.1:not-a-port";

        WHEN("the server is started on a worker thread")
        {
            ServerRunner runner(config);
            auto result = runner.startup.wait();
            runner.join();

            THEN("the startup signal reports failure instead of success")
            {
                REQUIRE_FALSE(result.started);
                REQUIRE(runner.error != nullptr);
            }

            AND_THEN("the failure message is available to the foreground runner")
            {
                REQUIRE_FALSE(result.message.empty());
            }
        }
    }
}
