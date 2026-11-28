#include "../server/sqlite_raii.hpp"
#include "../server/store_sqlite.hpp"
#include "test_support/enrollment_client.hpp"
#include "test_support/server_runner.hpp"
#include "test_support/temp_dir.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include <sqlite3.h>
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

namespace
{
// Counts the rows of the sessions table through its own connection.
int stored_session_count(const std::string& path)
{
    thewatcher::server::sqlite::Connection conn(path);
    thewatcher::server::sqlite::Stmt st;
    REQUIRE(sqlite3_prepare_v2(conn, "SELECT COUNT(*) FROM sessions;", -1, &st.s, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_step(st.s) == SQLITE_ROW);
    return sqlite3_column_int(st.s, 0);
}
} // namespace

SCENARIO("An idle server purges expired sessions on its own")
{
    GIVEN("a database holding an expired session and a server that receives no requests")
    {
        ScopedTempDir dir{"idle-purge"};
        const auto db_path = (dir / "idle.db").string();
        {
            thewatcher::server::SqliteStore store(db_path);
            thewatcher::server::SessionRecord session;
            session.token = "long-expired";
            session.user_id = 1; // the bootstrapped admin
            session.created_at = 1;
            session.expires_at = 2; // long past
            store.create_session(session);
        }
        REQUIRE(stored_session_count(db_path) == 1);
        auto config = make_in_memory_server_config();
        config.db_path = db_path;

        WHEN("the server runs with no login or session request")
        {
            ServerRunner runner(config);
            REQUIRE(runner.startup.wait().started);

            THEN("the periodic maintenance removes the expired session")
            {
                REQUIRE(eventually(
                    [&] {
                        return stored_session_count(db_path) == 0;
                    },
                    10s, 100ms));
            }
        }
    }
}
