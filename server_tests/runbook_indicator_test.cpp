// Behaviour tests for the indicator names the server raises alerts on (#151) and
// for runbook URL validation (#11) and resolution (#53).

#include "../server/alert_pipeline.hpp"
#include "../server/indicators.hpp"
#include "../server/offline_alerts.hpp"
#include "../server/runbook.hpp"
#include "../server/status_engine.hpp"
#include "../server/store_sqlite.hpp"
#include "common/metrics.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>

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

        THEN("malformed authorities are rejected")
        {
            REQUIRE_FALSE(is_valid_runbook_url("https://:"));
            REQUIRE_FALSE(is_valid_runbook_url("https://@x"));
            REQUIRE_FALSE(is_valid_runbook_url("https://u:p@host"));
            REQUIRE_FALSE(is_valid_runbook_url("https://?bad"));
            REQUIRE_FALSE(is_valid_runbook_url("https://host:0"));
            REQUIRE_FALSE(is_valid_runbook_url("https://host:65536"));
            REQUIRE_FALSE(is_valid_runbook_url("https://host:"));
            REQUIRE_FALSE(is_valid_runbook_url("https://host:80a"));
            REQUIRE_FALSE(is_valid_runbook_url("https://[::1"));
            REQUIRE_FALSE(is_valid_runbook_url("https://[]/x"));
            REQUIRE_FALSE(is_valid_runbook_url("https://ho_st%/x"));
            REQUIRE_FALSE(is_valid_runbook_url("httpx://host/x"));
            REQUIRE_FALSE(is_valid_runbook_url("https:/host/x"));
        }

        THEN("hosts with ports, IPv4 and bracketed IPv6 are valid")
        {
            REQUIRE(is_valid_runbook_url("HTTPS://Host/x"));
            REQUIRE(is_valid_runbook_url("http://wiki.local/p"));
            REQUIRE(is_valid_runbook_url("https://host:65535/x"));
            REQUIRE(is_valid_runbook_url("http://10.0.0.5:8080"));
            REQUIRE(is_valid_runbook_url("https://[::1]:8443/x"));
            REQUIRE(is_valid_runbook_url("https://wiki.example.com?q=1"));
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

    GIVEN("an invalid agent-specific exact URL and a valid global wildcard")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-a");
        add_runbook(store, "agent-a", "cpu", "red", "javascript:alert(1)");
        add_runbook(store, "", "*", "red", "https://wiki/global-wildcard");

        THEN("the invalid higher-precedence URL is skipped and the wildcard is used")
        {
            REQUIRE(resolve_runbook_url(store, "agent-a", "cpu", "red") == "https://wiki/global-wildcard");
        }
    }

    GIVEN("a runbook for a custom indicator named like a prefix of another")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-a");
        add_runbook(store, "", "Auth", "red", "https://wiki/auth");

        THEN("it does not match the alert indicator Auth:Failure")
        {
            REQUIRE(resolve_runbook_url(store, "agent-a", "Auth:Failure", "red").empty());
            REQUIRE(resolve_runbook_url(store, "agent-a", "Auth", "red") == "https://wiki/auth");
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

SCENARIO("Offline and log-match alerts carry the matching runbook link through the pipeline")
{
    GIVEN("an offline agent with runbooks for Heartbeat and for a log indicator")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-x");
        add_runbook(store, "agent-x", "Heartbeat", "red", "https://wiki/offline");
        add_runbook(store, "", "syslog", "amber", "https://wiki/syslog");
        AlertPipeline pipeline(store);

        auto find_alert = [&](const std::string& indicator) -> std::optional<AlertRecord> {
            for (const auto& alert : store.list_alerts(true))
            {
                if (alert.indicator == indicator)
                    return alert;
            }
            return std::nullopt;
        };

        WHEN("the offline scan raises the alert")
        {
            OfflineAlerts offline(store, pipeline);
            REQUIRE(offline.raise_for_offline_agents(5000) == 1);
            const auto it = find_alert("Heartbeat");
            REQUIRE(it.has_value());

            THEN("it is a red Heartbeat alert with the agent's runbook link")
            {
                REQUIRE(it->agent_id == "agent-x");
                REQUIRE(it->old_status == "green");
                REQUIRE(it->new_status == "red");
                REQUIRE(it->created_at == 5000);
                REQUIRE(it->runbook_url == "https://wiki/offline");
            }
        }

        WHEN("a log match on the matching indicator and severity is raised")
        {
            REQUIRE(pipeline.raise(log_match_request("agent-x", "syslog", "amber", "/var/log/syslog", "oops", 6000)) ==
                    AlertOutcome::Raised);
            const auto it = find_alert("syslog");
            REQUIRE(it.has_value());

            THEN("it carries the log runbook link and the message")
            {
                REQUIRE(it->new_status == "amber");
                REQUIRE(it->runbook_url == "https://wiki/syslog");
                REQUIRE(it->message == "Log match in /var/log/syslog: oops");
            }
        }

        WHEN("a log match has a severity no runbook covers")
        {
            pipeline.raise(log_match_request("agent-x", "syslog", "red", "/p", "x", 6000));

            THEN("the link is empty")
            {
                REQUIRE(find_alert("syslog").has_value());
                REQUIRE(find_alert("syslog")->runbook_url.empty());
            }
        }

        WHEN("the matched line is longer than 200 characters")
        {
            const auto request = log_match_request("agent-x", "syslog", "amber", "/p", std::string(500, 'z'), 6000);

            THEN("only the first 200 characters are kept")
            {
                REQUIRE(request.message == "Log match in /p: " + std::string(200, 'z'));
            }
        }
    }

    GIVEN("a runbook whose stored URL is not http(s) and a valid global wildcard")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-y");
        add_runbook(store, "agent-y", "syslog", "amber", "javascript:alert(1)");
        add_runbook(store, "", "*", "amber", "https://wiki/fallback");
        AlertPipeline pipeline(store);

        WHEN("a log match is raised")
        {
            pipeline.raise(log_match_request("agent-y", "syslog", "amber", "/p", "x", 7000));

            THEN("the pipeline skips the invalid URL and links the valid fallback")
            {
                const auto alerts = store.list_alerts(true);
                REQUIRE(alerts.size() == 1);
                REQUIRE(alerts[0].runbook_url == "https://wiki/fallback");
            }
        }
    }

    GIVEN("an agent with a 'disk' kind silence")
    {
        SqliteStore store(":memory:");
        insert_agent(store, "agent-z");
        store.create_silence({0, "agent-z", "disk", "planned", 10'000'000, "admin", 1});
        AlertPipeline pipeline(store);

        THEN("the pipeline's single silence check applies the kind rule to a per-mount alert")
        {
            REQUIRE(pipeline.raise({"agent-z", "disk:/", "green", "red", "full", 5000, false}) ==
                    AlertOutcome::SuppressedSilence);
            REQUIRE(pipeline.raise({"agent-z", "memory", "green", "red", "full", 5000, false}) == AlertOutcome::Raised);
        }
    }
}
