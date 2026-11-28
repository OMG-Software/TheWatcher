#ifdef HAVE_LIBPQ

#include "../server/postgres_raii.hpp"
#include "../server/session_token.hpp"
#include "../server/store_postgres.hpp"
#include "../server/store_synchronized.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <libpq-fe.h>

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

// A conninfo value in single quotes, with backslash and quote escaped as libpq
// expects in keyword/value form.
std::string quote_conninfo_value(std::string_view value)
{
    std::string quoted = "'";
    for (const char c : value)
    {
        if (c == '\\' || c == '\'')
            quoted += '\\';
        quoted += c;
    }
    return quoted + "'";
}

// Returns `dsn` with `extra` added to its server `options` (after any it
// already has). libpq parses the DSN, so keyword/value and URI forms both
// work; the result is always keyword/value. Throws std::invalid_argument if
// libpq cannot parse the DSN.
std::string dsn_with_server_options(const std::string& dsn, const std::string& extra)
{
    char* error = nullptr;
    const std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> parsed(PQconninfoParse(dsn.c_str(), &error),
                                                                              &PQconninfoFree);
    const std::unique_ptr<char, decltype(&PQfreemem)> error_owner(error, &PQfreemem);
    if (!parsed)
        throw std::invalid_argument(std::string("test DSN does not parse: ") +
                                    (error != nullptr ? error : "out of memory"));
    std::string options = extra;
    std::string result;
    // Only settings the DSN names have a value; libpq fills in no defaults.
    for (const PQconninfoOption* option = parsed.get(); option->keyword != nullptr; ++option)
    {
        if (option->val == nullptr)
            continue;
        if (std::string_view(option->keyword) == "options")
        {
            if (*option->val != '\0')
                options = std::string(option->val) + " " + extra;
            continue;
        }
        result += std::string(option->keyword) + "=" + quote_conninfo_value(option->val) + " ";
    }
    return result + "options=" + quote_conninfo_value(options);
}

// Every setting a conninfo string (keyword/value or URI form) names, parsed by
// libpq itself. An unparsable string gives an empty map.
std::map<std::string, std::string> conninfo_settings(const std::string& dsn)
{
    char* error = nullptr;
    const std::unique_ptr<PQconninfoOption, decltype(&PQconninfoFree)> parsed(PQconninfoParse(dsn.c_str(), &error),
                                                                              &PQconninfoFree);
    const std::unique_ptr<char, decltype(&PQfreemem)> error_owner(error, &PQfreemem);
    std::map<std::string, std::string> settings;
    if (!parsed)
        return settings;
    for (const PQconninfoOption* option = parsed.get(); option->keyword != nullptr; ++option)
        if (option->val != nullptr)
            settings[option->keyword] = option->val;
    return settings;
}

// A clock that always reads `ms`.
thewatcher::server::WallClock fixed_clock(int64_t ms)
{
    return [ms] {
        return ms;
    };
}

// A clock that reads `first` once and `later` on every read after that, so a
// store that reads the clock only once never sees `later`. The flag is shared
// so copies of the clock agree on whether it has been read.
thewatcher::server::WallClock clock_reaching(int64_t first, int64_t later)
{
    auto read_before = std::make_shared<bool>(false);
    return [first, later, read_before] {
        if (*read_before)
            return later;
        *read_before = true;
        return first;
    };
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
        // A plain connection: only "no server reachable" skips the suite. Once
        // connected, a store-initialisation or migration error in a test fails it.
        PGconn* conn = PQconnectdb(test_dsn());
        available = PQstatus(conn) == CONNECTION_OK;
        PQfinish(conn);
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

// ── Atomicity and concurrency (#72, #73, #79, #51) ───────────────────────────

namespace
{
// Removes the agent and the groups a test created so reruns start clean.
void cleanup_pg(PostgresStore& store, const std::string& agent_id)
{
    try
    {
        store.delete_agent(agent_id);
    }
    catch (...)
    {
    }
}
} // namespace

SCENARIO("[postgres] A failed group assignment leaves the previous memberships intact")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("an agent in group A")
    {
        PostgresStore store(test_dsn());
        const std::string id = "pg-atomic-groups";
        cleanup_pg(store, id);
        store.upsert_agent(make_agent(id));
        const auto group_a = store.create_group("pg-atomic-A");
        store.set_agent_groups(id, {group_a});

        WHEN("an assignment names a group that does not exist")
        {
            THEN("it throws and the agent is still in A")
            {
                REQUIRE_THROWS(store.set_agent_groups(id, {group_a, 999999999}));
                REQUIRE(store.get_agent_groups(id) == std::vector<int64_t>{group_a});
            }
        }

        WHEN("the configure-modal save names a group that does not exist")
        {
            AgentConfigUpdate update;
            update.collection_interval = 77;
            update.heartbeat_interval = 9;
            update.process_limit = 3;
            update.collector_config = default_collector_config();
            update.group_ids = std::vector<int64_t>{999999999};

            THEN("it throws and no part is stored")
            {
                REQUIRE_THROWS(store.save_agent_configuration(id, update));
                const auto rec = store.get_agent(id);
                CHECK(rec->collection_interval == 30);
                CHECK(rec->heartbeat_interval == 5);
                CHECK(store.get_agent_groups(id) == std::vector<int64_t>{group_a});
            }
        }
        cleanup_pg(store, id);
    }
}

SCENARIO("[postgres] A heartbeat touch does not revert saved settings")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("an agent with a saved heartbeat interval and runbook")
    {
        PostgresStore store(test_dsn());
        const std::string id = "pg-touch";
        cleanup_pg(store, id);
        store.upsert_agent(make_agent(id));
        store.set_agent_heartbeat_interval(id, 17);
        store.set_agent_runbook(id, "# rb");

        WHEN("a heartbeat touches the agent")
        {
            store.touch_agent(id, 9999, true);

            THEN("only last_seen and connected change")
            {
                const auto rec = store.get_agent(id);
                CHECK(rec->last_seen == 9999);
                CHECK(rec->connected);
                CHECK(rec->heartbeat_interval == 17);
                CHECK(rec->runbook_markdown == "# rb");
            }
        }
        cleanup_pg(store, id);
    }
}

SCENARIO("[postgres] A dead-agent heartbeat alert is created once and bulk alert changes are atomic")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("an agent with no alerts")
    {
        PostgresStore store(test_dsn());
        const std::string id = "pg-heartbeat-alert";
        cleanup_pg(store, id);
        store.upsert_agent(make_agent(id));
        AlertRecord alert;
        alert.agent_id = id;
        alert.indicator = "Heartbeat";
        alert.old_status = "green";
        alert.new_status = "red";
        alert.created_at = 2000;

        WHEN("the heartbeat alert is inserted twice")
        {
            const bool first = store.insert_heartbeat_alert_if_absent(alert);
            const bool second = store.insert_heartbeat_alert_if_absent(alert);

            THEN("only the first creates a row")
            {
                CHECK(first);
                CHECK_FALSE(second);
            }
        }

        WHEN("alerts are acknowledged in bulk")
        {
            std::vector<int64_t> ids;
            for (int i = 0; i < 3; ++i)
            {
                alert.indicator = "cpu";
                ids.push_back(store.insert_alert(alert));
            }
            store.bulk_acknowledge_alerts(ids, "bob", 5000);

            THEN("all are acknowledged")
            {
                for (const auto& a : store.list_alerts(false))
                    if (a.agent_id == id)
                        CHECK(a.acknowledged_at == 5000);
            }
        }
        cleanup_pg(store, id);
    }
}

SCENARIO("[postgres] Eight threads can use one synchronized store at the same time")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a shared store with an agent and two groups")
    {
        SynchronizedStore store(std::make_unique<PostgresStore>(test_dsn()));
        const std::string id = "pg-concurrent";
        store.delete_agent(id);
        store.upsert_agent(make_agent(id));
        const auto group_a = store.create_group("pg-conc-A");
        const auto group_b = store.create_group("pg-conc-B");
        std::atomic<int> errors{0};

        WHEN("threads mix transactional and plain calls")
        {
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; ++t)
            {
                threads.emplace_back([&, t] {
                    for (int i = 0; i < 25; ++i)
                    {
                        try
                        {
                            if ((t + i) % 3 == 0)
                                store.set_agent_groups(id, (i % 2) ? std::vector<int64_t>{group_a}
                                                                   : std::vector<int64_t>{group_a, group_b});
                            else if ((t + i) % 3 == 1)
                                store.touch_agent(id, i, true);
                            else
                                (void)store.get_agent(id);
                        }
                        catch (...)
                        {
                            ++errors;
                        }
                    }
                });
            }
            for (auto& th : threads)
                th.join();

            THEN("no call fails and the final state is consistent")
            {
                REQUIRE(errors == 0);
                const auto groups = store.get_agent_groups(id);
                REQUIRE((groups == std::vector<int64_t>{group_a} || groups == std::vector<int64_t>{group_a, group_b}));
            }
        }
        store.delete_agent(id);
    }
}

SCENARIO("[postgres] Two server nodes racing to raise a dead-agent alert create exactly one")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("two connections to one database and several offline agents")
    {
        PostgresStore node_a(test_dsn());
        PostgresStore node_b(test_dsn());
        std::vector<std::string> ids;
        for (int i = 0; i < 20; ++i)
        {
            ids.push_back("pg-race-" + std::to_string(i));
            cleanup_pg(node_a, ids.back());
            node_a.upsert_agent(make_agent(ids.back()));
        }

        WHEN("both nodes insert the heartbeat alert for every agent at the same time")
        {
            std::atomic<bool> go{false};
            std::atomic<int> errors{0};
            std::atomic<int> inserted{0};
            auto race = [&](PostgresStore& node) {
                while (!go.load())
                    std::this_thread::yield();
                for (const auto& id : ids)
                {
                    try
                    {
                        AlertRecord alert;
                        alert.agent_id = id;
                        alert.indicator = "Heartbeat";
                        alert.old_status = "green";
                        alert.new_status = "red";
                        alert.created_at = 2000;
                        if (node.insert_heartbeat_alert_if_absent(alert))
                            ++inserted;
                    }
                    catch (...)
                    {
                        ++errors;
                    }
                }
            };
            std::thread a(race, std::ref(node_a));
            std::thread b(race, std::ref(node_b));
            go = true;
            a.join();
            b.join();

            THEN("each agent has exactly one live heartbeat alert")
            {
                CHECK(errors == 0);
                CHECK(inserted == static_cast<int>(ids.size()));
                for (const auto& id : ids)
                {
                    int live = 0;
                    for (const auto& alert : node_a.list_alerts(false))
                        if (alert.agent_id == id && alert.indicator == "Heartbeat")
                            ++live;
                    CHECK(live == 1);
                }
            }
        }
        for (const auto& id : ids)
            cleanup_pg(node_a, id);
    }
}

// ── #145: sessions and credentials ───────────────────────────────────────────

namespace
{

// Closing the connection rolls back anything still open, so a failed
// assertion cannot leave a row lock behind that blocks the worker thread.
struct PgSideConnection
{
    PgSideConnection()
        : conn(PQconnectdb(test_dsn()))
    {
    }
    PgSideConnection(const PgSideConnection&) = delete;
    PgSideConnection& operator=(const PgSideConnection&) = delete;
    ~PgSideConnection()
    {
        PQfinish(conn);
    }
    void run(const std::string& sql)
    {
        PGresult* res = PQexec(conn, sql.c_str());
        const auto status = PQresultStatus(res);
        PQclear(res);
        REQUIRE((status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK));
    }
    PGconn* conn;
};

// Runs one call on its own thread and always joins it on scope exit. Unlike
// std::async, destroying a call that is still blocked does not hang in a
// future destructor, so a hung call under test fails the checks instead.
// Declare it before any connection holding the lock it waits on, so that
// connection is closed first and the call can finish.
template <typename T>
class BackgroundCall
{
public:
    BackgroundCall() = default;
    BackgroundCall(const BackgroundCall&) = delete;
    BackgroundCall& operator=(const BackgroundCall&) = delete;
    ~BackgroundCall()
    {
        if (thread_.joinable())
            thread_.join();
    }

    template <typename Fn>
    void start(Fn fn)
    {
        promise_ = std::promise<T>();
        future_ = promise_.get_future();
        thread_ = std::thread([this, fn = std::move(fn)]() mutable {
            try
            {
                promise_.set_value(fn());
            }
            catch (...)
            {
                promise_.set_exception(std::current_exception());
            }
        });
    }
    // True while the call has not returned yet.
    bool running() const
    {
        return future_.wait_for(std::chrono::seconds(0)) == std::future_status::timeout;
    }
    T get()
    {
        T value = future_.get();
        thread_.join();
        return value;
    }

private:
    std::promise<T> promise_;
    std::future<T> future_;
    std::thread thread_;
};

// Polls (bounded) until the backend `waiter_pid` (the store connection the
// call under test runs on) is waiting on a lock that the fixture connection
// `holder_pid` holds, using a third connection. A lock wait anywhere else in
// the database does not count. Returns false if that pair never appeared, so a
// call that finished without blocking on the fixture's lock fails the overlap
// case.
bool wait_until_blocked_by(PGconn* observer, int waiter_pid, int holder_pid)
{
    const auto sql =
        "SELECT " + std::to_string(holder_pid) + " = ANY(pg_blocking_pids(" + std::to_string(waiter_pid) + "))";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        PGresult* res = PQexec(observer, sql.c_str());
        const bool blocked = PQresultStatus(res) == PGRES_TUPLES_OK && std::string(PQgetvalue(res, 0, 0)) == "t";
        PQclear(res);
        if (blocked)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

} // namespace

SCENARIO("[postgres] A login waits for a concurrent credential change instead of creating a session")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user whose password or enabled flag is being changed in an open transaction")
    {
        PostgresStore store(test_dsn());
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto old_hash = "old-hash-" + suffix;
        const auto user_id = store.create_user("race-" + suffix, old_hash, "global_viewer");
        SessionRecord session;
        session.token = "race-token-" + suffix;
        session.user_id = user_id;
        session.created_at = 1;
        session.expires_at = 4'000'000'000'000LL;

        // Declared before the side connection so that connection closes (and
        // releases its lock) first if an assertion fails.
        BackgroundCall<bool> login;
        PgSideConnection side;
        PgSideConnection observer;
        REQUIRE(PQstatus(side.conn) == CONNECTION_OK);
        REQUIRE(PQstatus(observer.conn) == CONNECTION_OK);
        // Read before the login thread starts using the store's connection.
        const int waiter_pid = store.backend_pid();
        const int holder_pid = PQbackendPID(side.conn);
        side.run("BEGIN");

        WHEN("the password is changed and not yet committed")
        {
            side.run("UPDATE users SET password_hash='new-hash-" + suffix +
                     "' WHERE user_id=" + std::to_string(user_id));
            login.start([&] {
                return store.create_session_if_credentials_current(session, old_hash);
            });
            const bool blocked = wait_until_blocked_by(observer.conn, waiter_pid, holder_pid) && login.running();
            side.run("COMMIT");
            const bool created = login.get();

            THEN("the login waited for the change and then created no session")
            {
                CHECK(blocked);
                CHECK_FALSE(created);
                CHECK(store.delete_sessions_for_user(user_id) == 0);
            }
        }

        WHEN("the user is disabled and not yet committed")
        {
            side.run("UPDATE users SET disabled=TRUE WHERE user_id=" + std::to_string(user_id));
            login.start([&] {
                return store.create_session_if_credentials_current(session, old_hash);
            });
            const bool blocked = wait_until_blocked_by(observer.conn, waiter_pid, holder_pid) && login.running();
            side.run("COMMIT");
            const bool created = login.get();

            THEN("the login waited for the change and then created no session")
            {
                CHECK(blocked);
                CHECK_FALSE(created);
                CHECK(store.delete_sessions_for_user(user_id) == 0);
            }
        }
        side.run("ROLLBACK");
        store.delete_user(user_id);
    }
}

SCENARIO("[postgres] Password change and disable report a missing user and revoke sessions")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user with a session")
    {
        PostgresStore store(test_dsn());
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto user_id = store.create_user("rev-" + suffix, "hash-" + suffix, "global_viewer");
        SessionRecord session;
        session.token = "rev-token-" + suffix;
        session.user_id = user_id;
        session.created_at = 1;
        session.expires_at = 4'000'000'000'000LL;
        store.create_session(session);

        WHEN("the password is replaced")
        {
            const auto revoked = store.update_user_password_and_revoke_sessions(user_id, "new-" + suffix);

            THEN("the session is revoked and the hash changed")
            {
                REQUIRE(revoked.has_value());
                CHECK(*revoked == 1);
                CHECK_FALSE(store.get_session(session.token, 1));
                CHECK(store.get_user_by_username("rev-" + suffix)->password_hash == "new-" + suffix);
            }
        }

        WHEN("the password of an unknown user is replaced")
        {
            THEN("no user is reported as updated")
            {
                CHECK_FALSE(store.update_user_password_and_revoke_sessions(-1, "x").has_value());
            }
        }

        WHEN("the user is disabled")
        {
            const auto affected = store.disable_user_and_revoke_sessions(user_id);

            THEN("the user is disabled and the session is gone")
            {
                CHECK(affected == 1);
                CHECK_FALSE(store.get_session(session.token, 1));
            }
        }
        store.delete_user(user_id);
    }
}

SCENARIO("[postgres] A password reset or disable revokes a session a login committed first")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user, one connection for logins and another for credential changes")
    {
        PostgresStore login_store(test_dsn());
        PostgresStore change_store(test_dsn());
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const auto old_hash = "old-hash-" + suffix;
        const auto user_id = change_store.create_user("order-" + suffix, old_hash, "global_viewer");
        const auto uid = std::to_string(user_id);
        SessionRecord session;
        session.token = "order-token-" + suffix;
        session.user_id = user_id;
        session.created_at = 1;
        session.expires_at = 4'000'000'000'000LL;

        // Declared before the side connection so that connection closes (and
        // releases its locks) first if an assertion fails.
        BackgroundCall<std::optional<int>> reset;
        BackgroundCall<int> disable;
        PgSideConnection login_tx;
        PgSideConnection observer;
        REQUIRE(PQstatus(login_tx.conn) == CONNECTION_OK);
        REQUIRE(PQstatus(observer.conn) == CONNECTION_OK);
        // Read before a change thread starts using the store's connection.
        const int waiter_pid = change_store.backend_pid();
        const int holder_pid = PQbackendPID(login_tx.conn);
        // The same lock-then-insert a login performs, but left open so the
        // change can start while the login is still in flight.
        const std::string held_login = "WITH u AS (SELECT user_id FROM users WHERE user_id=" + uid +
                                       " AND password_hash='" + old_hash +
                                       "' AND disabled=FALSE FOR SHARE) "
                                       "INSERT INTO sessions(token,user_id,created_at,expires_at) SELECT '" +
                                       session.token + "',user_id,1,4000000000000 FROM u";

        WHEN("the login commits and the password is reset afterwards")
        {
            REQUIRE(login_store.create_session_if_credentials_current(session, old_hash));
            const auto revoked = change_store.update_user_password_and_revoke_sessions(user_id, "new-" + suffix);

            THEN("the session is revoked")
            {
                REQUIRE(revoked.has_value());
                CHECK(*revoked == 1);
                CHECK(change_store.delete_sessions_for_user(user_id) == 0);
            }
        }

        WHEN("the login commits and the user is disabled afterwards")
        {
            REQUIRE(login_store.create_session_if_credentials_current(session, old_hash));
            const auto affected = change_store.disable_user_and_revoke_sessions(user_id);

            THEN("the session is revoked")
            {
                CHECK(affected == 1);
                CHECK(change_store.delete_sessions_for_user(user_id) == 0);
            }
        }

        WHEN("the password reset starts while the login still holds the row lock and commits later")
        {
            login_tx.run("BEGIN");
            login_tx.run(held_login);
            reset.start([&] {
                return change_store.update_user_password_and_revoke_sessions(user_id, "new-" + suffix);
            });
            const bool blocked = wait_until_blocked_by(observer.conn, waiter_pid, holder_pid) && reset.running();
            login_tx.run("COMMIT");
            const auto revoked = reset.get();

            THEN("the reset waited for the login and still deletes the session it committed")
            {
                CHECK(blocked);
                REQUIRE(revoked.has_value());
                CHECK(*revoked == 1);
                CHECK(change_store.delete_sessions_for_user(user_id) == 0);
                CHECK(change_store.get_user_by_username("order-" + suffix)->password_hash == "new-" + suffix);
            }
        }

        WHEN("the disable starts while the login still holds the row lock and commits later")
        {
            login_tx.run("BEGIN");
            login_tx.run(held_login);
            disable.start([&] {
                return change_store.disable_user_and_revoke_sessions(user_id);
            });
            const bool blocked = wait_until_blocked_by(observer.conn, waiter_pid, holder_pid) && disable.running();
            login_tx.run("COMMIT");
            const auto affected = disable.get();

            THEN("the disable waited for the login and still deletes the session it committed")
            {
                CHECK(blocked);
                CHECK(affected == 1);
                CHECK(change_store.delete_sessions_for_user(user_id) == 0);
            }
        }
        login_tx.run("ROLLBACK");
        change_store.delete_user(user_id);
    }
}

// ── Session tokens at rest (#145) ─────────────────────────────────────────────

namespace
{
// Reads the user's session tokens straight from the table through a side
// connection, so the test sees what is on disk rather than what the store
// hands back.
std::vector<std::string> stored_session_tokens(int64_t user_id)
{
    PgSideConnection side;
    REQUIRE(PQstatus(side.conn) == CONNECTION_OK);
    const auto sql = "SELECT token FROM sessions WHERE user_id=" + std::to_string(user_id) + " ORDER BY token";
    PGresult* res = PQexec(side.conn, sql.c_str());
    REQUIRE(PQresultStatus(res) == PGRES_TUPLES_OK);
    std::vector<std::string> tokens;
    for (int row = 0; row < PQntuples(res); ++row)
        tokens.emplace_back(PQgetvalue(res, row, 0));
    PQclear(res);
    return tokens;
}

SessionRecord make_pg_session(int64_t user_id, const std::string& token, int64_t expires_at = 4'000'000'000'000LL)
{
    SessionRecord session;
    session.token = token;
    session.user_id = user_id;
    session.created_at = 1;
    session.expires_at = expires_at;
    return session;
}

std::string unique_suffix()
{
    return std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}
} // namespace

SCENARIO("[postgres] Sessions are stored as SHA-256 hashes of their tokens")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user with a session created from a raw token")
    {
        PostgresStore store(test_dsn());
        const auto suffix = unique_suffix();
        const auto user_id = store.create_user("hash-" + suffix, "hash-of-password", "global_viewer");
        const auto first = "raw-first-" + suffix;
        const auto second = "raw-second-" + suffix;
        store.create_session(make_pg_session(user_id, first));

        THEN("the table holds the hex hash and no row equals the raw token")
        {
            const auto rows = stored_session_tokens(user_id);
            REQUIRE(rows.size() == 1);
            CHECK(rows[0] == hash_session_token(first));
            CHECK(rows[0] != first);
        }

        WHEN("the session is looked up with the raw token")
        {
            const auto found = store.get_session(first, 5000);

            THEN("the record carries the raw token, so passing it back never hashes twice")
            {
                REQUIRE(found.has_value());
                CHECK(found->token == first);
                CHECK(store.get_session(found->token, 5000).has_value());
                CHECK_FALSE(store.get_session(hash_session_token(first), 5000).has_value());
            }
        }

        WHEN("the session is deleted by its raw token")
        {
            store.delete_session(first);

            THEN("it is gone")
            {
                CHECK_FALSE(store.get_session(first, 5000).has_value());
                CHECK(stored_session_tokens(user_id).empty());
            }
        }

        WHEN("the user's other sessions are revoked while keeping the first")
        {
            store.create_session(make_pg_session(user_id, second));
            const auto revoked = store.delete_sessions_for_user(user_id, first);

            THEN("only the kept session survives")
            {
                CHECK(revoked == 1);
                CHECK(store.get_session(first, 5000).has_value());
                CHECK_FALSE(store.get_session(second, 5000).has_value());
            }
        }

        WHEN("a password change keeps the caller's raw token")
        {
            store.create_session(make_pg_session(user_id, second));
            const auto revoked = store.update_user_password_and_revoke_sessions(user_id, "new-hash-" + suffix, second);

            THEN("that session survives and the other is revoked")
            {
                REQUIRE(revoked.has_value());
                CHECK(*revoked == 1);
                CHECK(store.get_session(second, 5000).has_value());
                CHECK_FALSE(store.get_session(first, 5000).has_value());
            }
        }

        WHEN("every session of the user is revoked with no kept token")
        {
            store.create_session(make_pg_session(user_id, second));

            THEN("none is left")
            {
                CHECK(store.delete_sessions_for_user(user_id) == 2);
                CHECK(stored_session_tokens(user_id).empty());
            }
        }

        WHEN("an expired session is purged")
        {
            store.create_session(make_pg_session(user_id, second, 2000));
            store.delete_expired_sessions(5000);

            THEN("only the expired row is removed")
            {
                CHECK(stored_session_tokens(user_id) == std::vector<std::string>{hash_session_token(first)});
            }
        }

        store.delete_user(user_id);
    }
}

SCENARIO("[postgres] A session created after a credentials check is also stored hashed")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user and the password hash a login just verified")
    {
        PostgresStore store(test_dsn());
        const auto suffix = unique_suffix();
        const auto password_hash = "login-hash-" + suffix;
        const auto user_id = store.create_user("login-" + suffix, password_hash, "global_viewer");
        const auto token = "login-token-" + suffix;

        WHEN("create_session_if_credentials_current succeeds")
        {
            REQUIRE(store.create_session_if_credentials_current(make_pg_session(user_id, token), password_hash));

            THEN("the table holds only the hash and the raw token still looks up")
            {
                CHECK(stored_session_tokens(user_id) == std::vector<std::string>{hash_session_token(token)});
                const auto found = store.get_session(token, 5000);
                REQUIRE(found.has_value());
                CHECK(found->token == token);
            }
        }

        store.delete_user(user_id);
    }
}

SCENARIO("[postgres] Upgrading clears sessions stored as raw tokens, exactly once")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a database from before token hashing holding a raw-token session")
    {
        const auto suffix = unique_suffix();
        int64_t user_id = 0;
        {
            PostgresStore store(test_dsn());
            user_id = store.create_user("migrate-" + suffix, "hash-of-password", "global_viewer");
        }
        // A pre-upgrade database has no migration marker and raw token rows.
        {
            PgSideConnection side;
            REQUIRE(PQstatus(side.conn) == CONNECTION_OK);
            side.run(std::string("DELETE FROM server_settings WHERE key='") + session_tokens_hashed_setting + "'");
            side.run("INSERT INTO sessions(token,user_id,created_at,expires_at) VALUES('legacy-raw-" + suffix + "'," +
                     std::to_string(user_id) + ",1,4000000000000)");
        }
        REQUIRE(stored_session_tokens(user_id) == std::vector<std::string>{"legacy-raw-" + suffix});

        WHEN("the upgraded server opens it")
        {
            PostgresStore upgraded(test_dsn());

            THEN("the old session is gone and its raw token no longer authenticates")
            {
                CHECK(stored_session_tokens(user_id).empty());
                CHECK_FALSE(upgraded.get_session("legacy-raw-" + suffix, 5000).has_value());
            }

            AND_WHEN("a new session is created and the server is restarted")
            {
                upgraded.create_session(make_pg_session(user_id, "fresh-" + suffix));
                PostgresStore restarted(test_dsn());

                THEN("the migration does not run again and the new session survives")
                {
                    CHECK(restarted.get_session("fresh-" + suffix, 5000).has_value());
                    CHECK(stored_session_tokens(user_id) ==
                          std::vector<std::string>{hash_session_token("fresh-" + suffix)});
                }
            }
        }

        PostgresStore cleanup(test_dsn());
        cleanup.delete_user(user_id);
    }
}

// ── Concurrent membership replacement ─────────────────────────────────────────

namespace
{
// Releases `parties` threads at the same moment so two replacements really
// start together.
class StartGate
{
public:
    explicit StartGate(int parties)
        : remaining_(parties)
    {
    }
    void arrive_and_wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (--remaining_ == 0)
            cv_.notify_all();
        else
            cv_.wait(lock, [this] {
                return remaining_ == 0;
            });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int remaining_;
};
} // namespace

SCENARIO("[postgres] Two connections replacing an empty membership set never end with the union")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("an agent and a user with no groups, and two connections")
    {
        PostgresStore first(test_dsn());
        PostgresStore second(test_dsn());
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        const std::string agent_id = "pg-union-" + suffix;
        first.upsert_agent(make_agent(agent_id));
        const auto user_id = first.create_user("union-" + suffix, "hash", "global_viewer");
        const auto group_a = first.create_group("union-A-" + suffix);
        const auto group_b = first.create_group("union-B-" + suffix);

        WHEN("each connection replaces the memberships with a different set, many times in lock step")
        {
            int unions = 0;
            std::atomic<int> errors{0};
            for (int round = 0; round < 150; ++round)
            {
                first.set_agent_groups(agent_id, {});
                first.set_user_groups(user_id, {});
                StartGate gate(2);
                const auto run = [&](PostgresStore& store, int64_t group) {
                    try
                    {
                        gate.arrive_and_wait();
                        store.set_agent_groups(agent_id, {group});
                        store.set_user_groups(user_id, {group});
                    }
                    catch (...)
                    {
                        ++errors;
                    }
                };
                std::thread t1(run, std::ref(first), group_a);
                std::thread t2(run, std::ref(second), group_b);
                t1.join();
                t2.join();
                if (first.get_agent_groups(agent_id).size() != 1 || first.get_user_groups(user_id).size() != 1)
                    ++unions;
            }

            THEN("every round ends with exactly one of the requested sets")
            {
                CHECK(errors == 0);
                CHECK(unions == 0);
            }
        }
        first.delete_agent(agent_id);
        first.delete_user(user_id);
    }
}

// ── Self-service password change and enable (#145) ───────────────────────────

SCENARIO("[postgres] A self-service change applies only to the hash that was verified")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user with two sessions")
    {
        PostgresStore store(test_dsn());
        const auto suffix = unique_suffix();
        const auto old_hash = "old-" + suffix;
        const auto user_id = store.create_user("own-" + suffix, old_hash, "global_viewer");
        const auto keep = "own-keep-" + suffix;
        const auto other = "own-other-" + suffix;
        store.create_session(make_pg_session(user_id, keep));
        store.create_session(make_pg_session(user_id, other));
        using Status = PasswordChangeResult::Status;

        WHEN("the change names the stored hash")
        {
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, fixed_clock(5000));

            THEN("the hash changes and only the other session is revoked")
            {
                CHECK(result.status == Status::Changed);
                CHECK(result.revoked == 1);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == "new-" + suffix);
                CHECK(store.get_session(keep, 5000).has_value());
                CHECK_FALSE(store.get_session(other, 5000).has_value());
            }
        }

        WHEN("an admin reset replaced the hash after it was verified")
        {
            store.update_user_password(user_id, "reset-" + suffix);
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, fixed_clock(5000));

            THEN("the change is refused and nothing is revoked")
            {
                CHECK(result.status == Status::PasswordChanged);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == "reset-" + suffix);
                CHECK(store.get_session(keep, 5000).has_value());
                CHECK(store.get_session(other, 5000).has_value());
            }
        }

        WHEN("the user was disabled, and their sessions revoked, after the password was verified")
        {
            REQUIRE(store.disable_user_and_revoke_sessions(user_id) == 1);
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, fixed_clock(5000));

            THEN("nothing changes and the store reports that the session ended")
            {
                CHECK(result.status == Status::SessionEnded);
                CHECK(result.revoked == 0);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == old_hash);
            }
        }

        WHEN("the user was disabled and enabled again after the password was verified")
        {
            REQUIRE(store.disable_user_and_revoke_sessions(user_id) == 1);
            REQUIRE(store.enable_user_and_revoke_sessions(user_id).has_value());
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, fixed_clock(5000));

            THEN("nothing changes: the session that verified the password is gone")
            {
                CHECK(result.status == Status::SessionEnded);
                CHECK(result.revoked == 0);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == old_hash);
            }
        }

        WHEN("the user was disabled without their sessions being revoked")
        {
            REQUIRE(store.disable_user(user_id) == 1);
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, fixed_clock(5000));

            THEN("nothing changes or is revoked while the user is disabled")
            {
                CHECK(result.status == Status::SessionEnded);
                CHECK(result.revoked == 0);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == old_hash);
                // Re-enabled without a revoke, both sessions are still there.
                REQUIRE(store.enable_user(user_id) == 1);
                CHECK(store.get_session(keep, 5000).has_value());
                CHECK(store.get_session(other, 5000).has_value());
            }
        }

        WHEN("the caller's session ended after the password was verified")
        {
            store.delete_session(keep);
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, fixed_clock(5000));

            THEN("nothing changes and the user's other session is kept")
            {
                CHECK(result.status == Status::SessionEnded);
                CHECK(result.revoked == 0);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == old_hash);
                CHECK(store.get_session(other, 5000).has_value());
            }
        }

        WHEN("the user does not exist")
        {
            THEN("the store reports it")
            {
                CHECK(store.update_user_password_if_current_and_revoke_sessions(-1, "x", "y", keep, fixed_clock(5000))
                          .status == Status::UserNotFound);
            }
        }

        WHEN("the caller's session expired after the password was verified, before it was purged")
        {
            // make_pg_session expires at 4'000'000'000'000; at that instant the
            // session is no longer valid, although its row is still there.
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, fixed_clock(4'000'000'000'000LL));

            THEN("nothing changes and the store reports that the session ended")
            {
                CHECK(result.status == Status::SessionEnded);
                CHECK(result.revoked == 0);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == old_hash);
                CHECK(store.get_session(other, 5000).has_value());
            }
        }

        WHEN("the caller's session expires after the change first reads the clock, before it commits")
        {
            // The first reading is the last instant the session is valid; every
            // later reading is the expiry itself.
            const auto result = store.update_user_password_if_current_and_revoke_sessions(
                user_id, old_hash, "new-" + suffix, keep, clock_reaching(3'999'999'999'999LL, 4'000'000'000'000LL));

            THEN("nothing changes or is revoked and the store reports that the session ended")
            {
                CHECK(result.status == Status::SessionEnded);
                CHECK(result.revoked == 0);
                CHECK(store.get_user_by_username("own-" + suffix)->password_hash == old_hash);
                CHECK(store.get_session(other, 5000).has_value());
            }
        }
        store.delete_user(user_id);
    }
}

SCENARIO("[postgres] A self-service change waits for a sign-out of the caller's session on another node")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user with two sessions, one of which another connection is signing out")
    {
        PostgresStore store(test_dsn());
        const auto suffix = unique_suffix();
        const auto old_hash = "old-" + suffix;
        const auto user_id = store.create_user("out-" + suffix, old_hash, "global_viewer");
        const auto mine = "out-mine-" + suffix;
        const auto other = "out-other-" + suffix;
        // The caller's session expires at 10'000 on the clock the change reads;
        // the clock starts at 5'000 and only the test moves it.
        constexpr int64_t mine_expires_at = 10'000;
        std::atomic<int64_t> clock_ms{5'000};
        store.create_session(make_pg_session(user_id, mine, mine_expires_at));
        store.create_session(make_pg_session(user_id, other));
        using Status = PasswordChangeResult::Status;
        // Another server node's logout: its DELETE of the caller's session row
        // is made but not yet committed when the change starts.
        const auto delete_mine = "DELETE FROM sessions WHERE token='" + hash_session_token(mine) + "'";

        WHEN("the change runs before that sign-out commits")
        {
            // Declared first, so the connections close (rolling back and
            // releasing their locks) before the call is joined.
            BackgroundCall<PasswordChangeResult> change;
            PgSideConnection observer;
            PgSideConnection logout;
            logout.run("BEGIN");
            logout.run(delete_mine);
            const int waiter_pid = store.backend_pid();
            const int holder_pid = PQbackendPID(logout.conn);
            change.start([&] {
                return store.update_user_password_if_current_and_revoke_sessions(user_id, old_hash, "new-" + suffix,
                                                                                 mine, [&clock_ms] {
                                                                                     return clock_ms.load();
                                                                                 });
            });
            const bool blocked = wait_until_blocked_by(observer.conn, waiter_pid, holder_pid) && change.running();

            AND_WHEN("the sign-out commits")
            {
                logout.run("COMMIT");
                const auto result = change.get();

                THEN("the change waited for it and then changed nothing")
                {
                    CHECK(blocked);
                    CHECK(result.status == Status::SessionEnded);
                    CHECK(result.revoked == 0);
                    CHECK(store.get_user_by_username("out-" + suffix)->password_hash == old_hash);
                    CHECK(store.get_session(other, 5000).has_value());
                }
            }

            AND_WHEN("the sign-out rolls back only after the caller's session expired")
            {
                // The change read the clock before it blocked; time moves on
                // while it waits, which a row lock does not prevent.
                clock_ms = mine_expires_at;
                logout.run("ROLLBACK");
                const auto result = change.get();

                THEN("the change judges expiry after the wait and changes nothing")
                {
                    CHECK(blocked);
                    CHECK(result.status == Status::SessionEnded);
                    CHECK(result.revoked == 0);
                    CHECK(store.get_user_by_username("out-" + suffix)->password_hash == old_hash);
                    CHECK(store.get_session(other, 5000).has_value());
                    CHECK(store.get_session(mine, 5000).has_value()); // rolled back, not deleted
                }
            }

            AND_WHEN("the sign-out rolls back while the caller's session is still valid")
            {
                logout.run("ROLLBACK");
                const auto result = change.get();

                THEN("the change waited for it and then went ahead, keeping the caller's session")
                {
                    CHECK(blocked);
                    CHECK(result.status == Status::Changed);
                    CHECK(result.revoked == 1);
                    CHECK(store.get_user_by_username("out-" + suffix)->password_hash == "new-" + suffix);
                    CHECK(store.get_session(mine, 5000).has_value());
                    CHECK_FALSE(store.get_session(other, 5000).has_value());
                }
            }
        }
        store.delete_user(user_id);
    }
}

// A self-service password change holds the caller's session row FOR SHARE and
// then deletes the user's other sessions. A purge that waited for that row
// while holding the user's other expired rows would deadlock with it, so the
// purge must skip rows another transaction holds and leave them for later.
SCENARIO("[postgres] A purge skips an expired session another transaction holds instead of waiting for it")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a user with two expired sessions, one of them locked by another transaction")
    {
        PostgresStore store(test_dsn());
        const auto suffix = unique_suffix();
        const auto user_id = store.create_user("purge-" + suffix, "hash", "global_viewer");
        const auto held = "purge-held-" + suffix;
        store.create_session(make_pg_session(user_id, held, 2'000));
        store.create_session(make_pg_session(user_id, "purge-loose-" + suffix, 2'000));

        WHEN("a purge runs while that transaction still holds the lock")
        {
            // Declared first, so the connections close (releasing the lock)
            // before the call is joined.
            BackgroundCall<int> purge;
            PgSideConnection observer;
            PgSideConnection holder;
            // This user's session rows, counted on a connection of its own.
            const auto rows_left = [&] {
                const auto sql = "SELECT COUNT(*) FROM sessions WHERE user_id=" + std::to_string(user_id);
                const std::unique_ptr<PGresult, decltype(&PQclear)> res(PQexec(observer.conn, sql.c_str()), &PQclear);
                REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
                return std::stoi(PQgetvalue(res.get(), 0, 0));
            };
            holder.run("BEGIN");
            // What a self-service password change holds on the caller's session.
            holder.run("SELECT 1 FROM sessions WHERE token='" + hash_session_token(held) + "' FOR SHARE");
            purge.start([&] {
                return store.delete_expired_sessions(5'000);
            });
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (purge.running() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const bool finished_while_held = !purge.running();
            const auto left_while_held = rows_left();
            holder.run("COMMIT");
            const int removed = purge.get();
            const int removed_later = store.delete_expired_sessions(5'000);
            const auto left_at_end = rows_left();

            THEN("it removes the other expired session without waiting, and a later purge removes the held one")
            {
                CHECK(finished_while_held);
                CHECK(left_while_held == 1);
                CHECK(removed >= 1); // the shared test database may hold other expired rows
                CHECK(removed_later >= 1);
                CHECK(left_at_end == 0);
            }
        }
        store.delete_user(user_id);
    }
}

namespace
{
// Value of `SHOW transaction_isolation` on conn: inside a transaction block
// the block's level, outside one the level a single statement runs at.
std::string isolation_level(PGconn* conn)
{
    const std::unique_ptr<PGresult, decltype(&PQclear)> res(PQexec(conn, "SHOW transaction_isolation"), &PQclear);
    REQUIRE(PQresultStatus(res.get()) == PGRES_TUPLES_OK);
    return PQgetvalue(res.get(), 0, 0);
}
} // namespace

// Needs no server: libpq parses and rebuilds the DSN locally. The isolation
// test below opens a connection with an extra server option, and the test DSN
// may be in either libpq form, so the option cannot simply be appended.
SCENARIO("A test DSN gains a server option whatever form it is written in")
{
    constexpr std::string_view extra = "-c default_transaction_isolation=serializable";

    GIVEN("a URI-form DSN with credentials, a port and a query parameter")
    {
        // The password is made at run time, so no credential is written in the
        // source; any value shows that the password survives the rebuild.
        const auto secret = "pw" + unique_suffix();
        const std::string dsn = "postgresql://tw_user:" + secret + "@db.example:6543/tw_db?sslmode=disable";

        WHEN("the server option is added")
        {
            const auto settings = conninfo_settings(dsn_with_server_options(dsn, std::string(extra)));

            THEN("the result parses, keeps every setting and carries the option")
            {
                REQUIRE_FALSE(settings.empty());
                CHECK(settings.at("user") == "tw_user");
                CHECK(settings.at("password") == secret);
                CHECK(settings.at("host") == "db.example");
                CHECK(settings.at("port") == "6543");
                CHECK(settings.at("dbname") == "tw_db");
                CHECK(settings.at("sslmode") == "disable");
                CHECK(settings.at("options") == extra);
            }
        }
    }

    GIVEN("a keyword DSN that already sets server options and has a password with a quote and a backslash")
    {
        const std::string dsn =
            R"(host=localhost dbname=tw_db password='it\'s\\x' options='-c statement_timeout=5000')";

        WHEN("the server option is added")
        {
            const auto settings = conninfo_settings(dsn_with_server_options(dsn, std::string(extra)));

            THEN("the existing options stay in front of the new one and the password is unchanged")
            {
                REQUIRE_FALSE(settings.empty());
                CHECK(settings.at("options") == "-c statement_timeout=5000 " + std::string(extra));
                CHECK(settings.at("password") == R"(it's\x)");
                CHECK(settings.at("host") == "localhost");
                CHECK(settings.at("dbname") == "tw_db");
            }
        }
    }

    GIVEN("a DSN that libpq cannot parse")
    {
        const std::string dsn = "host='unterminated";

        THEN("adding an option is refused instead of producing a broken DSN")
        {
            REQUIRE_THROWS_AS(dsn_with_server_options(dsn, std::string(extra)), std::invalid_argument);
        }
    }
}

SCENARIO("[postgres] Store statements run at READ COMMITTED whatever the connection's default")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    // The store's compare-and-swap updates and row locks rely on READ COMMITTED
    // re-checking a row after a lock wait. A DSN option, ALTER ROLE or ALTER
    // DATABASE can make a stricter level the default for every connection.
    GIVEN("a connection whose default isolation level was changed to REPEATABLE READ after it opened")
    {
        PgSideConnection side;
        side.run("SET default_transaction_isolation = 'repeatable read'");

        WHEN("a store transaction is opened on it")
        {
            postgres::Transaction tx(side.conn);
            const auto level = isolation_level(side.conn);

            THEN("the transaction runs at READ COMMITTED")
            {
                REQUIRE(level == "read committed");
            }
        }
    }

    GIVEN("a store connection opened with a DSN that makes SERIALIZABLE the default")
    {
        postgres::Connection conn(dsn_with_server_options(test_dsn(), "-c default_transaction_isolation=serializable"));

        WHEN("a statement runs outside any transaction block")
        {
            const auto level = isolation_level(conn);

            THEN("it runs at READ COMMITTED too")
            {
                REQUIRE(level == "read committed");
            }
        }
    }
}

SCENARIO("[postgres] Enabling revokes leftover sessions only on a real disabled-to-enabled change")
{
    if (!postgres_is_available())
        SKIP("PostgreSQL not available");
    GIVEN("a disabled user holding a leftover session")
    {
        PostgresStore store(test_dsn());
        const auto suffix = unique_suffix();
        const auto user_id = store.create_user("en-" + suffix, "hash", "global_viewer");
        store.disable_user(user_id);
        store.create_session(make_pg_session(user_id, "en-old-" + suffix));

        WHEN("the user is enabled, a session is created and the enable is retried")
        {
            const auto first = store.enable_user_and_revoke_sessions(user_id);
            store.create_session(make_pg_session(user_id, "en-new-" + suffix));
            const auto retry = store.enable_user_and_revoke_sessions(user_id);

            THEN("the first revokes the leftover, the retry revokes nothing and the new session survives")
            {
                REQUIRE(first.has_value());
                CHECK(*first == 1);
                REQUIRE(retry.has_value());
                CHECK(*retry == 0);
                CHECK_FALSE(store.get_session("en-old-" + suffix, 5000).has_value());
                CHECK(store.get_session("en-new-" + suffix, 5000).has_value());
            }
        }

        WHEN("an unknown user is enabled")
        {
            THEN("the store reports no such user")
            {
                CHECK_FALSE(store.enable_user_and_revoke_sessions(-1).has_value());
            }
        }
        store.delete_user(user_id);
    }
}
#endif // HAVE_LIBPQ
