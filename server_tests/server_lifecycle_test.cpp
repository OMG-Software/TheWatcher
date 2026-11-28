#include "test_support/enrollment_client.hpp"
#include "test_support/server_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include <zmq.h>

using namespace thewatcher::test;

namespace
{

// Server configuration that asks the OS for both ZeroMQ ports.
thewatcher::server::ServerConfig os_assigned_config()
{
    auto config = make_in_memory_server_config();
    config.bind_address = "tcp://127.0.0.1:*";
    config.enrollment_address = "tcp://127.0.0.1:*";
    config.api_port = 0;
    return config;
}

// The port at the end of "tcp://host:port"; 0 when it is not a number.
int port_of(const std::string& endpoint)
{
    const auto colon = endpoint.rfind(':');
    if (colon == std::string::npos)
        return 0;
    try
    {
        return std::stoi(endpoint.substr(colon + 1));
    }
    catch (const std::exception&)
    {
        return 0;
    }
}

} // namespace

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

SCENARIO("A server asked for OS-assigned ports reports the endpoints it bound")
{
    GIVEN("a server configured with tcp://127.0.0.1:* for both ZeroMQ addresses and API port 0")
    {
        const auto config = os_assigned_config();
        std::exception_ptr failure;
        thewatcher::server::StartupSignal startup;
        thewatcher::server::Server server(config);
        std::thread worker([&] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                failure = std::current_exception();
                startup.fail(failure);
            }
        });

        WHEN("startup succeeds")
        {
            const auto result = startup.wait();
            REQUIRE(result.started);

            THEN("both endpoints are concrete with a non-zero port, and the API has a port")
            {
                REQUIRE(port_of(server.router_endpoint()) > 0);
                REQUIRE(port_of(server.enrollment_endpoint()) > 0);
                REQUIRE(server.router_endpoint() != server.enrollment_endpoint());
                REQUIRE(server.api_port() > 0);
            }

            AND_THEN("a client gets an enrollment reply on the reported enrollment endpoint")
            {
                auto response = enroll(
                    server.enrollment_endpoint(),
                    make_enroll_request("os-port-agent", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "os-port-host"));
                REQUIRE(response.has_value());
                REQUIRE(response->approved == false);
            }
        }

        server.stop();
        worker.join();
    }
}

SCENARIO("Two servers started side by side with OS-assigned ports do not share a port")
{
    GIVEN("two servers configured with tcp://127.0.0.1:* and API port 0")
    {
        ServerRunner first(os_assigned_config());
        ServerRunner second(os_assigned_config());

        WHEN("both have started")
        {
            REQUIRE(first.startup.wait().started);
            REQUIRE(second.startup.wait().started);

            THEN("their ROUTER, enrollment and API ports are all different")
            {
                REQUIRE(first.server.router_endpoint() != second.server.router_endpoint());
                REQUIRE(first.server.enrollment_endpoint() != second.server.enrollment_endpoint());
                REQUIRE(first.server.api_port() != second.server.api_port());
            }

            AND_THEN("each answers enrollment on its own endpoint")
            {
                for (auto* runner : {&first, &second})
                {
                    auto response =
                        enroll(runner->server.enrollment_endpoint(),
                               make_enroll_request("side-by-side", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "host"));
                    REQUIRE(response.has_value());
                }
            }
        }
    }
}
