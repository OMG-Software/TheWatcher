#include "server/setting_parse.hpp"
#include "server/store.hpp"
#include "test_support/background_call.hpp"
#include "test_support/enrollment_client.hpp"
#include "test_support/server_runner.hpp"
#include "test_support/temp_dir.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <thread>

#include <zmq.h>

using namespace thewatcher::test;
using namespace std::chrono_literals;

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

SCENARIO("A stop requested before the server finishes starting is not lost (issue #84)")
{
    GIVEN("a server that is told to stop before run() is entered")
    {
        thewatcher::server::Server server(make_in_memory_server_config());
        server.stop();

        WHEN("run() is started afterwards")
        {
            thewatcher::server::StartupSignal startup;
            thewatcher::test::BackgroundCall finished([&] {
                server.run(&startup);
            });

            THEN("run() returns on its own instead of serving until killed")
            {
                REQUIRE(finished.wait_for(5s) == std::future_status::ready);
                REQUIRE_NOTHROW(finished.get());
            }
        }
    }
}

SCENARIO("A corrupt stored setting does not stop the monitoring loop (issue #47)")
{
    GIVEN("a database whose numeric settings hold non-numeric text and a running server")
    {
        ScopedTempDir dir("lifecycle-bad-settings");
        const auto db_path = (dir.path() / "bad-settings.db").string();
        {
            auto store = thewatcher::server::make_store("sqlite", db_path);
            store->set_setting("escalation_timeout_seconds", "not-a-number");
            store->set_setting("reports.enabled", "true");
            store->set_setting("reports.hour", "eight");
        }
        auto config = make_in_memory_server_config();
        config.db_path = db_path;
        ServerRunner runner(config);
        REQUIRE(runner.startup.wait().started);

        WHEN("the periodic checks have run more than once")
        {
            std::this_thread::sleep_for(2500ms);

            THEN("the server is still serving and no error escaped run()")
            {
                REQUIRE(runner.error == nullptr);
                auto response = enroll(runner.config.enrollment_address,
                                       make_enroll_request("survivor", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"));
                REQUIRE(response.has_value());
            }
        }
    }
}

SCENARIO("Integer settings fall back to their default when the stored text is invalid (issue #47)")
{
    GIVEN("stored values and a default")
    {
        using thewatcher::server::parse_int_setting;

        THEN("a valid number is used as is")
        {
            REQUIRE(parse_int_setting("3600", 5, "test") == 3600);
            REQUIRE(parse_int_setting("-1", 5, "test") == -1);
        }

        THEN("text, empty, trailing junk and overflow yield the default")
        {
            REQUIRE(parse_int_setting("abc", 5, "test") == 5);
            REQUIRE(parse_int_setting("", 5, "test") == 5);
            REQUIRE(parse_int_setting("12abc", 5, "test") == 5);
            REQUIRE(parse_int_setting("99999999999999999999", 5, "test") == 5);
        }
    }
}
