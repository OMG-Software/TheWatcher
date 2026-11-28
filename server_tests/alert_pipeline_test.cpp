// AlertPipeline and OfflineAlerts: every alert kind takes one path that
// applies maintenance, silences, severity validation, de-duplication and the
// webhook (issue #52); operator disconnects do not alert (issue #156).
#include "../server/alert_pipeline.hpp"
#include "../server/offline_alerts.hpp"
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

SCENARIO("A webhook alert carries the runbook link and the agent's markdown runbook (issue #53)")
{
    GIVEN("a configured webhook, an agent with a markdown runbook and a global runbook for a log indicator")
    {
        SqliteStore store(":memory:");
        AgentRecord agent;
        agent.agent_id = "a1";
        agent.hostname = "a1-host";
        agent.platform = "linux";
        agent.approved = true;
        agent.first_seen = 1;
        agent.last_seen = 1000;
        agent.runbook_markdown = "# App errors\nRestart the app";
        store.upsert_agent(agent);
        RunbookRecord runbook;
        runbook.indicator = "app-errors";
        runbook.status = "red";
        runbook.url = "https://wiki/app-errors";
        store.create_runbook(runbook);
        store.set_setting("notifications.webhook_url", "http://hooks.example.net/alert");
        Capture capture;
        NotificationWorker worker({}, [&](const Notification& n) {
            return capture.record(n);
        });
        AlertPipeline pipeline(store, worker);

        WHEN("a log-match alert is raised")
        {
            REQUIRE(pipeline.raise(log_alert("a1")) == AlertOutcome::Raised);

            THEN("the stored alert and the webhook payload both carry the runbook")
            {
                REQUIRE(eventually(
                    [&] {
                        return capture.size() == 1;
                    },
                    5s, 10ms));
                REQUIRE(capture.bodies[0]["runbook_url"] == "https://wiki/app-errors");
                REQUIRE(capture.bodies[0]["runbook_markdown"] == "# App errors\nRestart the app");
                REQUIRE(store.list_alerts(true).at(0).runbook_url == "https://wiki/app-errors");
            }
        }
    }
}
