// AlertPipeline and OfflineAlerts: every alert kind takes one path that
// applies maintenance, silences, severity validation, de-duplication and the
// webhook (issue #52); operator disconnects do not alert (issue #156).
#include "../server/alert_pipeline.hpp"
#include "../server/offline_alerts.hpp"
#include "../server/status_engine.hpp"
#include "../server/store_sqlite.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <mutex>
#include <vector>

using namespace thewatcher::server;
using thewatcher::test::eventually;
using namespace std::chrono_literals;

namespace
{
void add_agent(SqliteStore& store, const std::string& id, bool maintenance = false, bool connected = true)
{
    AgentRecord agent;
    agent.agent_id = id;
    agent.hostname = id + "-host";
    agent.platform = "linux";
    agent.approved = true;
    agent.connected = connected;
    agent.maintenance = maintenance;
    agent.first_seen = 1;
    agent.last_seen = 1000;
    store.upsert_agent(agent);
}

AlertRequest log_alert(const std::string& agent, const std::string& severity = "red", bool dedupe = true)
{
    return {agent, "app-errors", "green", severity, "Log match in /var/log/app.log: boom", 5000, dedupe};
}

// Captures what the worker would POST.
struct Capture
{
    std::mutex mutex;
    std::vector<nlohmann::json> bodies;

    int record(const Notification& n)
    {
        std::lock_guard lock(mutex);
        bodies.push_back(nlohmann::json::parse(n.body));
        return 200;
    }
    std::size_t size()
    {
        std::lock_guard lock(mutex);
        return bodies.size();
    }
};
} // namespace

SCENARIO("A log-match alert respects maintenance (issue #52)")
{
    GIVEN("an agent in maintenance")
    {
        SqliteStore store(":memory:");
        add_agent(store, "a1", /*maintenance=*/true);
        AlertPipeline pipeline(store);

        WHEN("a log match raises an alert")
        {
            const auto outcome = pipeline.raise(log_alert("a1"));

            THEN("no alert is stored")
            {
                REQUIRE(outcome == AlertOutcome::SuppressedMaintenance);
                REQUIRE(store.list_alerts(false).empty());
            }
        }
    }
}

SCENARIO("Silences apply to every alert kind (issue #52)")
{
    GIVEN("an agent with a silence on the Heartbeat indicator")
    {
        SqliteStore store(":memory:");
        add_agent(store, "a1", false, /*connected=*/false);
        store.create_silence({0, "a1", "Heartbeat", "planned", 10'000'000, "admin", 1});
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline);

        WHEN("the offline scan runs")
        {
            const auto raised = offline.raise_for_offline_agents(5000);

            THEN("the offline alert is silenced")
            {
                REQUIRE(raised == 0);
                REQUIRE(store.list_alerts(false).empty());
            }
        }
    }
}

SCENARIO("An invalid severity is rejected (issue #52)")
{
    GIVEN("a pipeline on an empty store")
    {
        SqliteStore store(":memory:");
        add_agent(store, "a1");
        AlertPipeline pipeline(store);

        WHEN("an alert has a severity that is not yellow, amber or red")
        {
            const auto outcome = pipeline.raise(log_alert("a1", "catastrophic"));

            THEN("it is refused and nothing is stored")
            {
                REQUIRE(outcome == AlertOutcome::InvalidSeverity);
                REQUIRE(store.list_alerts(false).empty());
            }
        }
    }
}

SCENARIO("A chatty log raises a single open alert (issue #52)")
{
    GIVEN("an agent with no alerts")
    {
        SqliteStore store(":memory:");
        add_agent(store, "a1");
        AlertPipeline pipeline(store);

        WHEN("100 matching lines raise alerts in one tick")
        {
            int raised = 0;
            for (int i = 0; i < 100; ++i)
                raised += pipeline.raise(log_alert("a1")) == AlertOutcome::Raised ? 1 : 0;

            THEN("one open alert exists")
            {
                REQUIRE(raised == 1);
                REQUIRE(store.list_alerts(false).size() == 1);
            }

            AND_WHEN("the alert is acknowledged and the log matches again")
            {
                store.acknowledge_alert(store.list_alerts(false)[0].alert_id, "op", 6000);
                const auto outcome = pipeline.raise(log_alert("a1"));

                THEN("a new alert is raised")
                {
                    REQUIRE(outcome == AlertOutcome::Raised);
                    REQUIRE(store.list_alerts(false).size() == 2);
                }
            }

            AND_WHEN("the same indicator turns more severe")
            {
                const auto outcome = pipeline.raise(log_alert("a1", "amber"));

                THEN("the new severity is raised too")
                {
                    REQUIRE(outcome == AlertOutcome::Raised);
                }
            }
        }
    }
}

SCENARIO("Every alert kind sends the webhook (issue #52)")
{
    GIVEN("a configured webhook and an agent that is offline")
    {
        SqliteStore store(":memory:");
        add_agent(store, "a1", false, /*connected=*/false);
        store.set_setting("notifications.webhook_url", "http://hooks.example.net/alert");
        Capture capture;
        NotificationWorker worker({}, [&](const Notification& n) {
            return capture.record(n);
        });
        AlertPipeline pipeline(store, worker);
        OfflineAlerts offline(store, pipeline);

        WHEN("the offline scan runs")
        {
            REQUIRE(offline.raise_for_offline_agents(5000) == 1);

            THEN("the webhook is called with the alert")
            {
                REQUIRE(eventually(
                    [&] {
                        return capture.size() == 1;
                    },
                    5s, 10ms));
                REQUIRE(capture.bodies[0]["indicator"] == "Heartbeat");
                REQUIRE(capture.bodies[0]["new_status"] == "red");
                REQUIRE(capture.bodies[0]["agent_id"] == "a1");
            }
        }

        WHEN("a log match raises an alert")
        {
            REQUIRE(pipeline.raise(log_alert("a1")) == AlertOutcome::Raised);

            THEN("the webhook is called as well")
            {
                REQUIRE(eventually(
                    [&] {
                        return capture.size() == 1;
                    },
                    5s, 10ms));
                REQUIRE(capture.bodies[0]["indicator"] == "app-errors");
            }
        }
    }
}

SCENARIO("A webhook failure does not stop the alert being stored")
{
    GIVEN("a webhook URL that fails validation")
    {
        SqliteStore store(":memory:");
        add_agent(store, "a1");
        store.set_setting("notifications.webhook_url", "http://127.0.0.1/alert");
        NotificationWorker worker;
        AlertPipeline pipeline(store, worker);

        WHEN("an alert is raised")
        {
            const auto outcome = pipeline.raise(log_alert("a1"));

            THEN("the alert is stored and nothing is queued")
            {
                REQUIRE(outcome == AlertOutcome::Raised);
                REQUIRE(store.list_alerts(false).size() == 1);
                REQUIRE(worker.pending() == 0);
            }
        }
    }
}

SCENARIO("An operator disconnect does not raise an offline alert (issue #156)")
{
    GIVEN("an agent that an operator disconnected on purpose")
    {
        SqliteStore store(":memory:");
        add_agent(store, "a1", false, /*connected=*/false);
        add_agent(store, "a2", false, /*connected=*/false);
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline);
        offline.mark_operator_disconnect("a1");

        WHEN("the offline scan runs")
        {
            const auto raised = offline.raise_for_offline_agents(5000);

            THEN("only the agent that dropped without being disconnected alerts")
            {
                REQUIRE(raised == 1);
                const auto alerts = store.list_alerts(false);
                REQUIRE(alerts.size() == 1);
                REQUIRE(alerts[0].agent_id == "a2");
            }
        }

        WHEN("the server restarts")
        {
            OfflineAlerts restarted(store, pipeline);

            THEN("the agent is still known as operator-disconnected")
            {
                REQUIRE(restarted.is_operator_disconnected("a1"));
                REQUIRE_FALSE(restarted.is_operator_disconnected("a2"));
            }
        }

        WHEN("the agent connects again and later drops")
        {
            offline.agent_seen("a1");
            const auto raised = offline.raise_for_offline_agents(6000);

            THEN("the next unexpected loss alerts as usual")
            {
                REQUIRE_FALSE(offline.is_operator_disconnected("a1"));
                REQUIRE(raised == 2);
            }
        }
    }
}

// ── Offline detection (#46) ──────────────────────────────────────────────────

namespace
{
void put_agent(SqliteStore& store, const std::string& agent_id, int64_t last_seen, int heartbeat_interval,
               bool connected = true)
{
    AgentRecord agent;
    agent.agent_id = agent_id;
    agent.hostname = "status-host";
    agent.platform = "linux";
    agent.approved = true;
    agent.connected = connected;
    agent.first_seen = 1;
    agent.last_seen = last_seen;
    agent.heartbeat_interval = heartbeat_interval;
    store.upsert_agent(agent);
}
} // namespace

SCENARIO("The effective offline timeout is the larger of the setting and three heartbeat intervals")
{
    GIVEN("the dashboard setting and an agent heartbeat interval")
    {
        THEN("the setting wins for short heartbeats")
        {
            REQUIRE(effective_offline_timeout_seconds(60, 5) == 60);
            REQUIRE(effective_offline_timeout_seconds(300, 60) == 300);
        }
        AND_THEN("three heartbeat intervals win for long heartbeats")
        {
            REQUIRE(effective_offline_timeout_seconds(60, 60) == 180);
            REQUIRE(effective_offline_timeout_seconds(10, 30) == 90);
        }
    }
}

SCENARIO("Offline detection honours the offline_after_seconds setting and each agent's heartbeat")
{
    constexpr int64_t now = 1'000'000;

    GIVEN("offline_after_seconds=300 set in the store and an agent silent for 120 s")
    {
        SqliteStore store(":memory:");
        store.set_setting("offline_after_seconds", "300");
        put_agent(store, "agent-quiet", now - 120'000, 5);
        AlertPipeline pipeline(store);

        WHEN("the offline check runs with a configured default of 60 s")
        {
            const auto gone = OfflineAlerts(store, pipeline, 60).mark_silent_agents_offline(now);

            THEN("the agent is still online")
            {
                REQUIRE(gone.empty());
                REQUIRE(store.get_agent("agent-quiet")->connected);
            }
        }

        WHEN("the agent has been silent for 301 s")
        {
            put_agent(store, "agent-quiet", now - 301'000, 5);
            const auto gone = OfflineAlerts(store, pipeline, 60).mark_silent_agents_offline(now);

            THEN("it goes offline")
            {
                REQUIRE(gone == std::vector<std::string>{"agent-quiet"});
                REQUIRE_FALSE(store.get_agent("agent-quiet")->connected);
            }
        }
    }

    GIVEN("no setting in the store and agents with different heartbeat intervals")
    {
        SqliteStore store(":memory:");
        put_agent(store, "agent-fast", now - 100'000, 5);
        put_agent(store, "agent-slow", now - 100'000, 60);
        AlertPipeline pipeline(store);

        WHEN("both have been silent for 100 s and the default is 60 s")
        {
            const auto gone = OfflineAlerts(store, pipeline, 60).mark_silent_agents_offline(now);

            THEN("only the fast agent goes offline; the 60 s heartbeat agent has 180 s")
            {
                REQUIRE(gone == std::vector<std::string>{"agent-fast"});
                REQUIRE(store.get_agent("agent-slow")->connected);
            }
        }
    }

    GIVEN("an agent in maintenance that has been silent for a long time")
    {
        SqliteStore store(":memory:");
        put_agent(store, "agent-maint-quiet", now - 900'000, 5);
        store.set_agent_maintenance("agent-maint-quiet", true, "patching", 0);
        AlertPipeline pipeline(store);

        WHEN("the offline check runs")
        {
            const auto gone = OfflineAlerts(store, pipeline, 60).mark_silent_agents_offline(now);

            THEN("it is not marked offline")
            {
                REQUIRE(gone.empty());
            }
        }
    }

    GIVEN("a non-positive default and no setting")
    {
        SqliteStore store(":memory:");
        put_agent(store, "agent-disabled", 1, 5);
        AlertPipeline pipeline(store);

        WHEN("the offline check runs")
        {
            const auto gone = OfflineAlerts(store, pipeline, 0).mark_silent_agents_offline(now);

            THEN("offline detection is disabled")
            {
                REQUIRE(gone.empty());
            }
        }
    }
}

SCENARIO("A persisted non-positive offline_after_seconds disables offline detection")
{
    GIVEN("an agent silent for a long time and offline_after_seconds=0 saved in the store")
    {
        SqliteStore store(":memory:");
        store.set_setting("offline_after_seconds", "0");
        put_agent(store, "agent-never-off", 1, 5);
        AlertPipeline pipeline(store);

        WHEN("the offline check runs with a positive configured default")
        {
            const auto gone = OfflineAlerts(store, pipeline, 60).mark_silent_agents_offline(10'000'000);

            THEN("the agent stays online")
            {
                REQUIRE(gone.empty());
                REQUIRE(store.get_agent("agent-never-off")->connected);
            }
        }
    }
}

SCENARIO("The offline timeout combines the setting, the default and the heartbeat")
{
    GIVEN("a store with and without the setting")
    {
        SqliteStore store(":memory:");
        AlertPipeline pipeline(store);

        THEN("the configured default is used when no setting is saved")
        {
            REQUIRE(OfflineAlerts(store, pipeline, 60).timeout_seconds_for(5) == 60);
            REQUIRE(OfflineAlerts(store, pipeline, 60).timeout_seconds_for(60) == 180);
        }

        AND_WHEN("the setting is saved")
        {
            store.set_setting("offline_after_seconds", "300");

            THEN("it replaces the default")
            {
                REQUIRE(OfflineAlerts(store, pipeline, 60).timeout_seconds_for(5) == 300);
            }
        }

        AND_WHEN("detection is disabled by the setting or by the default")
        {
            THEN("the timeout is 0")
            {
                REQUIRE(OfflineAlerts(store, pipeline, 0).timeout_seconds_for(5) == 0);
                store.set_setting("offline_after_seconds", "-1");
                REQUIRE(OfflineAlerts(store, pipeline, 60).timeout_seconds_for(5) == 0);
            }
        }
    }
}

SCENARIO("A saved positive offline_after_seconds enables offline detection that the config disabled")
{
    GIVEN("a config with offline detection disabled and offline_after_seconds=120 saved in the store")
    {
        SqliteStore store(":memory:");
        store.set_setting("offline_after_seconds", "120");
        put_agent(store, "agent-runtime-on", 1'000'000 - 200'000, 5);
        AlertPipeline pipeline(store);

        THEN("the timeout is the saved setting, which also decides route expiry")
        {
            REQUIRE(OfflineAlerts(store, pipeline, 0).timeout_seconds_for(5) == 120);
        }

        WHEN("the agent has been silent past the saved setting")
        {
            const auto gone = OfflineAlerts(store, pipeline, 0).mark_silent_agents_offline(1'000'000);

            THEN("it is marked offline")
            {
                REQUIRE(gone == std::vector<std::string>{"agent-runtime-on"});
                REQUIRE_FALSE(store.get_agent("agent-runtime-on")->connected);
            }
        }
    }
}
SCENARIO("The offline scan marks silent agents offline and raises one alert for each (#46, #52)")
{
    constexpr int64_t now = 1'000'000;

    GIVEN("a silent agent, a recently seen agent and a silent agent an operator disconnected")
    {
        SqliteStore store(":memory:");
        put_agent(store, "agent-silent", now - 120'000, 5);
        put_agent(store, "agent-fresh", now - 10'000, 5);
        put_agent(store, "agent-operator", now - 120'000, 5);
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline, 60);
        offline.mark_operator_disconnect("agent-operator");

        WHEN("the scan runs")
        {
            const auto raised = offline.raise_for_offline_agents(now);

            THEN("only the silent agent is marked offline and alerted")
            {
                REQUIRE(raised == 1);
                REQUIRE_FALSE(store.get_agent("agent-silent")->connected);
                REQUIRE(store.get_agent("agent-fresh")->connected);
                const auto alerts = store.list_alerts(false);
                REQUIRE(alerts.size() == 1);
                REQUIRE(alerts[0].agent_id == "agent-silent");
                REQUIRE(alerts[0].indicator == "Heartbeat");
            }
            AND_WHEN("the scan runs again")
            {
                const auto again = offline.raise_for_offline_agents(now + 1000);

                THEN("no second alert is raised")
                {
                    REQUIRE(again == 0);
                    REQUIRE(store.list_alerts(false).size() == 1);
                }
            }
        }
    }

    GIVEN("offline detection disabled by a non-positive default")
    {
        SqliteStore store(":memory:");
        put_agent(store, "agent-silent", 1, 5);
        put_agent(store, "agent-gone", 1, 5, /*connected=*/false);
        AlertPipeline pipeline(store);
        OfflineAlerts offline(store, pipeline, 0);

        WHEN("the scan runs")
        {
            const auto raised = offline.raise_for_offline_agents(now);

            THEN("nothing is marked offline and nothing alerts")
            {
                REQUIRE_FALSE(offline.detection_enabled());
                REQUIRE(raised == 0);
                REQUIRE(store.get_agent("agent-silent")->connected);
                REQUIRE(store.list_alerts(false).empty());
            }
        }
    }
}

SCENARIO("The pipeline and the status engine agree on maintenance at a window boundary (#44, #52)")
{
    constexpr int64_t start = 100'000;
    constexpr int64_t end = 200'000;

    GIVEN("an agent with a scheduled maintenance window and a status engine using the pipeline")
    {
        SqliteStore store(":memory:");
        put_agent(store, "agent-window", start, 5);
        store.create_maintenance_window({0, "agent-window", start, end, "patching", "admin", 1});
        AlertPipeline pipeline(store);
        StatusEngine engine(store, pipeline);

        WHEN("the window opens")
        {
            engine.reconcile_maintenance(start);

            THEN("the agent is in maintenance and the pipeline suppresses its alerts")
            {
                REQUIRE(store.get_agent("agent-window")->maintenance);
                REQUIRE(pipeline.raise({"agent-window", "cpu", "green", "red", "high", start, false}) ==
                        AlertOutcome::SuppressedMaintenance);
            }
        }

        WHEN("the window has closed")
        {
            engine.reconcile_maintenance(start);
            engine.reconcile_maintenance(end);

            THEN("the agent is out of maintenance and the pipeline raises its alerts again")
            {
                REQUIRE_FALSE(store.get_agent("agent-window")->maintenance);
                REQUIRE(pipeline.raise({"agent-window", "cpu", "green", "red", "high", end, false}) ==
                        AlertOutcome::Raised);
            }
        }
    }
}
