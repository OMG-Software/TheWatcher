// apply_command_ack: which successful command ACKs change the stored agent.
// Only DISCONNECT, PAUSE and RESUME confirm a state change; every other ACK
// must leave the agent row (connection and maintenance) as it was.
#include "../server/alert_pipeline.hpp"
#include "../server/command_ack.hpp"
#include "../server/offline_alerts.hpp"
#include "../server/store_sqlite.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace thewatcher::server;
using thewatcher::CommandType;

namespace
{
constexpr int64_t kAckTime = 50000;

void add_agent(SqliteStore& store, const std::string& id, bool maintenance, const std::string& reason,
               int64_t maintenance_until)
{
    AgentRecord agent;
    agent.agent_id = id;
    agent.hostname = id + "-host";
    agent.platform = "linux";
    agent.approved = true;
    agent.connected = true;
    agent.maintenance = maintenance;
    agent.maintenance_reason = reason;
    agent.maintenance_until = maintenance_until;
    agent.first_seen = 1;
    agent.last_seen = 1000;
    store.upsert_agent(agent);
}
} // namespace

SCENARIO("An ACK for a command that does not change maintenance leaves maintenance alone")
{
    GIVEN("an agent in an operator maintenance window with a reason and an expiry")
    {
        SqliteStore store(":memory:");
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline);
        add_agent(store, "a1", true, "patching", 900000);

        const std::array<CommandType, 4> unrelated{CommandType::SET_INTERVAL, CommandType::SET_PROCESS_LIMIT,
                                                   CommandType::RESTART_COLLECTORS, CommandType::GET_STATUS};
        for (const auto type : unrelated)
        {
            WHEN("a successful ACK for command type " + std::to_string(static_cast<int>(type)) + " is applied")
            {
                apply_command_ack(store, offline, pipeline, "a1", type, kAckTime);

                THEN("the agent is still in the same maintenance window and still connected")
                {
                    const auto agent = store.get_agent("a1");
                    REQUIRE(agent);
                    CHECK(agent->maintenance);
                    CHECK(agent->maintenance_reason == "patching");
                    CHECK(agent->maintenance_until == 900000);
                    CHECK(agent->connected);
                    CHECK_FALSE(offline.is_operator_disconnected("a1"));
                }
            }
        }
    }
}

SCENARIO("A PAUSE ACK puts the agent into command maintenance")
{
    GIVEN("a connected agent that is not in maintenance")
    {
        SqliteStore store(":memory:");
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline);
        add_agent(store, "a1", false, "", 0);

        WHEN("a successful PAUSE ACK is applied")
        {
            apply_command_ack(store, offline, pipeline, "a1", CommandType::PAUSE, kAckTime);

            THEN("the agent is in maintenance for the command, with no expiry, and still connected")
            {
                const auto agent = store.get_agent("a1");
                REQUIRE(agent);
                CHECK(agent->maintenance);
                CHECK(agent->maintenance_reason == "command");
                CHECK(agent->maintenance_until == 0);
                CHECK(agent->connected);
                CHECK(agent->last_seen == kAckTime);
            }
        }
    }
}

SCENARIO("A RESUME ACK ends the agent's maintenance")
{
    GIVEN("a connected agent paused by a command")
    {
        SqliteStore store(":memory:");
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline);
        add_agent(store, "a1", true, "command", 0);

        WHEN("a successful RESUME ACK is applied")
        {
            apply_command_ack(store, offline, pipeline, "a1", CommandType::RESUME, kAckTime);

            THEN("the agent is out of maintenance and still connected")
            {
                const auto agent = store.get_agent("a1");
                REQUIRE(agent);
                CHECK_FALSE(agent->maintenance);
                CHECK(agent->maintenance_reason.empty());
                CHECK(agent->connected);
                CHECK(agent->last_seen == kAckTime);
            }
        }
    }
}

SCENARIO("A DISCONNECT ACK marks the agent disconnected on purpose")
{
    GIVEN("a connected agent in maintenance")
    {
        SqliteStore store(":memory:");
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline);
        add_agent(store, "a1", true, "patching", 900000);

        WHEN("a successful DISCONNECT ACK is applied")
        {
            apply_command_ack(store, offline, pipeline, "a1", CommandType::DISCONNECT, kAckTime);

            THEN("the agent is disconnected, the offline scan is told it was deliberate, and maintenance is kept")
            {
                const auto agent = store.get_agent("a1");
                REQUIRE(agent);
                CHECK_FALSE(agent->connected);
                CHECK(agent->last_seen == kAckTime);
                CHECK(offline.is_operator_disconnected("a1"));
                CHECK(agent->maintenance);
                CHECK(agent->maintenance_reason == "patching");
            }
        }
    }
}

SCENARIO("An ACK for an agent that no longer exists changes nothing")
{
    GIVEN("a store without the agent")
    {
        SqliteStore store(":memory:");
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline);

        WHEN("a successful PAUSE or DISCONNECT ACK for it is applied")
        {
            apply_command_ack(store, offline, pipeline, "gone", CommandType::PAUSE, kAckTime);
            apply_command_ack(store, offline, pipeline, "gone", CommandType::DISCONNECT, kAckTime);

            THEN("no agent is created and it is not marked as disconnected")
            {
                CHECK_FALSE(store.get_agent("gone"));
                CHECK_FALSE(offline.is_operator_disconnected("gone"));
            }
        }
    }
}
