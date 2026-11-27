// Behaviour tests for the indicator names the server raises alerts on (#151) and
// for runbook URL validation (#11) and resolution (#53).

#include "../server/indicators.hpp"
#include "../server/runbook.hpp"
#include "../server/status_engine.hpp"
#include "../server/store_sqlite.hpp"
#include "common/metrics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>

using namespace thewatcher;
using namespace thewatcher::server;

namespace
{
bool contains(const std::vector<std::string>& values, const std::string& wanted)
{
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

void insert_agent(SqliteStore& store, const std::string& agent_id, const std::string& runbook_markdown = "")
{
    AgentRecord agent;
    agent.agent_id = agent_id;
    agent.hostname = "host";
    agent.platform = "linux";
    agent.approved = true;
    agent.first_seen = 1;
    agent.last_seen = 1;
    agent.runbook_markdown = runbook_markdown;
    store.upsert_agent(agent);
}

void add_runbook(SqliteStore& store, const std::string& agent_id, const std::string& indicator,
                 const std::string& status, const std::string& url)
{
    RunbookRecord rb;
    rb.agent_id = agent_id;
    rb.indicator = indicator;
    rb.status = status;
    rb.url = url;
    rb.created_at = 1000;
    store.create_runbook(rb);
}
} // namespace

SCENARIO("Indicator names are built and split in one place")
{
    GIVEN("the indicator helpers")
    {
        WHEN("a per-mount and a per-interface indicator are built")
        {
            THEN("they use the kind, a colon and the qualifier")
            {
                REQUIRE(indicators::make(indicators::kDisk, "/") == "disk:/");
                REQUIRE(indicators::make(indicators::kNetwork, "eth0") == "network:eth0");
                REQUIRE(indicators::make(indicators::kProcess, "nginx") == "process:nginx");
            }
        }

        WHEN("the kind is read back from an indicator name")
        {
            THEN("the part before the first colon is returned")
            {
                REQUIRE(indicators::kind_of("disk:C:\\") == "disk");
                REQUIRE(indicators::kind_of("cpu") == "cpu");
                REQUIRE(indicators::kind_of("Heartbeat") == "Heartbeat");
            }
        }
    }
}

SCENARIO("Silence candidates cover the exact name, its kind and legacy spellings")
{
    GIVEN("a per-mount disk indicator")
    {
        WHEN("the silence candidates are listed")
        {
            const auto candidates = indicators::silence_candidates("disk:/");

            THEN("both the exact indicator and the disk kind are candidates")
            {
                REQUIRE(contains(candidates, "disk:/"));
                REQUIRE(contains(candidates, "disk"));
            }
        }
    }

    GIVEN("the offline Heartbeat indicator")
    {
        WHEN("the silence candidates are listed")
        {
            const auto candidates = indicators::silence_candidates("Heartbeat");

            THEN("the lower-case spelling the old UI stored is still a candidate")
            {
                REQUIRE(contains(candidates, "Heartbeat"));
                REQUIRE(contains(candidates, "heartbeat"));
            }
        }
    }

    GIVEN("a process indicator")
    {
        WHEN("the silence candidates are listed")
        {
            const auto candidates = indicators::silence_candidates("process:nginx");

            THEN("the old plural 'processes' spelling is still a candidate")
            {
                REQUIRE(contains(candidates, "process:nginx"));
                REQUIRE(contains(candidates, "process"));
                REQUIRE(contains(candidates, "processes"));
            }
        }
    }
}

SCENARIO("Silence indicator names are canonicalised")
{
    GIVEN("names the old dashboard offered")
    {
        THEN("known kinds map to the spelling the engine uses")
        {
            REQUIRE(indicators::canonical("heartbeat") == "Heartbeat");
            REQUIRE(indicators::canonical("processes") == "process");
            REQUIRE(indicators::canonical("Disk:/") == "disk:/");
            REQUIRE(indicators::canonical("cpu") == "cpu");
            REQUIRE(indicators::canonical("*") == "*");
        }

        THEN("a user-defined log indicator is left alone")
        {
            REQUIRE(indicators::canonical("Auth Failures") == "Auth Failures");
        }
    }
}

SCENARIO("Every indicator the status engine raises belongs to a listed kind")
{
    GIVEN("a store and metrics with CPU, memory, a disk, a network interface and a watched process")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-kinds");
        StatusEngine engine(store);

        auto make_metrics = [](double cpu) {
            SystemMetrics metrics;
            metrics.cpu.usage_percent = cpu;
            metrics.memory.usage_percent = 10.0;
            metrics.hostname = "host";
            metrics.platform = "linux";
            DiskMetrics disk;
            disk.mount_point = "/data";
            disk.usage_percent = 10.0;
            metrics.disks.push_back(disk);
            NetworkMetrics network;
            network.interface_name = "eth0";
            network.is_up = true;
            metrics.networks.push_back(network);
            return metrics;
        };

        WHEN("the metrics are evaluated")
        {
            engine.evaluate_metrics("agent-kinds", make_metrics(10.0), 1000);

            THEN("the kind of every recorded indicator is in indicators::kinds()")
            {
                const auto rows = store.list_status_history("agent-kinds", 100);
                REQUIRE_FALSE(rows.empty());
                for (const auto& row : rows)
                {
                    INFO("indicator: " << row.indicator);
                    REQUIRE(indicators::is_known_kind(indicators::kind_of(row.indicator)));
                }
                // The per-mount and per-interface names carry their qualifier.
                REQUIRE(std::any_of(rows.begin(), rows.end(), [](const auto& r) {
                    return r.indicator == "disk:/data";
                }));
                REQUIRE(std::any_of(rows.begin(), rows.end(), [](const auto& r) {
                    return r.indicator == "network:eth0";
                }));
            }
        }
    }
}

SCENARIO("A silence on an indicator kind suppresses per-mount alerts of that kind")
{
    GIVEN("an agent with a 'disk' silence and a disk that fills up")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-silence");
        StatusEngine engine(store);

        SilenceRecord silence;
        silence.agent_id = "agent-silence";
        silence.indicator = "disk";
        silence.reason = "planned work";
        silence.until_ms = 10'000'000;
        silence.created_by = "admin";
        silence.created_at = 1;
        store.create_silence(silence);

        auto with_disk = [](double usage) {
            SystemMetrics metrics;
            metrics.cpu.usage_percent = 10.0;
            metrics.memory.usage_percent = 10.0;
            DiskMetrics disk;
            disk.mount_point = "/";
            disk.usage_percent = usage;
            metrics.disks.push_back(disk);
            return metrics;
        };

        WHEN("the disk goes critical")
        {
            engine.evaluate_metrics("agent-silence", with_disk(10.0), 1000);
            engine.evaluate_metrics("agent-silence", with_disk(99.0), 2000);

            THEN("the status changes but no alert is raised for it")
            {
                const auto alerts = store.list_alerts(true);
                REQUIRE(std::none_of(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                    return a.indicator == "disk:/";
                }));
                const auto history = store.latest_status_for_indicator("agent-silence", "disk:/");
                REQUIRE(history.has_value());
                REQUIRE(history->new_status == "red");
            }
        }
    }
}

SCENARIO("Runbook URLs accept only http and https")
{
    GIVEN("candidate URLs")
    {
        THEN("web URLs are valid")
        {
            REQUIRE(is_valid_runbook_url("https://wiki.example.com/runbooks/cpu"));
            REQUIRE(is_valid_runbook_url("http://wiki.local:8080/x?y=1#z"));
            REQUIRE(is_valid_runbook_url("HTTPS://Wiki.Example.com/"));
        }

        THEN("script, data, file and relative URLs are rejected")
        {
            REQUIRE_FALSE(is_valid_runbook_url("javascript:alert(1)"));
            REQUIRE_FALSE(is_valid_runbook_url("JaVaScRiPt:alert(1)"));
            REQUIRE_FALSE(is_valid_runbook_url("data:text/html,<script>1</script>"));
            REQUIRE_FALSE(is_valid_runbook_url("file:///etc/passwd"));
            REQUIRE_FALSE(is_valid_runbook_url("ftp://example.com/x"));
            REQUIRE_FALSE(is_valid_runbook_url("//example.com/x"));
            REQUIRE_FALSE(is_valid_runbook_url("/runbooks/cpu"));
            REQUIRE_FALSE(is_valid_runbook_url(""));
        }

        THEN("URLs without a host, with whitespace or control characters, or too long are rejected")
        {
            REQUIRE_FALSE(is_valid_runbook_url("https://"));
            REQUIRE_FALSE(is_valid_runbook_url("https:///path"));
            REQUIRE_FALSE(is_valid_runbook_url(" https://example.com"));
            REQUIRE_FALSE(is_valid_runbook_url("https://exa mple.com"));
            REQUIRE_FALSE(is_valid_runbook_url(std::string("https://example.com/\n")));
            REQUIRE_FALSE(is_valid_runbook_url(std::string("https://example.com/") + std::string(3000, 'a')));
        }
    }
}

SCENARIO("An alert's runbook link follows a documented precedence")
{
    GIVEN("an agent and runbooks at every level of the precedence")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-a");
        add_runbook(store, "", "*", "red", "https://wiki/global-wildcard");
        add_runbook(store, "agent-a", "*", "red", "https://wiki/agent-wildcard");

        WHEN("only the global and agent wildcards exist")
        {
            THEN("the agent's own wildcard wins over the global wildcard")
            {
                REQUIRE(resolve_runbook_url(store, "agent-a", "cpu", "red") == "https://wiki/agent-wildcard");
            }

            THEN("another agent only sees the global wildcard")
            {
                insert_agent(store, "agent-b");
                REQUIRE(resolve_runbook_url(store, "agent-b", "cpu", "red") == "https://wiki/global-wildcard");
            }
        }

        WHEN("a global runbook is added for the indicator kind")
        {
            add_runbook(store, "", "disk", "red", "https://wiki/global-disk");

            THEN("it beats the global wildcard but not the agent's own wildcard")
            {
                REQUIRE(resolve_runbook_url(store, "agent-a", "disk:/", "red") == "https://wiki/agent-wildcard");
                insert_agent(store, "agent-b");
                REQUIRE(resolve_runbook_url(store, "agent-b", "disk:/", "red") == "https://wiki/global-disk");
            }
        }

        WHEN("the agent also has a runbook for the indicator kind and an exact mount")
        {
            add_runbook(store, "agent-a", "disk", "red", "https://wiki/agent-disk");
            add_runbook(store, "agent-a", "disk:/data", "red", "https://wiki/agent-disk-data");

            THEN("exact beats kind, and kind beats wildcard")
            {
                REQUIRE(resolve_runbook_url(store, "agent-a", "disk:/data", "red") == "https://wiki/agent-disk-data");
                REQUIRE(resolve_runbook_url(store, "agent-a", "disk:/", "red") == "https://wiki/agent-disk");
                REQUIRE(resolve_runbook_url(store, "agent-a", "memory", "red") == "https://wiki/agent-wildcard");
            }
        }

        WHEN("the status has no runbook")
        {
            THEN("no link is returned")
            {
                REQUIRE(resolve_runbook_url(store, "agent-a", "cpu", "yellow").empty());
            }
        }
    }

    GIVEN("a stored runbook whose URL is not http(s), as an old database could hold")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-a");
        add_runbook(store, "", "cpu", "red", "javascript:alert(1)");

        THEN("it is never attached to an alert")
        {
            REQUIRE(resolve_runbook_url(store, "agent-a", "cpu", "red").empty());
        }
    }
}

SCENARIO("Alerts carry the per-agent runbook and the matching link")
{
    GIVEN("an agent with a markdown runbook and a global 'disk' link")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-rb", "# Disk full\nClear /var/log");
        add_runbook(store, "", "disk", "red", "https://wiki/disk");
        StatusEngine engine(store);

        auto with_disk = [](double usage) {
            SystemMetrics metrics;
            metrics.cpu.usage_percent = 10.0;
            metrics.memory.usage_percent = 10.0;
            DiskMetrics disk;
            disk.mount_point = "/";
            disk.usage_percent = usage;
            metrics.disks.push_back(disk);
            return metrics;
        };

        WHEN("the disk mount goes critical")
        {
            engine.evaluate_metrics("agent-rb", with_disk(10.0), 1000);
            engine.evaluate_metrics("agent-rb", with_disk(99.0), 2000);

            const auto alerts = store.list_alerts(false);
            const auto it = std::find_if(alerts.begin(), alerts.end(), [](const AlertRecord& a) {
                return a.indicator == "disk:/";
            });
            REQUIRE(it != alerts.end());

            THEN("the stored alert links the kind-level runbook")
            {
                REQUIRE(it->runbook_url == "https://wiki/disk");
            }

            THEN("the webhook payload carries both the link and the agent's markdown runbook")
            {
                const auto payload = build_webhook_payload(store, *it, 7);
                REQUIRE(payload.at("alert_id") == 7);
                REQUIRE(payload.at("indicator") == "disk:/");
                REQUIRE(payload.at("runbook_url") == "https://wiki/disk");
                REQUIRE(payload.at("runbook_markdown") == "# Disk full\nClear /var/log");
            }
        }
    }

    GIVEN("an alert on an agent with no runbook of any kind")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-none");
        AlertRecord alert;
        alert.agent_id = "agent-none";
        alert.indicator = "cpu";

        THEN("the webhook payload has empty runbook fields")
        {
            const auto payload = build_webhook_payload(store, alert, 1);
            REQUIRE(payload.at("runbook_url") == "");
            REQUIRE(payload.at("runbook_markdown") == "");
        }
    }
}
