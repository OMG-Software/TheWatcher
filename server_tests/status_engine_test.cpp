#include "../server/status_engine.hpp"
#include "../server/store_sqlite.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>

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

        WHEN("either status is Blue (maintenance)")
        {
            THEN("is_worse_status always returns false")
            {
                REQUIRE_FALSE(is_worse_status(IndicatorStatus::Blue, IndicatorStatus::Red));
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

SCENARIO("Metrics pruning is triggered by evaluate_metrics after one hour")
{
    GIVEN("an approved agent with old metrics in the store")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-prune");

        // Use timestamps far enough apart that the 30-day cutoff at eval_time
        // falls between old_row and recent_row.
        // eval_time = ~30.0042 days; cutoff = eval_time - 30 days ≈ 364 seconds.
        const int64_t eval_time = 2'600'000'000LL + 3'600'001LL; // ~30 days + ~1 hour in ms
        const int64_t recent_ts = 2'600'000'000LL;               // within retention window
        const int64_t old_ts = 1'000LL;                          // well before cutoff

        MetricsRow old_row;
        old_row.agent_id = "agent-prune";
        old_row.timestamp_ms = old_ts;
        old_row.metrics_cbor = {0x01};
        store.insert_metrics(old_row);

        MetricsRow recent_row;
        recent_row.agent_id = "agent-prune";
        recent_row.timestamp_ms = recent_ts;
        recent_row.metrics_cbor = {0x02};
        store.insert_metrics(recent_row);

        StatusEngine engine(store);

        WHEN("evaluate_metrics is called one hour after last prune with default 30-day retention")
        {
            auto metrics = metrics_with_cpu(10.0);
            engine.evaluate_metrics("agent-prune", metrics, eval_time);

            THEN("the old metrics row is pruned")
            {
                auto rows = store.get_metrics("agent-prune", 100);
                for (const auto& r : rows)
                    REQUIRE(r.timestamp_ms != old_ts);
            }

            AND_THEN("the recent metrics row is retained")
            {
                auto rows = store.get_metrics("agent-prune", 100);
                bool found = false;
                for (const auto& r : rows)
                    if (r.timestamp_ms == recent_ts)
                        found = true;
                REQUIRE(found);
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

SCENARIO("Status engine default network evaluation skips loopback, pseudo and down interfaces")
{
    GIVEN("an approved agent with no explicit network configuration")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-default-net");

        auto make_interface = [](const std::string& name, bool is_up, bool is_loopback) {
            NetworkMetrics network;
            network.interface_name = name;
            network.is_up = is_up;
            network.is_loopback = is_loopback;
            return network;
        };

        auto metrics = metrics_with_cpu(10.0);
        metrics.networks.clear();
        metrics.networks.push_back(make_interface("eth0", true, false));
        metrics.networks.push_back(make_interface("lo", true, true));
        metrics.networks.push_back(make_interface("lo0", true, false));      // BSD loopback, name only
        metrics.networks.push_back(make_interface("weirdloop", true, true)); // loopback by flag only
        metrics.networks.push_back(make_interface("pflog0", false, false));  // BSD pseudo, down
        metrics.networks.push_back(make_interface("enc0", true, false));     // BSD pseudo
        metrics.networks.push_back(make_interface("Loopback Pseudo-Interface 1", true, false));
        metrics.networks.push_back(make_interface("Wi-Fi", false, false)); // disconnected adapter

        WHEN("metrics are evaluated")
        {
            StatusEngine engine(store);
            insert_metrics(store, "agent-default-net", 1000, metrics);
            engine.evaluate_metrics("agent-default-net", metrics, 1000);

            THEN("only the real, up interface gets a status")
            {
                REQUIRE(store.latest_status_for_indicator("agent-default-net", "network:eth0").has_value());
                for (const auto* name :
                     {"lo", "lo0", "weirdloop", "pflog0", "enc0", "Loopback Pseudo-Interface 1", "Wi-Fi"})
                {
                    INFO(name);
                    REQUIRE_FALSE(store.latest_status_for_indicator("agent-default-net", std::string("network:") + name)
                                      .has_value());
                }
            }
        }
    }

    GIVEN("an agent whose configuration explicitly names a down interface")
    {
        SqliteStore store(":memory:");
        insert_approved_agent(store, "agent-explicit-net");
        auto agent = store.get_agent("agent-explicit-net");
        REQUIRE(agent.has_value());
        NetworkInterfaceConfig network_config;
        network_config.interface_name = "eth1";
        agent->collector_config.networks.push_back(network_config);
        store.set_agent_collector_config(agent->agent_id, agent->collector_config);

        auto metrics = metrics_with_cpu(10.0);
        metrics.networks.clear();
        NetworkMetrics eth1;
        eth1.interface_name = "eth1";
        eth1.is_up = false;
        metrics.networks.push_back(eth1);

        WHEN("metrics are evaluated")
        {
            StatusEngine engine(store);
            insert_metrics(store, "agent-explicit-net", 1000, metrics);
            engine.evaluate_metrics("agent-explicit-net", metrics, 1000);

            THEN("the down interface is still reported red")
            {
                auto status = store.latest_status_for_indicator("agent-explicit-net", "network:eth1");
                REQUIRE(status.has_value());
                REQUIRE(status->new_status == "red");
            }
        }
    }
}
