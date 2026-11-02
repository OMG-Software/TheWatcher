#ifdef HAVE_LIBPQ

#include "../server/store_postgres.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>

using namespace thewatcher;
using namespace thewatcher::server;

// ── Helpers ──────────────────────────────────────────────────────────────────

namespace
{

// Returns DSN from environment or a sensible local default.
const char* test_dsn()
{
    const char* env = std::getenv("THEWATCHER_POSTGRES_TEST_DSN");
    if (env && *env)
        return env;
    return "host=localhost dbname=thewatcher_test user=thewatcher_test password=thewatcher_test";
}

// Returns true if a PostgreSQL server is reachable at the test DSN.
// Result is cached after the first probe so subsequent calls are free.
bool postgres_is_available()
{
    static bool checked = false;
    static bool available = false;
    if (!checked)
    {
        checked = true;
        try
        {
            PostgresStore store(test_dsn());
            available = true;
        }
        catch (...)
        {
        }
    }
    return available;
}

AgentRecord make_agent(const std::string& id)
{
    AgentRecord r;
    r.agent_id = id;
    r.hostname = "host-" + id;
    r.platform = "linux";
    r.curve_public_key_z85 = std::string(40, 'A');
    r.approved = false;
    r.rejected = false;
    r.connected = false;
    r.maintenance = false;
    r.collection_interval = 30;
    r.process_limit = 50;
    r.first_seen = 1000;
    r.last_seen = 1000;
    return r;
}

} // namespace

// ── Bootstrap ─────────────────────────────────────────────────────────────────

SCENARIO("[postgres] A freshly opened PostgresStore bootstraps the default admin user")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a connected PostgresStore")
    {
        PostgresStore store(test_dsn());

        THEN("the default admin user exists")
        {
            auto user = store.get_user_by_username("thewatcher");
            REQUIRE(user.has_value());
            CHECK(user->role == "global_admin");
        }

        THEN("list_users returns at least the built-in admin")
        {
            auto users = store.list_users();
            REQUIRE_FALSE(users.empty());
            auto it = std::find_if(users.begin(), users.end(), [](const UserRecord& u) {
                return u.username == "thewatcher";
            });
            REQUIRE(it != users.end());
        }
    }
}

// ── Agent CRUD ────────────────────────────────────────────────────────────────

SCENARIO("[postgres] Agents can be upserted and retrieved")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore and a new agent record")
    {
        PostgresStore store(test_dsn());
        const std::string id = "pg-test-agent-crud";

        // Clean up from any prior run.
        try
        {
            store.delete_agent(id);
        }
        catch (...)
        {
        }

        AgentRecord rec = make_agent(id);

        WHEN("the agent is upserted")
        {
            store.upsert_agent(rec);

            THEN("get_agent returns the record")
            {
                auto got = store.get_agent(id);
                REQUIRE(got.has_value());
                CHECK(got->hostname == rec.hostname);
                CHECK(got->platform == rec.platform);
            }

            THEN("list_agents includes the record")
            {
                auto agents = store.list_agents();
                auto it = std::find_if(agents.begin(), agents.end(), [&](const AgentRecord& a) {
                    return a.agent_id == id;
                });
                REQUIRE(it != agents.end());
            }

            AND_WHEN("the agent is approved")
            {
                store.approve_agent(id);

                THEN("list_approved_agents includes it")
                {
                    auto approved = store.list_approved_agents();
                    auto it = std::find_if(approved.begin(), approved.end(), [&](const AgentRecord& a) {
                        return a.agent_id == id;
                    });
                    REQUIRE(it != approved.end());
                }

                THEN("list_pending_agents does not include it")
                {
                    auto pending = store.list_pending_agents();
                    auto it = std::find_if(pending.begin(), pending.end(), [&](const AgentRecord& a) {
                        return a.agent_id == id;
                    });
                    REQUIRE(it == pending.end());
                }
            }

            AND_WHEN("the agent is deleted")
            {
                store.delete_agent(id);

                THEN("get_agent returns nullopt")
                {
                    REQUIRE_FALSE(store.get_agent(id).has_value());
                }
            }
        }
    }
}

SCENARIO("[postgres] get_agent returns nullopt for an unknown id")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore")
    {
        PostgresStore store(test_dsn());
        THEN("get_agent for a non-existent id returns nullopt")
        {
            REQUIRE_FALSE(store.get_agent("pg-no-such-agent-xyz").has_value());
        }
    }
}

// ── Metrics ───────────────────────────────────────────────────────────────────

SCENARIO("[postgres] Metrics can be inserted and retrieved")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore with a registered agent")
    {
        PostgresStore store(test_dsn());
        const std::string id = "pg-test-agent-metrics";
        try
        {
            store.delete_agent(id);
        }
        catch (...)
        {
        }
        store.upsert_agent(make_agent(id));

        WHEN("a metrics row is inserted")
        {
            MetricsRow row;
            row.agent_id = id;
            row.timestamp_ms = 5000;
            row.metrics_cbor = {1, 2, 3, 4};
            store.insert_metrics(row);

            THEN("get_metrics returns the row")
            {
                auto rows = store.get_metrics(id, 10);
                REQUIRE_FALSE(rows.empty());
                CHECK(rows.front().timestamp_ms == 5000);
                CHECK(rows.front().metrics_cbor == row.metrics_cbor);
            }

            THEN("count_metrics_in_window counts it")
            {
                auto count = store.count_metrics_in_window(id, 4000, 6000);
                CHECK(count == 1);
            }

            THEN("latest_metrics includes this agent")
            {
                auto latest = store.latest_metrics();
                auto it = std::find_if(latest.begin(), latest.end(), [&](const MetricsRow& r) {
                    return r.agent_id == id;
                });
                REQUIRE(it != latest.end());
                CHECK(it->timestamp_ms == 5000);
            }
        }

        store.delete_agent(id);
    }
}

// ── Alerts ────────────────────────────────────────────────────────────────────

SCENARIO("[postgres] Alerts can be inserted, listed, acknowledged, and soft-deleted")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore with a registered agent")
    {
        PostgresStore store(test_dsn());
        const std::string agent_id = "pg-test-agent-alerts";
        try
        {
            store.delete_agent(agent_id);
        }
        catch (...)
        {
        }
        store.upsert_agent(make_agent(agent_id));

        AlertRecord alert;
        alert.agent_id = agent_id;
        alert.indicator = "cpu";
        alert.old_status = "green";
        alert.new_status = "red";
        alert.message = "CPU spike";
        alert.created_at = 10000;

        WHEN("an alert is inserted")
        {
            const int64_t aid = store.insert_alert(alert);
            REQUIRE(aid > 0);

            THEN("list_alerts includes the alert")
            {
                auto alerts = store.list_alerts(false);
                auto it = std::find_if(alerts.begin(), alerts.end(), [aid](const AlertRecord& a) {
                    return a.alert_id == aid;
                });
                REQUIRE(it != alerts.end());
                CHECK(it->indicator == "cpu");
                CHECK(it->new_status == "red");
            }

            THEN("list_unacknowledged_alerts includes the alert")
            {
                auto unacked = store.list_unacknowledged_alerts();
                auto it = std::find_if(unacked.begin(), unacked.end(), [aid](const AlertRecord& a) {
                    return a.alert_id == aid;
                });
                REQUIRE(it != unacked.end());
            }

            AND_WHEN("the alert is acknowledged")
            {
                store.acknowledge_alert(aid, "thewatcher", 20000, "checked");

                THEN("it no longer appears in list_unacknowledged_alerts")
                {
                    auto unacked = store.list_unacknowledged_alerts();
                    auto it = std::find_if(unacked.begin(), unacked.end(), [aid](const AlertRecord& a) {
                        return a.alert_id == aid;
                    });
                    CHECK(it == unacked.end());
                }
            }

            AND_WHEN("the alert is soft-deleted")
            {
                store.soft_delete_alert(aid, 30000);

                THEN("list_alerts(false) excludes it")
                {
                    auto active = store.list_alerts(false);
                    auto it = std::find_if(active.begin(), active.end(), [aid](const AlertRecord& a) {
                        return a.alert_id == aid;
                    });
                    CHECK(it == active.end());
                }

                THEN("list_alerts(true) includes it")
                {
                    auto all = store.list_alerts(true);
                    auto it = std::find_if(all.begin(), all.end(), [aid](const AlertRecord& a) {
                        return a.alert_id == aid;
                    });
                    REQUIRE(it != all.end());
                    CHECK(it->deleted_at == 30000);
                }
            }
        }

        store.delete_agent(agent_id);
    }
}

// ── Runbooks ──────────────────────────────────────────────────────────────────

SCENARIO("[postgres] Runbooks can be created, listed, and deleted")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore")
    {
        PostgresStore store(test_dsn());

        // Remove any runbooks left over from prior runs.
        for (auto& rb : store.list_runbooks())
            if (rb.indicator == "pg-test-cpu")
                store.delete_runbook(rb.runbook_id);

        WHEN("a runbook is created")
        {
            RunbookRecord rec;
            rec.indicator = "pg-test-cpu";
            rec.status = "red";
            rec.url = "https://example.com/runbook";
            rec.notes = "Check disk pressure";
            rec.created_by = "thewatcher";
            rec.created_at = 1000;

            const int64_t rid = store.create_runbook(rec);
            REQUIRE(rid > 0);

            THEN("list_runbooks includes the record")
            {
                auto books = store.list_runbooks();
                auto it = std::find_if(books.begin(), books.end(), [rid](const RunbookRecord& r) {
                    return r.runbook_id == rid;
                });
                REQUIRE(it != books.end());
                CHECK(it->url == rec.url);
                CHECK(it->notes == rec.notes);
            }

            THEN("get_runbook finds it by exact indicator+status")
            {
                auto rb = store.get_runbook("", "pg-test-cpu", "red");
                REQUIRE(rb.has_value());
                CHECK(rb->url == rec.url);
            }

            AND_WHEN("the runbook is deleted")
            {
                store.delete_runbook(rid);

                THEN("list_runbooks no longer includes it")
                {
                    auto books = store.list_runbooks();
                    auto it = std::find_if(books.begin(), books.end(), [rid](const RunbookRecord& r) {
                        return r.runbook_id == rid;
                    });
                    CHECK(it == books.end());
                }

                THEN("get_runbook returns nullopt")
                {
                    CHECK_FALSE(store.get_runbook("", "pg-test-cpu", "red").has_value());
                }
            }
        }
    }
}

SCENARIO("[postgres] get_runbook returns exact indicator match before wildcard")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore with a wildcard and an exact runbook for the same status")
    {
        PostgresStore store(test_dsn());

        for (auto& rb : store.list_runbooks())
            if (rb.indicator == "pg-test-exact" || rb.indicator == "*")
                store.delete_runbook(rb.runbook_id);

        RunbookRecord wildcard;
        wildcard.indicator = "*";
        wildcard.status = "amber";
        wildcard.url = "https://example.com/wildcard";
        wildcard.created_at = 1;
        const int64_t wid = store.create_runbook(wildcard);

        RunbookRecord exact;
        exact.indicator = "pg-test-exact";
        exact.status = "amber";
        exact.url = "https://example.com/exact";
        exact.created_at = 2;
        const int64_t eid = store.create_runbook(exact);

        THEN("get_runbook for the exact indicator returns the exact match")
        {
            auto rb = store.get_runbook("", "pg-test-exact", "amber");
            REQUIRE(rb.has_value());
            CHECK(rb->url == "https://example.com/exact");
        }

        THEN("get_runbook for an unknown indicator returns the wildcard")
        {
            auto rb = store.get_runbook("", "pg-some-other-indicator", "amber");
            REQUIRE(rb.has_value());
            CHECK(rb->url == "https://example.com/wildcard");
        }

        store.delete_runbook(wid);
        store.delete_runbook(eid);
    }
}

// ── Settings ──────────────────────────────────────────────────────────────────

SCENARIO("[postgres] Settings key-value store persists values with fallback")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore")
    {
        PostgresStore store(test_dsn());

        THEN("an unset key returns the fallback")
        {
            auto val = store.get_setting("pg_test_nonexistent_key", "default");
            CHECK(val == "default");
        }

        WHEN("a value is set")
        {
            store.set_setting("pg_test_key", "hello_postgres");

            THEN("it is returned by get_setting")
            {
                auto val = store.get_setting("pg_test_key", "default");
                CHECK(val == "hello_postgres");
            }

            AND_WHEN("it is overwritten")
            {
                store.set_setting("pg_test_key", "updated");
                auto val = store.get_setting("pg_test_key", "default");
                CHECK(val == "updated");
            }
        }
    }
}

// ── Sessions ──────────────────────────────────────────────────────────────────

SCENARIO("[postgres] Session lifecycle: create, retrieve, and expire")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore")
    {
        PostgresStore store(test_dsn());

        SessionRecord session;
        session.token = "pg-test-session-token-xyz";
        session.user_id = 1;
        session.username = "thewatcher";
        session.role = "global_admin";
        session.expires_at = 9999999999LL;

        store.create_session(session);

        WHEN("the token is looked up before expiry")
        {
            auto got = store.get_session(session.token, 1000);
            REQUIRE(got.has_value());
            CHECK(got->username == "thewatcher");
            CHECK(got->role == "global_admin");
        }

        WHEN("the token is looked up after expiry")
        {
            auto got = store.get_session(session.token, 99999999999LL);
            CHECK_FALSE(got.has_value());
        }

        store.delete_session(session.token);
    }
}

// ── Maintenance windows ───────────────────────────────────────────────────────

SCENARIO("[postgres] Maintenance windows can be created, listed, and deleted")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore")
    {
        PostgresStore store(test_dsn());

        MaintenanceWindowRecord rec;
        rec.agent_id = "*";
        rec.start_ms = 1000;
        rec.end_ms = 9000;
        rec.reason = "pg test maintenance";
        rec.created_by = "thewatcher";
        rec.created_at = 500;

        WHEN("a maintenance window is created")
        {
            const int64_t wid = store.create_maintenance_window(rec);
            REQUIRE(wid > 0);

            THEN("list_maintenance_windows includes it")
            {
                auto windows = store.list_maintenance_windows();
                auto it = std::find_if(windows.begin(), windows.end(), [wid](const MaintenanceWindowRecord& w) {
                    return w.window_id == wid;
                });
                REQUIRE(it != windows.end());
                CHECK(it->reason == rec.reason);
            }

            THEN("active_maintenance_windows at time 5000 includes it")
            {
                auto active = store.active_maintenance_windows(5000);
                auto it = std::find_if(active.begin(), active.end(), [wid](const MaintenanceWindowRecord& w) {
                    return w.window_id == wid;
                });
                REQUIRE(it != active.end());
            }

            THEN("active_maintenance_windows at time 10000 excludes it")
            {
                auto active = store.active_maintenance_windows(10000);
                auto it = std::find_if(active.begin(), active.end(), [wid](const MaintenanceWindowRecord& w) {
                    return w.window_id == wid;
                });
                CHECK(it == active.end());
            }

            AND_WHEN("the window is deleted")
            {
                store.delete_maintenance_window(wid);
                auto windows = store.list_maintenance_windows();
                auto it = std::find_if(windows.begin(), windows.end(), [wid](const MaintenanceWindowRecord& w) {
                    return w.window_id == wid;
                });
                CHECK(it == windows.end());
            }
        }
    }
}

// ── Silence rules ─────────────────────────────────────────────────────────────

SCENARIO("[postgres] Silence rules can be created, listed, deleted, and matched")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a PostgresStore")
    {
        PostgresStore store(test_dsn());

        // Remove any leftover silences from prior runs.
        for (auto& s : store.list_silences())
            if (s.agent_id == "pg-silence-agent")
                store.delete_silence(s.silence_id);

        SilenceRecord rec;
        rec.agent_id = "pg-silence-agent";
        rec.indicator = "cpu";
        rec.reason = "pg test silence";
        rec.until_ms = 9999999999LL;
        rec.created_by = "thewatcher";
        rec.created_at = 1000;

        WHEN("a silence is created")
        {
            const int64_t sid = store.create_silence(rec);
            REQUIRE(sid > 0);

            THEN("list_silences includes it")
            {
                auto silences = store.list_silences();
                auto it = std::find_if(silences.begin(), silences.end(), [sid](const SilenceRecord& s) {
                    return s.silence_id == sid;
                });
                REQUIRE(it != silences.end());
            }

            THEN("is_silenced returns true for matching agent+indicator before expiry")
            {
                CHECK(store.is_silenced("pg-silence-agent", "cpu", 5000));
            }

            THEN("is_silenced returns false for non-matching agent")
            {
                CHECK_FALSE(store.is_silenced("other-agent", "cpu", 5000));
            }

            AND_WHEN("the silence is deleted")
            {
                store.delete_silence(sid);
                CHECK_FALSE(store.is_silenced("pg-silence-agent", "cpu", 5000));
            }
        }
    }
}

#endif // HAVE_LIBPQ
