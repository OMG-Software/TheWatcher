#include "../server/retention.hpp"
#include "../server/status_engine.hpp"
#include "../server/store_sqlite.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::server;

namespace
{
SystemMetrics metrics_with_cpu(double cpu_percent)
{
    SystemMetrics metrics;
    metrics.cpu.usage_percent = cpu_percent;
    metrics.memory.usage_percent = 10.0;
    metrics.hostname = "status-host";
    metrics.platform = "linux";
    metrics.uptime_seconds = 100.0;

    DiskMetrics disk;
    disk.device = "disk0";
    disk.mount_point = "/";
    disk.filesystem = "ext4";
    disk.usage_percent = 10.0;
    metrics.disks.push_back(disk);

    TemperatureMetrics temperature;
    temperature.sensor_name = "cpu";
    temperature.temperature_celsius = 30.0;
    metrics.temperatures.push_back(temperature);

    ProcessInfo process;
    process.pid = 1;
    process.name = "init";
    process.cpu_percent = 1.0;
    metrics.top_processes.push_back(process);

    NetworkMetrics network;
    network.interface_name = "eth0";
    network.is_up = true;
    metrics.networks.push_back(network);

    return metrics;
}

SystemMetrics metrics_with_processes(std::vector<std::string> process_names)
{
    auto metrics = metrics_with_cpu(10.0);
    metrics.top_processes.clear();
    uint32_t pid = 100;
    for (const auto& name : process_names)
    {
        ProcessInfo process;
        process.pid = pid++;
        process.name = name;
        process.cpu_percent = 1.0;
        metrics.top_processes.push_back(process);
    }
    return metrics;
}

void insert_metrics(SqliteStore& store, const std::string& agent_id, int64_t timestamp_ms, const SystemMetrics& metrics)
{
    store.insert_metrics({agent_id, timestamp_ms, thewatcher::proto::pack(metrics)});
}

void insert_approved_agent(SqliteStore& store, const std::string& agent_id)
{
    AgentRecord agent;
    agent.agent_id = agent_id;
    agent.hostname = "status-host";
    agent.platform = "linux";
    agent.approved = true;
    agent.first_seen = 1;
    agent.last_seen = 1;
    store.upsert_agent(agent);
}
} // namespace

SCENARIO("Status engine records transitions and alerts only on confirmed worsening changes")
{
    GIVEN("an approved agent with baseline metrics")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-status");
        StatusEngine engine(store);

        auto baseline = metrics_with_cpu(10.0);
        insert_metrics(store, "agent-status", 1000, baseline);
        engine.evaluate_metrics("agent-status", baseline, 1000);

        WHEN("the CPU indicator crosses the absolute degraded threshold")
        {
            auto high = metrics_with_cpu(91.0);
            insert_metrics(store, "agent-status", 2000, high);
            engine.evaluate_metrics("agent-status", high, 2000);

            THEN("an alert is generated for the worsening transition")
            {
                auto alerts = store.list_unacknowledged_alerts();
                REQUIRE(alerts.size() == 1);
                REQUIRE(alerts[0].agent_id == "agent-status");
                REQUIRE(alerts[0].indicator == "cpu");
                REQUIRE(alerts[0].old_status == "green");
                REQUIRE(alerts[0].new_status == "amber");
            }

            AND_WHEN("the CPU indicator recovers")
            {
                auto recovered = metrics_with_cpu(10.0);
                insert_metrics(store, "agent-status", 3000, recovered);
                engine.evaluate_metrics("agent-status", recovered, 3000);

                THEN("the recovery is stored in history without creating another alert")
                {
                    auto alerts = store.list_alerts(false);
                    REQUIRE(alerts.size() == 1);

                    auto history = store.list_status_history("agent-status", 20);
                    bool saw_recovery = false;
                    for (const auto& row : history)
                        saw_recovery = saw_recovery || (row.indicator == "cpu" && row.old_status == "amber" &&
                                                        row.new_status == "green");
                    REQUIRE(saw_recovery);
                }
            }
        }
    }
}

SCENARIO("Status engine confirms numeric collector alerts after configured consecutive readings")
{
    GIVEN("an approved agent requiring two CPU readings before changing state")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-confirmed");

        auto agent = store.get_agent("agent-confirmed");
        REQUIRE(agent.has_value());
        agent->collector_config.cpu_readings = 2;
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        StatusEngine engine(store);

        WHEN("only one high CPU reading is received")
        {
            auto baseline = metrics_with_cpu(10.0);
            insert_metrics(store, "agent-confirmed", 1000, baseline);
            engine.evaluate_metrics("agent-confirmed", baseline, 1000);

            auto elevated = metrics_with_cpu(96.0);
            insert_metrics(store, "agent-confirmed", 2000, elevated);
            engine.evaluate_metrics("agent-confirmed", elevated, 2000);

            THEN("the last committed status stays green and the pending count is stored")
            {
                auto status = store.latest_status_for_indicator("agent-confirmed", "cpu");
                REQUIRE(status.has_value());
                REQUIRE(status->new_status == "green");
                auto pending = store.get_pending_status("agent-confirmed", "cpu");
                REQUIRE(pending.has_value());
                REQUIRE(pending->target_status == "red");
                REQUIRE(pending->count == 1);
            }
        }

        AND_WHEN("two consecutive high CPU readings are received")
        {
            auto baseline = metrics_with_cpu(10.0);
            insert_metrics(store, "agent-confirmed", 1000, baseline);
            engine.evaluate_metrics("agent-confirmed", baseline, 1000);

            auto high = metrics_with_cpu(96.0);
            insert_metrics(store, "agent-confirmed", 2000, high);
            engine.evaluate_metrics("agent-confirmed", high, 2000);
            insert_metrics(store, "agent-confirmed", 3000, high);
            engine.evaluate_metrics("agent-confirmed", high, 3000);

            THEN("the red status is committed and an alert is generated")
            {
                auto status = store.latest_status_for_indicator("agent-confirmed", "cpu");
                REQUIRE(status.has_value());
                REQUIRE(status->new_status == "red");
                REQUIRE_FALSE(store.get_pending_status("agent-confirmed", "cpu").has_value());
                REQUIRE(store.list_unacknowledged_alerts().size() == 1);
            }
        }
    }
}

SCENARIO("Status engine uses disk and network collector configuration")
{
    GIVEN("an approved agent with configured disk and network checks")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-items");

        auto agent = store.get_agent("agent-items");
        REQUIRE(agent.has_value());
        DiskMonitorConfig disk_config;
        disk_config.mount_point = "/data";
        disk_config.device = "/dev/sdb1";
        agent->collector_config.disks.push_back(disk_config);
        NetworkInterfaceConfig network_config;
        network_config.interface_name = "eth0";
        agent->collector_config.networks.push_back(network_config);
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        auto metrics = metrics_with_cpu(10.0);
        metrics.disks[0].mount_point = "/data";
        metrics.disks[0].device = "/dev/sdb1";
        metrics.disks[0].usage_percent = 96.0;
        metrics.networks[0].interface_name = "eth0";
        metrics.networks[0].bytes_recv_per_sec = 20'000'000;
        metrics.networks[0].bytes_sent_per_sec = 20'000'000;

        WHEN("metrics cross disk percent and interface Mbps thresholds")
        {
            StatusEngine engine(store);
            insert_metrics(store, "agent-items", 1000, metrics);
            engine.evaluate_metrics("agent-items", metrics, 1000);

            THEN("per-item status history and alerts identify the disk and interface")
            {
                auto disk_status = store.latest_status_for_indicator("agent-items", "disk:/data");
                REQUIRE(disk_status.has_value());
                REQUIRE(disk_status->new_status == "red");
                REQUIRE(disk_status->message.find("/data (/dev/sdb1)") != std::string::npos);

                auto network_status = store.latest_status_for_indicator("agent-items", "network:eth0");
                REQUIRE(network_status.has_value());
                REQUIRE(network_status->new_status == "red");
                REQUIRE(network_status->message.find("eth0") != std::string::npos);
            }
        }
    }
}

SCENARIO("Status engine escalates missing watched processes by consecutive failed readings")
{
    GIVEN("an approved agent watching an exact process name")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-process");

        auto agent = store.get_agent("agent-process");
        REQUIRE(agent.has_value());
        ProcessWatchConfig watch;
        watch.name = "TheWatcherAgent.exe";
        watch.expected_count = 1;
        agent->collector_config.processes.push_back(watch);
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        StatusEngine engine(store);

        WHEN("the watched process is missing three times")
        {
            for (int i = 1; i <= 3; ++i)
            {
                auto metrics = metrics_with_processes({"other.exe"});
                insert_metrics(store, "agent-process", i * 1000, metrics);
                engine.evaluate_metrics("agent-process", metrics, i * 1000);
            }

            THEN("the process status reaches red and the alert message names the missing process")
            {
                auto status = store.latest_status_for_indicator("agent-process", "process:TheWatcherAgent.exe");
                REQUIRE(status.has_value());
                REQUIRE(status->new_status == "red");

                auto alerts = store.list_unacknowledged_alerts();
                REQUIRE_FALSE(alerts.empty());
                REQUIRE(alerts.back().message.find("TheWatcherAgent.exe") != std::string::npos);
            }
        }

        AND_WHEN("the watched process recovers")
        {
            auto missing = metrics_with_processes({"other.exe"});
            insert_metrics(store, "agent-process", 1000, missing);
            engine.evaluate_metrics("agent-process", missing, 1000);

            auto recovered = metrics_with_processes({"TheWatcherAgent.exe"});
            insert_metrics(store, "agent-process", 2000, recovered);
            engine.evaluate_metrics("agent-process", recovered, 2000);

            THEN("the status returns to green and pending count is cleared")
            {
                auto status = store.latest_status_for_indicator("agent-process", "process:TheWatcherAgent.exe");
                REQUIRE(status.has_value());
                REQUIRE(status->new_status == "green");
                REQUIRE_FALSE(store.get_pending_status("agent-process", "process:TheWatcherAgent.exe").has_value());
            }
        }
    }
}

SCENARIO("Maintenance clears active alerts and records blue indicator state")
{
    GIVEN("an approved agent with an active alert")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-maint");

        AlertRecord alert;
        alert.agent_id = "agent-maint";
        alert.indicator = "cpu";
        alert.old_status = "green";
        alert.new_status = "red";
        alert.message = "cpu changed from green to red";
        alert.created_at = 1000;
        store.insert_alert(alert);

        WHEN("the status engine enters maintenance")
        {
            StatusEngine engine(store);
            engine.enter_maintenance("agent-maint", "patch window", 5000, 2000);

            THEN("active alerts are cleared")
            {
                REQUIRE(store.list_unacknowledged_alerts().empty());
            }

            AND_THEN("the agent and indicators are put into maintenance state")
            {
                auto agent = store.get_agent("agent-maint");
                REQUIRE(agent.has_value());
                REQUIRE(agent->maintenance == true);
                REQUIRE(agent->maintenance_reason == "patch window");
                REQUIRE(agent->maintenance_until == 5000);

                auto cpu_status = store.latest_status_for_indicator("agent-maint", "cpu");
                REQUIRE(cpu_status.has_value());
                REQUIRE(cpu_status->new_status == "blue");
            }
        }
    }
}

SCENARIO("status_to_string and status_from_string are mutual inverses for all states")
{
    GIVEN("each IndicatorStatus value")
    {
        const std::vector<std::pair<IndicatorStatus, std::string>> cases = {
            {IndicatorStatus::Green,  "green" },
            {IndicatorStatus::Grey,   "grey"  },
            {IndicatorStatus::Yellow, "yellow"},
            {IndicatorStatus::Amber,  "amber" },
            {IndicatorStatus::Red,    "red"   },
            {IndicatorStatus::Blue,   "blue"  },
        };

        WHEN("each status is converted to string and back")
        {
            THEN("the roundtrip produces the original status")
            {
                for (const auto& [status, name] : cases)
                {
                    REQUIRE(status_to_string(status) == name);
                    REQUIRE(status_from_string(name) == status);
                }
            }
        }

        WHEN("an unknown string is converted")
        {
            THEN("it maps to Grey")
            {
                REQUIRE(status_from_string("unknown") == IndicatorStatus::Grey);
                REQUIRE(status_from_string("") == IndicatorStatus::Grey);
            }
        }

        WHEN("the legacy alias 'orange' is converted")
        {
            THEN("it maps to Amber")
            {
                REQUIRE(status_from_string("orange") == IndicatorStatus::Amber);
            }
        }
    }
}

SCENARIO("is_worse_status returns true only when severity strictly increases")
{
    GIVEN("the full severity ordering: Green < Grey < Yellow < Amber < Red")
    {
        WHEN("a status transitions to something more severe")
        {
            THEN("is_worse_status returns true")
            {
                REQUIRE(is_worse_status(IndicatorStatus::Green, IndicatorStatus::Yellow));
                REQUIRE(is_worse_status(IndicatorStatus::Green, IndicatorStatus::Amber));
                REQUIRE(is_worse_status(IndicatorStatus::Green, IndicatorStatus::Red));
                REQUIRE(is_worse_status(IndicatorStatus::Yellow, IndicatorStatus::Amber));
                REQUIRE(is_worse_status(IndicatorStatus::Yellow, IndicatorStatus::Red));
                REQUIRE(is_worse_status(IndicatorStatus::Amber, IndicatorStatus::Red));
                REQUIRE(is_worse_status(IndicatorStatus::Grey, IndicatorStatus::Yellow));
            }
        }

        WHEN("a status stays the same or improves")
        {
            THEN("is_worse_status returns false")
            {
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Red, IndicatorStatus::Red));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Red, IndicatorStatus::Amber));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Red, IndicatorStatus::Green));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Amber, IndicatorStatus::Green));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Yellow, IndicatorStatus::Green));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Green, IndicatorStatus::Green));
            }
        }

        WHEN("the previous status is Blue (maintenance) or Grey (no data)")
        {
            THEN("it carries no severity, so any real problem is a worsening (#44, #55)")
            {
                REQUIRE(is_worse_status(IndicatorStatus::Blue, IndicatorStatus::Red));
                REQUIRE(is_worse_status(IndicatorStatus::Blue, IndicatorStatus::Yellow));
                REQUIRE(is_worse_status(IndicatorStatus::Grey, IndicatorStatus::Red));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Blue, IndicatorStatus::Green));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Grey, IndicatorStatus::Green));
            }
        }

        WHEN("the new status is Blue or Grey")
        {
            THEN("it is never a worsening, so it never raises an alert (#55)")
            {
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Green, IndicatorStatus::Grey));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Yellow, IndicatorStatus::Grey));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Red, IndicatorStatus::Grey));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Green, IndicatorStatus::Blue));
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Blue, IndicatorStatus::Blue));
            }
        }
    }
}

SCENARIO("exit_maintenance clears the maintenance flag on the agent")
{
    GIVEN("an approved agent in maintenance mode")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-exit-maint");

        StatusEngine engine(store);
        engine.enter_maintenance("agent-exit-maint", "planned downtime", 99999, 1000);

        auto in_maint = store.get_agent("agent-exit-maint");
        REQUIRE(in_maint.has_value());
        REQUIRE(in_maint->maintenance == true);

        WHEN("exit_maintenance is called")
        {
            engine.exit_maintenance("agent-exit-maint");

            THEN("the agent is no longer in maintenance mode")
            {
                auto after = store.get_agent("agent-exit-maint");
                REQUIRE(after.has_value());
                REQUIRE(after->maintenance == false);
                REQUIRE(after->maintenance_reason.empty());
                REQUIRE(after->maintenance_until == 0);
            }
        }
    }
}

SCENARIO("A silence rule suppresses alert generation on status degradation")
{
    GIVEN("an approved agent and an active global silence rule")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-silenced");

        const int64_t now = 10'000;
        SilenceRecord silence;
        silence.agent_id = "*";
        silence.indicator = "*";
        silence.reason = "maintenance window";
        silence.until_ms = now + 3'600'000;
        silence.created_by = "admin";
        silence.created_at = now;
        store.create_silence(silence);

        StatusEngine engine(store);

        WHEN("metrics that would normally trigger a CPU alert are evaluated")
        {
            auto metrics = metrics_with_cpu(95.0); // above default critical threshold
            engine.evaluate_metrics("agent-silenced", metrics, now);

            THEN("no alert is generated")
            {
                auto alerts = store.list_unacknowledged_alerts();
                REQUIRE(alerts.empty());
            }

            AND_THEN("the status transition is still recorded in history")
            {
                auto history = store.list_status_history("agent-silenced", 10);
                REQUIRE_FALSE(history.empty());
            }
        }
    }
}

SCENARIO("Status engine raises a yellow alert when CPU crosses the warning threshold")
{
    GIVEN("an approved agent with default CPU thresholds (warning=80, degraded=90, critical=95)")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-yellow");
        StatusEngine engine(store);

        auto baseline = metrics_with_cpu(10.0);
        insert_metrics(store, "agent-yellow", 1000, baseline);
        engine.evaluate_metrics("agent-yellow", baseline, 1000);

        WHEN("CPU rises to 85 percent (above warning but below degraded)")
        {
            auto elevated = metrics_with_cpu(85.0);
            insert_metrics(store, "agent-yellow", 2000, elevated);
            engine.evaluate_metrics("agent-yellow", elevated, 2000);

            THEN("the alert shows a green-to-yellow transition")
            {
                auto alerts = store.list_unacknowledged_alerts();
                REQUIRE(alerts.size() == 1);
                REQUIRE(alerts[0].indicator == "cpu");
                REQUIRE(alerts[0].old_status == "green");
                REQUIRE(alerts[0].new_status == "yellow");
            }

            AND_WHEN("CPU then drops back below the warning threshold")
            {
                auto recovered = metrics_with_cpu(10.0);
                insert_metrics(store, "agent-yellow", 3000, recovered);
                engine.evaluate_metrics("agent-yellow", recovered, 3000);

                THEN("the yellow alert is resolved and a green recovery is recorded in history")
                {
                    auto active = store.list_alerts(false);
                    REQUIRE(active.size() == 1);

                    auto history = store.list_status_history("agent-yellow", 10);
                    bool saw_recovery = false;
                    for (const auto& row : history)
                        saw_recovery = saw_recovery || (row.indicator == "cpu" && row.old_status == "yellow" &&
                                                        row.new_status == "green");
                    REQUIRE(saw_recovery);
                }
            }
        }
    }
}

SCENARIO("Status engine steps through yellow then amber as CPU worsens")
{
    GIVEN("an approved agent at steady green CPU")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-step");
        StatusEngine engine(store);

        auto baseline = metrics_with_cpu(10.0);
        insert_metrics(store, "agent-step", 1000, baseline);
        engine.evaluate_metrics("agent-step", baseline, 1000);

        WHEN("CPU first crosses warning (85%) then degraded (92%)")
        {
            auto warning = metrics_with_cpu(85.0);
            insert_metrics(store, "agent-step", 2000, warning);
            engine.evaluate_metrics("agent-step", warning, 2000);

            auto degraded = metrics_with_cpu(92.0);
            insert_metrics(store, "agent-step", 3000, degraded);
            engine.evaluate_metrics("agent-step", degraded, 3000);

            THEN("history records green→yellow then yellow→amber transitions")
            {
                auto history = store.list_status_history("agent-step", 10);

                bool saw_yellow = false, saw_amber = false;
                for (const auto& row : history)
                {
                    if (row.indicator == "cpu" && row.old_status == "green" && row.new_status == "yellow")
                        saw_yellow = true;
                    if (row.indicator == "cpu" && row.old_status == "yellow" && row.new_status == "amber")
                        saw_amber = true;
                }
                REQUIRE(saw_yellow);
                REQUIRE(saw_amber);
            }
        }
    }
}

// ── Anomaly detection ─────────────────────────────────────────────────────────

SCENARIO("Anomaly detection upgrades a green CPU reading to yellow when it exceeds multiplier times the baseline mean")
{
    GIVEN("an approved agent with CPU anomaly detection enabled (multiplier=2.0, window=1h) and an established green "
          "state")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-anomaly");

        auto agent = store.get_agent("agent-anomaly");
        REQUIRE(agent.has_value());
        agent->collector_config.cpu_anomaly.multiplier = 2.0;
        agent->collector_config.cpu_anomaly.baseline_window_hours = 1;
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        StatusEngine engine(store);

        // Seed 10 baseline metrics at 10% CPU (mean = 10.0).
        auto baseline = metrics_with_cpu(10.0);
        for (int i = 1; i <= 10; ++i)
            store.insert_metrics({agent->agent_id, static_cast<int64_t>(i * 1000), thewatcher::proto::pack(baseline)});

        // First evaluation at t=300,000 establishes the Green prior state and fills
        // the anomaly cache (cache TTL = 5 min; 300,000 ms >= 300,000 → cold start).
        // 10.0% < 2× mean=10.0 so no anomaly fires here.
        insert_metrics(store, "agent-anomaly", 300'000, baseline);
        engine.evaluate_metrics("agent-anomaly", baseline, 300'000);

        WHEN("a reading of 25% CPU is evaluated at t=700,000 (above 2x mean=10, below warning=80)")
        {
            // t=700,000: cache age = 700,000 - 300,000 = 400,000 >= 300,000 → recomputes mean.
            auto elevated = metrics_with_cpu(25.0);
            engine.evaluate_metrics("agent-anomaly", elevated, 700'000);

            THEN("an anomaly alert fires with yellow status even though 25% is below the warning threshold")
            {
                auto alerts = store.list_unacknowledged_alerts();
                auto cpu_it = std::find_if(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "cpu";
                });
                REQUIRE(cpu_it != alerts.end());
                REQUIRE(cpu_it->old_status == "green");
                REQUIRE(cpu_it->new_status == "yellow");
            }
        }

        WHEN("a reading of 15% CPU is evaluated at t=700,000 (below 2x mean=10)")
        {
            auto normal = metrics_with_cpu(15.0);
            engine.evaluate_metrics("agent-anomaly", normal, 700'000);

            THEN("no anomaly alert is raised")
            {
                auto alerts = store.list_unacknowledged_alerts();
                bool cpu_yellow = std::any_of(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "cpu" && a.new_status == "yellow";
                });
                REQUIRE_FALSE(cpu_yellow);
            }
        }
    }
}

SCENARIO("Anomaly detection does not fire when fewer than 10 baseline samples are available")
{
    GIVEN("an approved agent with CPU anomaly enabled but only 9 baseline samples")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-few-samples");

        auto agent = store.get_agent("agent-few-samples");
        REQUIRE(agent.has_value());
        agent->collector_config.cpu_anomaly.multiplier = 2.0;
        agent->collector_config.cpu_anomaly.baseline_window_hours = 1;
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        StatusEngine engine(store);

        // Only 9 rows — below the minimum of 10 required for a baseline mean.
        // We do NOT insert a 10th metric for the Green-establishing call so the
        // count stays at 9 throughout the scenario.
        auto baseline = metrics_with_cpu(10.0);
        for (int i = 1; i <= 9; ++i)
            store.insert_metrics({agent->agent_id, static_cast<int64_t>(i * 1000), thewatcher::proto::pack(baseline)});

        // Establish Green prior state without seeding an extra row (sparse baseline → mean=-1 → no anomaly).
        engine.evaluate_metrics("agent-few-samples", baseline, 300'000);

        WHEN("a reading well above the expected anomaly multiplier is evaluated")
        {
            auto elevated = metrics_with_cpu(40.0);
            engine.evaluate_metrics("agent-few-samples", elevated, 700'000);

            THEN("no anomaly alert is raised because the baseline is too sparse")
            {
                auto alerts = store.list_unacknowledged_alerts();
                bool cpu_yellow = std::any_of(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "cpu" && a.new_status == "yellow";
                });
                REQUIRE_FALSE(cpu_yellow);
            }
        }
    }
}

// ── Stale metric detection ────────────────────────────────────────────────────

SCENARIO("Stale metric detection upgrades a green CPU indicator to yellow when value is unchanged for too long")
{
    GIVEN("an approved agent with stale detection configured (stale_after_seconds=10)")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-stale");

        auto agent = store.get_agent("agent-stale");
        REQUIRE(agent.has_value());
        agent->collector_config.stale_after_seconds = 10;
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        StatusEngine engine(store);

        // First evaluation: establishes Green state and seeds the staleness clock.
        auto steady = metrics_with_cpu(50.0);
        insert_metrics(store, "agent-stale", 1000, steady);
        engine.evaluate_metrics("agent-stale", steady, 1000);

        WHEN("the same CPU value is reported again after 11 seconds")
        {
            engine.evaluate_metrics("agent-stale", steady, 12'000);

            THEN("the CPU indicator is promoted to yellow by the stale detection")
            {
                auto cpu_status = store.latest_status_for_indicator("agent-stale", "cpu");
                REQUIRE(cpu_status.has_value());
                REQUIRE(cpu_status->new_status == "yellow");
            }
        }

        WHEN("a different CPU value is reported after 11 seconds (value changed by more than epsilon)")
        {
            auto changed = metrics_with_cpu(51.0); // 51 - 50 = 1.0 >> epsilon (0.01)
            engine.evaluate_metrics("agent-stale", changed, 12'000);

            THEN("the CPU indicator remains green because the value changed")
            {
                auto cpu_status = store.latest_status_for_indicator("agent-stale", "cpu");
                REQUIRE(cpu_status.has_value());
                REQUIRE(cpu_status->new_status == "green");
            }
        }
    }
}

// ── classify_percent threshold boundaries ────────────────────────────────────

SCENARIO("classify_percent threshold boundaries: exact values produce the correct status")
{
    GIVEN("an approved agent with default CPU thresholds (warning=80, degraded=90, critical=95)")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-boundary");
        StatusEngine engine(store);

        // Establish prior green state so worsening transitions create alerts.
        auto green_baseline = metrics_with_cpu(10.0);
        insert_metrics(store, "agent-boundary", 1000, green_baseline);
        engine.evaluate_metrics("agent-boundary", green_baseline, 1000);

        WHEN("CPU is exactly at the warning threshold (80.0%)")
        {
            auto at_warning = metrics_with_cpu(80.0);
            insert_metrics(store, "agent-boundary", 2000, at_warning);
            engine.evaluate_metrics("agent-boundary", at_warning, 2000);

            THEN("the indicator transitions to yellow")
            {
                auto s = store.latest_status_for_indicator("agent-boundary", "cpu");
                REQUIRE(s.has_value());
                REQUIRE(s->new_status == "yellow");
            }
        }

        WHEN("CPU is just below the warning threshold (79.9%)")
        {
            auto below_warning = metrics_with_cpu(79.9);
            insert_metrics(store, "agent-boundary", 2000, below_warning);
            engine.evaluate_metrics("agent-boundary", below_warning, 2000);

            THEN("the indicator stays green")
            {
                auto s = store.latest_status_for_indicator("agent-boundary", "cpu");
                REQUIRE(s.has_value());
                REQUIRE(s->new_status == "green");
            }
        }

        WHEN("CPU is exactly at the degraded threshold (90.0%)")
        {
            auto at_degraded = metrics_with_cpu(90.0);
            insert_metrics(store, "agent-boundary", 2000, at_degraded);
            engine.evaluate_metrics("agent-boundary", at_degraded, 2000);

            THEN("the indicator transitions to amber")
            {
                auto s = store.latest_status_for_indicator("agent-boundary", "cpu");
                REQUIRE(s.has_value());
                REQUIRE(s->new_status == "amber");
            }
        }

        WHEN("CPU is exactly at the critical threshold (95.0%)")
        {
            auto at_critical = metrics_with_cpu(95.0);
            insert_metrics(store, "agent-boundary", 2000, at_critical);
            engine.evaluate_metrics("agent-boundary", at_critical, 2000);

            THEN("the indicator transitions to red")
            {
                auto s = store.latest_status_for_indicator("agent-boundary", "cpu");
                REQUIRE(s.has_value());
                REQUIRE(s->new_status == "red");
            }
        }
    }
}

// ── Direct green → red jump ───────────────────────────────────────────────────

SCENARIO("CPU jumps directly from green to red without intermediate states when readings=1")
{
    GIVEN("an approved agent at steady green CPU with cpu_readings=1 (default)")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-direct-red");
        StatusEngine engine(store);

        auto baseline = metrics_with_cpu(10.0);
        insert_metrics(store, "agent-direct-red", 1000, baseline);
        engine.evaluate_metrics("agent-direct-red", baseline, 1000);

        WHEN("CPU spikes immediately to 96% (above critical=95)")
        {
            auto spike = metrics_with_cpu(96.0);
            insert_metrics(store, "agent-direct-red", 2000, spike);
            engine.evaluate_metrics("agent-direct-red", spike, 2000);

            THEN("only one alert is raised and it is green-to-red")
            {
                auto alerts = store.list_unacknowledged_alerts();
                auto cpu_it = std::find_if(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "cpu";
                });
                REQUIRE(cpu_it != alerts.end());
                REQUIRE(cpu_it->old_status == "green");
                REQUIRE(cpu_it->new_status == "red");
            }

            AND_THEN("no yellow or amber history entries exist for cpu")
            {
                auto history = store.list_status_history("agent-direct-red", 20);
                for (const auto& row : history)
                {
                    if (row.indicator == "cpu")
                    {
                        REQUIRE(row.new_status != "yellow");
                        REQUIRE(row.new_status != "amber");
                    }
                }
            }
        }
    }
}

// ── Process monitoring: only watched processes alert ─────────────────────────

SCENARIO("Process monitoring fires alerts only for watched processes, not arbitrary missing ones")
{
    GIVEN("an approved agent watching exactly one process name")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-proc-scope");

        auto agent = store.get_agent("agent-proc-scope");
        REQUIRE(agent.has_value());
        ProcessWatchConfig watch;
        watch.name = "monitored.exe";
        watch.expected_count = 1;
        agent->collector_config.processes.push_back(watch);
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        StatusEngine engine(store);

        // Establish green baseline: process is present.
        auto with_proc = metrics_with_processes({"monitored.exe"});
        insert_metrics(store, "agent-proc-scope", 1, with_proc);
        engine.evaluate_metrics("agent-proc-scope", with_proc, 1);

        WHEN("a metrics snapshot has no running processes at all")
        {
            auto no_procs = metrics_with_processes({});
            for (int i = 1; i <= 3; ++i)
            {
                insert_metrics(store, "agent-proc-scope", i * 1000, no_procs);
                engine.evaluate_metrics("agent-proc-scope", no_procs, i * 1000);
            }

            THEN("an alert fires only for the watched process indicator")
            {
                auto alerts = store.list_unacknowledged_alerts();
                REQUIRE_FALSE(alerts.empty());
                for (const auto& a : alerts)
                    REQUIRE(a.indicator == "process:monitored.exe");
            }
        }

        WHEN("the watched process is present but other processes are absent")
        {
            auto with_watched = metrics_with_processes({"monitored.exe", "other.exe"});
            for (int i = 1; i <= 3; ++i)
            {
                insert_metrics(store, "agent-proc-scope", i * 1000, with_watched);
                engine.evaluate_metrics("agent-proc-scope", with_watched, i * 1000);
            }

            THEN("no alerts fire because the watched process is present")
            {
                auto alerts = store.list_unacknowledged_alerts();
                bool watched_alert = std::any_of(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "process:monitored.exe";
                });
                REQUIRE_FALSE(watched_alert);
            }
        }
    }
}

SCENARIO("Process monitoring with process_readings=1 fires red on the very first missing observation")
{
    GIVEN("an approved agent with a watched process and process_readings=1")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-proc-fast");

        auto agent = store.get_agent("agent-proc-fast");
        REQUIRE(agent.has_value());
        ProcessWatchConfig watch;
        watch.name = "critical.exe";
        watch.expected_count = 1;
        agent->collector_config.processes.push_back(watch);
        agent->collector_config.process_readings = 1; // first miss → immediate red
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        StatusEngine engine(store);

        // Establish Green state by first evaluating with the process present.
        auto with_proc = metrics_with_processes({"critical.exe"});
        insert_metrics(store, "agent-proc-fast", 1000, with_proc);
        engine.evaluate_metrics("agent-proc-fast", with_proc, 1000);

        WHEN("the watched process disappears in the very next snapshot")
        {
            auto missing = metrics_with_processes({"something-else.exe"});
            insert_metrics(store, "agent-proc-fast", 2000, missing);
            engine.evaluate_metrics("agent-proc-fast", missing, 2000);

            THEN("a red alert fires immediately without waiting for multiple readings")
            {
                auto alerts = store.list_unacknowledged_alerts();
                auto it = std::find_if(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "process:critical.exe";
                });
                REQUIRE(it != alerts.end());
                REQUIRE(it->new_status == "red");
            }
        }
    }
}

// ── Exit maintenance: indicators remain blue until next evaluation ────────────

SCENARIO("exit_maintenance clears the maintenance flag but does not immediately re-evaluate indicators")
{
    GIVEN("an approved agent in maintenance mode (indicators are blue)")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-exit-maint2");

        StatusEngine engine(store);
        engine.enter_maintenance("agent-exit-maint2", "patching", 99999, 1000);

        auto cpu_blue = store.latest_status_for_indicator("agent-exit-maint2", "cpu");
        REQUIRE(cpu_blue.has_value());
        REQUIRE(cpu_blue->new_status == "blue");

        WHEN("exit_maintenance is called")
        {
            engine.exit_maintenance("agent-exit-maint2");

            THEN("the agent maintenance flag is cleared")
            {
                auto agent = store.get_agent("agent-exit-maint2");
                REQUIRE(agent.has_value());
                REQUIRE(agent->maintenance == false);
            }

            AND_THEN("the cpu indicator status is still blue (no re-evaluation has occurred)")
            {
                auto cpu_status = store.latest_status_for_indicator("agent-exit-maint2", "cpu");
                REQUIRE(cpu_status.has_value());
                REQUIRE(cpu_status->new_status == "blue");
            }

            AND_WHEN("a new normal metrics snapshot is evaluated after exiting maintenance")
            {
                auto normal = metrics_with_cpu(10.0);
                insert_metrics(store, "agent-exit-maint2", 2000, normal);
                engine.evaluate_metrics("agent-exit-maint2", normal, 2000);

                THEN("the cpu indicator returns to green after the first post-maintenance evaluation")
                {
                    auto cpu_status = store.latest_status_for_indicator("agent-exit-maint2", "cpu");
                    REQUIRE(cpu_status.has_value());
                    REQUIRE(cpu_status->new_status == "green");
                }
            }
        }
    }
}

// ── non-finite CPU values ─────────────────────────────────────────────────────

SCENARIO("classify_percent treats NaN and infinite CPU values as grey")
{
    GIVEN("an approved agent with an established green CPU state")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-nan");
        StatusEngine engine(store);

        auto baseline = metrics_with_cpu(10.0);
        insert_metrics(store, "agent-nan", 1000, baseline);
        engine.evaluate_metrics("agent-nan", baseline, 1000);

        WHEN("a metrics snapshot with NaN CPU is evaluated")
        {
            auto nan_metrics = metrics_with_cpu(std::numeric_limits<double>::quiet_NaN());
            insert_metrics(store, "agent-nan", 2000, nan_metrics);
            engine.evaluate_metrics("agent-nan", nan_metrics, 2000);

            THEN("the cpu indicator transitions to grey (non-finite → grey classification)")
            {
                auto s = store.latest_status_for_indicator("agent-nan", "cpu");
                REQUIRE(s.has_value());
                REQUIRE(s->new_status == "grey");
            }
        }
    }
}

SCENARIO("record_transition attaches a runbook URL to alerts when a matching runbook exists")
{
    GIVEN("a store with a cpu/red runbook and an approved agent")
    {
        SqliteStore store(":memory:");
        StatusEngine engine(store);

        insert_approved_agent(store, "agent-runbook");

        RunbookRecord rb;
        rb.indicator = "cpu";
        rb.status = "red";
        rb.url = "https://wiki.example.com/cpu-runbook";
        rb.created_at = 1000;
        store.create_runbook(rb);

        WHEN("cpu transitions from green to red")
        {
            engine.evaluate_metrics("agent-runbook", metrics_with_cpu(10.0), 1000);
            engine.evaluate_metrics("agent-runbook", metrics_with_cpu(99.0), 2000);

            THEN("an alert is generated with the runbook URL set")
            {
                auto alerts = store.list_alerts(false);
                auto it = std::find_if(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "cpu" && a.new_status == "red";
                });
                REQUIRE(it != alerts.end());
                REQUIRE(it->runbook_url == "https://wiki.example.com/cpu-runbook");
            }
        }
    }
}

SCENARIO("record_transition leaves runbook_url empty when no matching runbook exists")
{
    GIVEN("a store with no runbooks and an approved agent")
    {
        SqliteStore store(":memory:");
        StatusEngine engine(store);

        insert_approved_agent(store, "agent-no-runbook");

        WHEN("cpu transitions from green to red")
        {
            engine.evaluate_metrics("agent-no-runbook", metrics_with_cpu(10.0), 1000);
            engine.evaluate_metrics("agent-no-runbook", metrics_with_cpu(99.0), 2000);

            THEN("the generated alert has an empty runbook_url")
            {
                auto alerts = store.list_alerts(false);
                auto it = std::find_if(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "cpu" && a.new_status == "red";
                });
                REQUIRE(it != alerts.end());
                REQUIRE(it->runbook_url.empty());
            }
        }
    }
}

// ── Maintenance state machine (#44, #45) ─────────────────────────────────────

namespace
{
bool has_alert(SqliteStore& store, const std::string& indicator, const std::string& new_status)
{
    for (const auto& alert : store.list_alerts(false))
    {
        if (alert.indicator == indicator && alert.new_status == new_status)
            return true;
    }
    return false;
}
} // namespace

SCENARIO("An indicator that is already red when maintenance ends raises an alert")
{
    GIVEN("an agent whose CPU was green before it entered maintenance")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-after-maint");
        StatusEngine engine(store);
        engine.evaluate_metrics("agent-after-maint", metrics_with_cpu(10.0), 1000);
        engine.enter_maintenance("agent-after-maint", "patching", 0, 2000);

        WHEN("maintenance ends and the next CPU reading is red")
        {
            engine.exit_maintenance("agent-after-maint");
            engine.evaluate_metrics("agent-after-maint", metrics_with_cpu(99.0), 3000);

            THEN("a red alert is raised for cpu")
            {
                REQUIRE(has_alert(store, "cpu", "red"));
            }
        }

        WHEN("maintenance ends and the next CPU reading is green")
        {
            engine.exit_maintenance("agent-after-maint");
            engine.evaluate_metrics("agent-after-maint", metrics_with_cpu(10.0), 3000);

            THEN("no alert is raised and the indicator returns to green")
            {
                REQUIRE(store.list_alerts(false).empty());
                REQUIRE(store.latest_status_for_indicator("agent-after-maint", "cpu")->new_status == "green");
            }
        }
    }
}

SCENARIO("Entering maintenance marks the agent's real indicators blue and no phantom ones")
{
    GIVEN("an agent with disk, network and process indicators on record")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-real-ind");
        auto agent = store.get_agent("agent-real-ind");
        ProcessWatchConfig watch;
        watch.name = "init";
        watch.expected_count = 1;
        agent->collector_config.processes.push_back(watch);
        store.set_agent_collector_config("agent-real-ind", agent->collector_config);

        StatusEngine engine(store);
        engine.evaluate_metrics("agent-real-ind", metrics_with_cpu(10.0), 1000);
        REQUIRE(store.latest_status_for_indicator("agent-real-ind", "disk:/").has_value());

        WHEN("the agent enters maintenance")
        {
            engine.enter_maintenance("agent-real-ind", "patching", 0, 2000);

            THEN("every existing indicator is blue")
            {
                for (const auto& name : {"cpu", "memory", "disk:/", "network:eth0", "process:init"})
                {
                    const auto row = store.latest_status_for_indicator("agent-real-ind", name);
                    REQUIRE(row.has_value());
                    REQUIRE(row->new_status == "blue");
                }
            }

            AND_THEN("no indicator named after a whole category is created")
            {
                for (const auto& name : {"disk", "network", "processes"})
                    REQUIRE_FALSE(store.latest_status_for_indicator("agent-real-ind", name).has_value());
            }
        }
    }
}

SCENARIO("Timed manual maintenance ends when its until time passes")
{
    GIVEN("an agent put into maintenance until t=5000")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-timed");
        StatusEngine engine(store);
        engine.enter_maintenance("agent-timed", "patching", 5000, 1000);

        WHEN("the maintenance tick runs before the until time")
        {
            engine.reconcile_maintenance(4000);

            THEN("the agent is still in maintenance")
            {
                REQUIRE(store.get_agent("agent-timed")->maintenance);
            }
        }

        WHEN("the maintenance tick runs after the until time")
        {
            engine.reconcile_maintenance(5000);

            THEN("maintenance is cleared")
            {
                const auto agent = store.get_agent("agent-timed");
                REQUIRE_FALSE(agent->maintenance);
                REQUIRE(agent->maintenance_until == 0);
            }

            AND_WHEN("the next CPU reading is red")
            {
                engine.evaluate_metrics("agent-timed", metrics_with_cpu(99.0), 6000);

                THEN("evaluation has resumed and alerts")
                {
                    REQUIRE(has_alert(store, "cpu", "red"));
                }
            }
        }
    }

    GIVEN("an agent in open-ended manual maintenance (until=0)")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-open");
        StatusEngine engine(store);
        engine.enter_maintenance("agent-open", "command", 0, 1000);

        WHEN("the maintenance tick runs much later")
        {
            engine.reconcile_maintenance(999'999'999);

            THEN("the agent stays in maintenance")
            {
                REQUIRE(store.get_agent("agent-open")->maintenance);
            }
        }
    }
}

SCENARIO("Scheduled maintenance windows follow the same state machine as manual maintenance")
{
    GIVEN("an agent with a CPU alert and a window from t=2000 to t=4000 covering all agents")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-window");
        StatusEngine engine(store);
        engine.evaluate_metrics("agent-window", metrics_with_cpu(10.0), 1000);
        engine.evaluate_metrics("agent-window", metrics_with_cpu(99.0), 1500);
        REQUIRE(store.list_unacknowledged_alerts().size() == 1);

        MaintenanceWindowRecord window;
        window.agent_id = "*";
        window.start_ms = 2000;
        window.end_ms = 4000;
        window.reason = "nightly";
        store.create_maintenance_window(window);

        WHEN("the tick runs inside the window")
        {
            engine.reconcile_maintenance(3000);

            THEN("the agent is in maintenance with the window reason and its indicators are blue")
            {
                const auto agent = store.get_agent("agent-window");
                REQUIRE(agent->maintenance);
                REQUIRE(agent->maintenance_reason == "mw:nightly");
                REQUIRE(store.latest_status_for_indicator("agent-window", "cpu")->new_status == "blue");
            }

            AND_THEN("active alerts are cleared like manual maintenance")
            {
                REQUIRE(store.list_unacknowledged_alerts().empty());
            }

            AND_WHEN("the tick runs after the window ended")
            {
                engine.reconcile_maintenance(4000);

                THEN("the agent leaves maintenance")
                {
                    REQUIRE_FALSE(store.get_agent("agent-window")->maintenance);
                }

                AND_THEN("a red reading afterwards alerts")
                {
                    engine.evaluate_metrics("agent-window", metrics_with_cpu(99.0), 5000);
                    REQUIRE(has_alert(store, "cpu", "red"));
                }
            }
        }
    }

    GIVEN("an agent in manual maintenance and no window")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-manual");
        StatusEngine engine(store);
        engine.enter_maintenance("agent-manual", "by hand", 0, 1000);

        WHEN("the tick runs")
        {
            engine.reconcile_maintenance(2000);

            THEN("the manual maintenance is not ended by the window logic")
            {
                REQUIRE(store.get_agent("agent-manual")->maintenance);
            }
        }
    }
}

SCENARIO("Maintenance set directly in the store is reflected in the indicators by the tick")
{
    GIVEN("an agent with a green CPU whose maintenance flag was set by the API")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-api-flag");
        StatusEngine engine(store);
        engine.evaluate_metrics("agent-api-flag", metrics_with_cpu(10.0), 1000);
        store.set_agent_maintenance("agent-api-flag", true, "from api", 0);

        WHEN("the maintenance tick runs")
        {
            engine.reconcile_maintenance(2000);

            THEN("the CPU indicator is blue")
            {
                REQUIRE(store.latest_status_for_indicator("agent-api-flag", "cpu")->new_status == "blue");
            }
        }
    }
}

// ── Grey means no data (#55) ─────────────────────────────────────────────────

SCENARIO("A non-finite reading after green raises no alert")
{
    GIVEN("an agent with an established green CPU")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-grey");
        StatusEngine engine(store);
        engine.evaluate_metrics("agent-grey", metrics_with_cpu(10.0), 1000);

        WHEN("a NaN reading arrives and then a normal one")
        {
            engine.evaluate_metrics("agent-grey", metrics_with_cpu(std::numeric_limits<double>::quiet_NaN()), 2000);
            engine.evaluate_metrics("agent-grey", metrics_with_cpu(10.0), 3000);

            THEN("no alert exists at all")
            {
                REQUIRE(store.list_alerts(false).empty());
            }
        }

        WHEN("an infinite reading arrives")
        {
            engine.evaluate_metrics("agent-grey", metrics_with_cpu(std::numeric_limits<double>::infinity()), 2000);

            THEN("the indicator is grey and no alert exists")
            {
                REQUIRE(store.latest_status_for_indicator("agent-grey", "cpu")->new_status == "grey");
                REQUIRE(store.list_alerts(false).empty());
            }
        }
    }
}

// ── Long-lived engine (#43) ──────────────────────────────────────────────────

SCENARIO("evaluate_metrics does not prune; retention is a separate periodic task")
{
    GIVEN("an agent with a metrics row far older than the retention period")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-no-prune");
        store.insert_metrics({"agent-no-prune", 1000, {0x01}});
        StatusEngine engine(store);

        WHEN("a frame is evaluated long after the old row")
        {
            engine.evaluate_metrics("agent-no-prune", metrics_with_cpu(10.0), 5'000'000'000LL);

            THEN("the old row is still there")
            {
                REQUIRE(store.get_metrics("agent-no-prune", 10).size() == 1);
            }
        }
    }
}

SCENARIO("Stale detection works across separate evaluate_metrics calls on one engine and tracks agents separately")
{
    GIVEN("two agents with stale detection at 10 seconds and one engine")
    {
        SqliteStore store(":memory:");
        for (const auto* id : {"agent-s1", "agent-s2"})
        {
            insert_approved_agent(store, id);
            auto agent = store.get_agent(id);
            agent->collector_config.stale_after_seconds = 10;
            store.set_agent_collector_config(id, agent->collector_config);
        }
        StatusEngine engine(store);

        WHEN("agent 1 repeats its value for 11 s while agent 2 changes its value")
        {
            engine.evaluate_metrics("agent-s1", metrics_with_cpu(50.0), 1000);
            engine.evaluate_metrics("agent-s2", metrics_with_cpu(50.0), 1000);
            engine.evaluate_metrics("agent-s1", metrics_with_cpu(50.0), 12'000);
            engine.evaluate_metrics("agent-s2", metrics_with_cpu(60.0), 12'000);

            THEN("only agent 1 is stale")
            {
                REQUIRE(store.latest_status_for_indicator("agent-s1", "cpu")->new_status == "yellow");
                REQUIRE(store.latest_status_for_indicator("agent-s2", "cpu")->new_status == "green");
            }
        }
    }
}

// ── Retention (#76) ──────────────────────────────────────────────────────────

SCENARIO("The retention task prunes every growing table without any metrics arriving")
{
    constexpr int64_t day = 86'400'000LL;
    constexpr int64_t now = 400 * day;

    GIVEN("old and recent rows in each growing table")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-ret");
        store.insert_metrics({"agent-ret", now - 40 * day, {0x01}});
        store.insert_metrics({"agent-ret", now - day, {0x02}});

        // status_history: an old row that is also the newest of its indicator, and
        // old+new rows for another indicator.
        store.insert_status_history({0, "agent-ret", "stable", "green", "green", "", now - 300 * day});
        store.insert_status_history({0, "agent-ret", "flappy", "green", "red", "", now - 300 * day});
        store.insert_status_history({0, "agent-ret", "flappy", "red", "green", "", now - day});

        AlertRecord old_open;
        old_open.agent_id = "agent-ret";
        old_open.indicator = "open";
        old_open.created_at = now - 300 * day;
        const auto open_id = store.insert_alert(old_open);
        AlertRecord old_acked = old_open;
        old_acked.indicator = "acked";
        const auto acked_id = store.insert_alert(old_acked);
        store.acknowledge_alert(acked_id, "admin", now - 299 * day);
        AlertRecord recent = old_open;
        recent.indicator = "recent";
        recent.created_at = now - day;
        store.insert_alert(recent);
        (void)open_id;

        store.insert_log_match({0, "agent-ret", "log", "/var/log/x", "old line", "red", now - 40 * day});
        store.insert_log_match({0, "agent-ret", "log", "/var/log/x", "new line", "red", now - day});

        SilenceRecord expired;
        expired.agent_id = "*";
        expired.indicator = "*";
        expired.until_ms = now - 200 * day;
        expired.created_at = now - 201 * day;
        store.create_silence(expired);
        SilenceRecord active = expired;
        active.until_ms = now + day;
        store.create_silence(active);

        MaintenanceWindowRecord finished;
        finished.agent_id = "*";
        finished.start_ms = now - 201 * day;
        finished.end_ms = now - 200 * day;
        finished.created_at = now - 202 * day;
        store.create_maintenance_window(finished);
        MaintenanceWindowRecord upcoming = finished;
        upcoming.start_ms = now + day;
        upcoming.end_ms = now + 2 * day;
        store.create_maintenance_window(upcoming);

        WHEN("retention runs with the default settings")
        {
            run_retention(store, now);

            THEN("old metrics and log matches are gone and recent ones stay")
            {
                REQUIRE(store.get_metrics("agent-ret", 10).size() == 1);
                const auto matches = store.list_log_matches("agent-ret", 10);
                REQUIRE(matches.size() == 1);
                REQUIRE(matches[0].matched_line == "new line");
            }

            AND_THEN("old status history is gone but each indicator keeps its newest row")
            {
                REQUIRE(store.latest_status_for_indicator("agent-ret", "stable").has_value());
                const auto flappy = store.latest_status_for_indicator("agent-ret", "flappy");
                REQUIRE(flappy.has_value());
                REQUIRE(flappy->new_status == "green");
                REQUIRE(store.list_status_history("agent-ret", 100).size() == 2);
            }

            AND_THEN("old handled alerts are gone but open and recent alerts stay")
            {
                std::vector<std::string> kept;
                for (const auto& alert : store.list_alerts(true))
                    kept.push_back(alert.indicator);
                std::sort(kept.begin(), kept.end());
                REQUIRE(kept == std::vector<std::string>{"open", "recent"});
            }

            AND_THEN("expired silences and finished windows are removed, active ones stay")
            {
                REQUIRE(store.list_silences().size() == 1);
                REQUIRE(store.list_maintenance_windows().size() == 1);
            }
        }

        WHEN("a retention setting is shortened")
        {
            store.set_setting("metrics_retention_days", "1000");
            store.set_setting("log_matches_retention_days", "1000");
            run_retention(store, now);

            THEN("the longer period is honoured")
            {
                REQUIRE(store.get_metrics("agent-ret", 10).size() == 2);
                REQUIRE(store.list_log_matches("agent-ret", 10).size() == 2);
            }
        }

        WHEN("a retention setting is garbage")
        {
            store.set_setting("metrics_retention_days", "not-a-number");
            run_retention(store, now);

            THEN("the default is used instead of throwing")
            {
                REQUIRE(store.get_metrics("agent-ret", 10).size() == 1);
            }
        }
    }
}

// ── Review follow-ups for PR #174 ────────────────────────────────────────────

SCENARIO("The first post-maintenance stale check starts a fresh clock")
{
    GIVEN("an agent with stale detection at 10 s that reported 50% before maintenance")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-stale-maint");
        auto agent = store.get_agent("agent-stale-maint");
        agent->collector_config.stale_after_seconds = 10;
        store.set_agent_collector_config("agent-stale-maint", agent->collector_config);
        StatusEngine engine(store);
        engine.evaluate_metrics("agent-stale-maint", metrics_with_cpu(50.0), 1000);

        WHEN("maintenance lasts a long time and the same value is reported afterwards")
        {
            engine.enter_maintenance("agent-stale-maint", "patching", 0, 2000);
            engine.exit_maintenance("agent-stale-maint");
            engine.evaluate_metrics("agent-stale-maint", metrics_with_cpu(50.0), 60'000);

            THEN("the indicator is not flagged stale")
            {
                REQUIRE(store.latest_status_for_indicator("agent-stale-maint", "cpu")->new_status == "green");
            }
        }
    }
}

SCENARIO("Consecutive-reading confirmation counts the first reading after maintenance")
{
    GIVEN("an agent needing 3 consecutive red CPU readings that was in maintenance")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-confirm");
        auto agent = store.get_agent("agent-confirm");
        agent->collector_config.cpu_readings = 3;
        store.set_agent_collector_config("agent-confirm", agent->collector_config);
        StatusEngine engine(store);
        engine.evaluate_metrics("agent-confirm", metrics_with_cpu(10.0), 1000);
        engine.enter_maintenance("agent-confirm", "patching", 0, 2000);
        engine.exit_maintenance("agent-confirm");

        WHEN("three red readings arrive after maintenance")
        {
            engine.evaluate_metrics("agent-confirm", metrics_with_cpu(99.0), 3000);
            engine.evaluate_metrics("agent-confirm", metrics_with_cpu(99.0), 4000);
            const bool alerted_before_third = has_alert(store, "cpu", "red");
            engine.evaluate_metrics("agent-confirm", metrics_with_cpu(99.0), 5000);

            THEN("the alert is raised on the third reading, not the fourth")
            {
                REQUIRE_FALSE(alerted_before_third);
                REQUIRE(has_alert(store, "cpu", "red"));
            }
        }
    }
}

SCENARIO("A pause acknowledgement keeps the maintenance reason and expiry already set")
{
    GIVEN("an agent put into timed maintenance through the API")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-pause");
        store.set_agent_maintenance("agent-pause", true, "kernel patch", 9000);
        StatusEngine engine(store);

        WHEN("the agent acknowledges the pause command")
        {
            engine.pause_agent("agent-pause", 2000);

            THEN("the reason and expiry are preserved and the indicators are blue")
            {
                const auto agent = store.get_agent("agent-pause");
                REQUIRE(agent->maintenance);
                REQUIRE(agent->maintenance_reason == "kernel patch");
                REQUIRE(agent->maintenance_until == 9000);
                REQUIRE(store.latest_status_for_indicator("agent-pause", "cpu")->new_status == "blue");
            }
        }
    }

    GIVEN("an agent not in maintenance")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-pause2");
        StatusEngine engine(store);

        WHEN("the agent acknowledges a pause command")
        {
            engine.pause_agent("agent-pause2", 2000);

            THEN("it enters open-ended maintenance with the reason command")
            {
                const auto agent = store.get_agent("agent-pause2");
                REQUIRE(agent->maintenance);
                REQUIRE(agent->maintenance_reason == "command");
                REQUIRE(agent->maintenance_until == 0);
            }
        }
    }
}

SCENARIO("Engine state of agents that no longer exist is dropped by the maintenance tick")
{
    GIVEN("an engine that evaluated two agents")
    {
        SqliteStore store(":memory:");
        for (const auto* id : {"agent-keep", "agent-gone"})
        {
            insert_approved_agent(store, id);
            auto agent = store.get_agent(id);
            agent->collector_config.stale_after_seconds = 10;
            store.set_agent_collector_config(id, agent->collector_config);
        }
        StatusEngine engine(store);
        engine.evaluate_metrics("agent-keep", metrics_with_cpu(50.0), 1000);
        engine.evaluate_metrics("agent-gone", metrics_with_cpu(50.0), 1000);
        REQUIRE(engine.tracked_agent_count() == 2);

        WHEN("one agent is deleted and the tick runs")
        {
            store.delete_agent("agent-gone");
            engine.reconcile_maintenance(2000);

            THEN("only the remaining agent is tracked")
            {
                REQUIRE(engine.tracked_agent_count() == 1);
            }
        }
    }
}

SCENARIO("Expired timed maintenance inside an active window re-enters for the window at once")
{
    GIVEN("an agent in timed manual maintenance until t=3000 and a window from 2000 to 4000")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-overlap");
        StatusEngine engine(store);
        engine.enter_maintenance("agent-overlap", "by hand", 3000, 1000);
        MaintenanceWindowRecord window;
        window.agent_id = "*";
        window.start_ms = 2000;
        window.end_ms = 4000;
        window.reason = "nightly";
        store.create_maintenance_window(window);

        WHEN("the tick runs when the timed maintenance expires")
        {
            engine.reconcile_maintenance(3000);

            THEN("the agent is in scheduled maintenance without a gap")
            {
                const auto agent = store.get_agent("agent-overlap");
                REQUIRE(agent->maintenance);
                REQUIRE(agent->maintenance_reason == "mw:nightly");
            }
        }
    }
}
