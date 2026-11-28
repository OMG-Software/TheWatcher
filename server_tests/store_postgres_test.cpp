// PostgreSQL-specific store behaviour: migration, reconnection, schema
// handling and multi-node use. The shared Store contract (every scenario in
// store_test.cpp) is run against PostgreSQL by the store_postgres_suite test.
//
// Each scenario gets its own throwaway schema (postgres_test_support.hpp), so
// the tests are isolated, order-independent and safe to re-run on a used
// database. See docs/development.md for how to point them at a server.
#ifdef HAVE_LIBPQ

#include "../server/store_synchronized.hpp"
#include "postgres_test_support.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::server;
using thewatcher::server::test::run_sql;
using thewatcher::server::test::TestSchema;

namespace
{

AgentRecord make_agent(const std::string& id)
{
    AgentRecord r;
    r.agent_id = id;
    r.hostname = "host-" + id;
    r.platform = "linux";
    r.curve_public_key_z85 = std::string(40, 'A');
    r.collection_interval = 30;
    r.process_limit = 50;
    r.first_seen = 1000;
    r.last_seen = 1000;
    return r;
}

// Runs a query that returns one value and returns it as text.
std::string query_scalar(const std::string& sql)
{
    postgres::Connection conn(test::postgres_test_dsn());
    PGresult* res = PQexec(conn, sql.c_str());
    std::string value;
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0 && !PQgetisnull(res, 0, 0))
        value = PQgetvalue(res, 0, 0);
    PQclear(res);
    return value;
}

AlertRecord heartbeat_alert(const std::string& agent_id)
{
    AlertRecord alert;
    alert.agent_id = agent_id;
    alert.indicator = "Heartbeat";
    alert.old_status = "green";
    alert.new_status = "red";
    alert.created_at = 2000;
    return alert;
}

} // namespace

// ── Connecting ───────────────────────────────────────────────────────────────

SCENARIO("[postgres] Opening a store on an unreachable server fails with an error")
{
    GIVEN("a DSN that points at a closed port")
    {
        WHEN("a PostgresStore is constructed")
        {
            THEN("it throws instead of returning a half-usable store")
            {
                REQUIRE_THROWS(PostgresStore("host=127.0.0.1 port=1 dbname=none user=none connect_timeout=2"));
            }
        }
    }
}

// ── Migration ────────────────────────────────────────────────────────────────

SCENARIO("[postgres] A database created by an older version is migrated in place")
{
    test::TestSchema schema;
    GIVEN("a legacy schema whose tables lack the newer columns, holding one row each")
    {
        const auto& n = schema.name();
        run_sql("CREATE TABLE " + n +
                ".agents (agent_id TEXT PRIMARY KEY, hostname TEXT NOT NULL DEFAULT '', "
                "platform TEXT NOT NULL DEFAULT '', curve_public_key_z85 TEXT NOT NULL DEFAULT '', "
                "approved BOOLEAN NOT NULL DEFAULT FALSE, first_seen BIGINT NOT NULL DEFAULT 0, "
                "last_seen BIGINT NOT NULL DEFAULT 0)");
        run_sql("INSERT INTO " + n +
                ".agents(agent_id,hostname,approved,first_seen,last_seen) "
                "VALUES('old-agent','old-host',TRUE,5,6)");
        run_sql("CREATE TABLE " + n +
                ".alerts (alert_id BIGSERIAL PRIMARY KEY, "
                "agent_id TEXT NOT NULL REFERENCES " +
                n +
                ".agents(agent_id) ON DELETE CASCADE, "
                "indicator TEXT NOT NULL, old_status TEXT NOT NULL, new_status TEXT NOT NULL, "
                "message TEXT NOT NULL DEFAULT '', created_at BIGINT NOT NULL, "
                "acknowledged_by TEXT NOT NULL DEFAULT '', acknowledged_at BIGINT NOT NULL DEFAULT 0, "
                "deleted_at BIGINT NOT NULL DEFAULT 0)");
        run_sql("INSERT INTO " + n +
                ".alerts(agent_id,indicator,old_status,new_status,created_at) "
                "VALUES('old-agent','cpu','green','red',7)");
        run_sql("CREATE TABLE " + n +
                ".runbooks (runbook_id BIGSERIAL PRIMARY KEY, "
                "indicator TEXT NOT NULL DEFAULT '*', status TEXT NOT NULL, url TEXT NOT NULL, "
                "notes TEXT NOT NULL DEFAULT '', created_by TEXT NOT NULL DEFAULT '', "
                "created_at BIGINT NOT NULL DEFAULT 0)");
        run_sql("INSERT INTO " + n + ".runbooks(indicator,status,url) VALUES('cpu','red','http://old')");

        WHEN("a store is opened on it")
        {
            PostgresStore store(schema.dsn());

            THEN("the old rows survive and the new columns carry their defaults")
            {
                const auto agent = store.get_agent("old-agent");
                REQUIRE(agent.has_value());
                CHECK(agent->hostname == "old-host");
                CHECK(agent->approved);
                CHECK(agent->collection_interval == 30);
                CHECK(agent->runbook_markdown.empty());

                const auto alerts = store.list_alerts(false);
                REQUIRE(alerts.size() == 1);
                CHECK(alerts[0].note.empty());
                CHECK(alerts[0].escalated_at == 0);

                const auto runbooks = store.list_runbooks();
                REQUIRE(runbooks.size() == 1);
                CHECK(runbooks[0].agent_id.empty());
                CHECK(runbooks[0].url == "http://old");
            }

            AND_WHEN("the migrated database is opened again")
            {
                THEN("the migration is idempotent")
                {
                    REQUIRE_NOTHROW(PostgresStore(schema.dsn()));
                    PostgresStore again(schema.dsn());
                    REQUIRE(again.get_agent("old-agent").has_value());
                }
            }
        }
    }
}

SCENARIO("[postgres] The store lives in the schema its search_path selects, not in public")
{
    test::TestSchema schema;
    GIVEN("a DSN whose search_path is a private schema")
    {
        WHEN("the store is opened twice (the second open re-runs every migration)")
        {
            PostgresStore first(schema.dsn());
            first.upsert_agent(make_agent("schema-agent"));

            THEN("the second open does not fail trying to re-add existing columns")
            {
                REQUIRE_NOTHROW(PostgresStore(schema.dsn()));
            }

            AND_THEN("the tables are in the private schema")
            {
                CHECK(query_scalar("SELECT count(*) FROM information_schema.tables WHERE table_schema='" +
                                   schema.name() + "' AND table_name='agents'") == "1");
                CHECK(query_scalar("SELECT count(*) FROM " + schema.name() + ".agents") == "1");
            }
        }
    }
}

SCENARIO("[postgres] The hot-path indexes exist")
{
    test::TestSchema schema;
    GIVEN("a freshly migrated store")
    {
        PostgresStore store(schema.dsn());

        THEN("the indexes for pruning, alert lookups, group cascades and sessions exist")
        {
            for (const char* index : {"idx_metrics_ts", "idx_alerts_agent_indicator", "idx_agent_groups_group",
                                      "idx_user_groups_group", "idx_sessions_expires", "idx_sessions_user"})
            {
                INFO(index);
                CHECK(query_scalar("SELECT count(*) FROM pg_indexes WHERE schemaname='" + schema.name() +
                                   "' AND indexname='" + index + "'") == "1");
            }
        }
    }
}

SCENARIO("[postgres] Several nodes starting at once migrate the schema exactly once")
{
    test::TestSchema schema;
    GIVEN("an empty schema and four nodes starting together")
    {
        std::atomic<bool> go{false};
        std::atomic<int> failures{0};
        std::vector<std::thread> nodes;

        WHEN("each node opens a store on the same schema at the same moment")
        {
            for (int i = 0; i < 4; ++i)
            {
                nodes.emplace_back([&] {
                    while (!go.load())
                        std::this_thread::yield();
                    try
                    {
                        PostgresStore store(schema.dsn());
                    }
                    catch (...)
                    {
                        ++failures;
                    }
                });
            }
            go = true;
            for (auto& node : nodes)
                node.join();

            THEN("no node fails and there is exactly one default admin and one Admins group")
            {
                CHECK(failures == 0);
                CHECK(query_scalar("SELECT count(*) FROM " + schema.name() + ".users WHERE username='thewatcher'") ==
                      "1");
                CHECK(query_scalar("SELECT count(*) FROM " + schema.name() + ".groups WHERE name='Admins'") == "1");
                CHECK(query_scalar("SELECT count(*) FROM " + schema.name() + ".user_groups") == "1");
            }
        }
    }
}

// ── Reconnecting ─────────────────────────────────────────────────────────────

namespace
{
// Terminates every backend that belongs to the store tagged `application`, as
// a pgBouncer restart or a failover would, and waits until they are gone so
// the next statement cannot slip in before the server has acted on it.
void kill_connections(const std::string& application)
{
    run_sql("SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE application_name='" + application +
            "' AND pid<>pg_backend_pid()");
    for (int i = 0; i < 100; ++i)
    {
        if (query_scalar("SELECT count(*) FROM pg_stat_activity WHERE application_name='" + application + "'") == "0")
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    FAIL("the backend tagged " << application << " did not terminate");
}

// Cuts the store's connection once, at the moment the store is about to send
// the first statement that starts with `sql_prefix`: that is, while the call
// is in flight (and, for a transaction, after BEGIN and earlier statements).
// Returns a counter of how many times it fired.
std::shared_ptr<std::atomic<int>> cut_connection_at(PostgresStore& store, const std::string& application,
                                                    const std::string& sql_prefix)
{
    auto fired = std::make_shared<std::atomic<int>>(0);
    store.set_statement_hook([=](const char* sql) {
        if (std::string(sql).rfind(sql_prefix, 0) == 0 && fired->fetch_add(1) == 0)
            kill_connections(application);
    });
    return fired;
}
} // namespace

SCENARIO("[postgres] A write that dies with its connection is reported, not repeated, and the next call recovers")
{
    test::TestSchema schema;
    GIVEN("a store whose connection is cut while a write is in flight")
    {
        const std::string application = "tw_write_" + schema.name();
        PostgresStore store(schema.dsn(application));
        const auto fired = cut_connection_at(store, application, "INSERT INTO server_settings");

        WHEN("the write is made")
        {
            THEN("it fails and is not repeated, then the next calls reconnect by themselves")
            {
                // The one injected fault is consumed by this first call.
                REQUIRE_THROWS_AS(store.set_setting("reconnect", "lost"), std::runtime_error);
                CHECK(fired->load() == 1);
                CHECK(store.get_setting("reconnect", "absent") == "absent"); // the lost write was not applied
                REQUIRE_NOTHROW(store.set_setting("reconnect", "second"));
                CHECK(store.get_setting("reconnect", "absent") == "second");
            }
        }
    }
}

SCENARIO("[postgres] A read that dies with its connection is retried on a fresh one")
{
    test::TestSchema schema;
    GIVEN("a store with a setting, whose connection is cut while a read is in flight")
    {
        const std::string application = "tw_read_" + schema.name();
        PostgresStore store(schema.dsn(application));
        store.set_setting("reconnect", "kept");
        const auto fired = cut_connection_at(store, application, "SELECT value FROM server_settings");

        WHEN("the setting is read")
        {
            const auto value = store.get_setting("reconnect", "absent");

            THEN("the caller sees the stored value and never notices the cut")
            {
                CHECK(value == "kept");
                CHECK(fired->load() == 1);
            }
        }
    }
}

SCENARIO("[postgres] A store that was idle when its connection was cut keeps working")
{
    test::TestSchema schema;
    GIVEN("a store whose connection is terminated between two calls")
    {
        const std::string application = "tw_idle_" + schema.name();
        PostgresStore store(schema.dsn(application));
        store.upsert_agent(make_agent("idle-agent"));
        kill_connections(application);

        WHEN("a read is issued")
        {
            THEN("it succeeds on a fresh connection")
            {
                const auto agent = store.get_agent("idle-agent");
                REQUIRE(agent.has_value());
                CHECK(agent->hostname == "host-idle-agent");
            }
        }
    }
}

SCENARIO("[postgres] A connection cut inside a transaction fails the call and rolls the transaction back")
{
    test::TestSchema schema;
    GIVEN("an agent in one group and a transaction that moves it to another")
    {
        const std::string application = "tw_txn_" + schema.name();
        PostgresStore store(schema.dsn(application));
        store.upsert_agent(make_agent("txn-agent"));
        const auto old_group = store.create_group("txn-old");
        const auto new_group = store.create_group("txn-new");
        store.set_agent_groups("txn-agent", {old_group});
        // BEGIN and the DELETE have run; the connection dies before the INSERT.
        const auto fired = cut_connection_at(store, application, "INSERT INTO agent_groups");

        WHEN("the group list is replaced")
        {
            THEN("the call fails, the old membership is intact, and the store works again")
            {
                // The one injected fault is consumed by this first call.
                REQUIRE_THROWS_AS(store.set_agent_groups("txn-agent", {new_group}), std::runtime_error);
                CHECK(fired->load() == 1);
                CHECK(store.get_agent_groups("txn-agent") == std::vector<int64_t>{old_group});
                REQUIRE_NOTHROW(store.set_agent_groups("txn-agent", {new_group}));
                CHECK(store.get_agent_groups("txn-agent") == std::vector<int64_t>{new_group});
            }
        }
    }
}

// ── Multiple nodes ───────────────────────────────────────────────────────────

SCENARIO("[postgres] Two server nodes on one database share agent state and metrics")
{
    test::TestSchema schema;
    GIVEN("two stores connected to the same schema")
    {
        PostgresStore node_a(schema.dsn());
        PostgresStore node_b(schema.dsn());

        WHEN("node A records an agent and metrics")
        {
            node_a.upsert_agent(make_agent("shared-agent"));
            MetricsRow row;
            row.agent_id = "shared-agent";
            row.timestamp_ms = 5000;
            row.metrics_cbor = {0x01, 0x02, 0x03};
            node_a.insert_metrics(row);

            THEN("node B sees them at once, and a change by B is visible to A")
            {
                REQUIRE(node_b.get_agent("shared-agent").has_value());
                const auto metrics = node_b.get_metrics("shared-agent", 10);
                REQUIRE(metrics.size() == 1);
                CHECK(metrics[0].metrics_cbor == std::vector<uint8_t>{0x01, 0x02, 0x03});

                node_b.approve_agent("shared-agent");
                CHECK(node_a.get_agent("shared-agent")->approved);
            }
        }
    }
}

SCENARIO("[postgres] Two server nodes racing to raise a dead-agent alert create exactly one")
{
    test::TestSchema schema;
    GIVEN("two connections to one database and several offline agents")
    {
        PostgresStore node_a(schema.dsn());
        PostgresStore node_b(schema.dsn());
        std::vector<std::string> ids;
        for (int i = 0; i < 20; ++i)
        {
            ids.push_back("race-" + std::to_string(i));
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
                        if (node.insert_heartbeat_alert_if_absent(heartbeat_alert(id)))
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
    }
}

SCENARIO("[postgres] The open-alert check and the heartbeat insert behave as they do on SQLite")
{
    test::TestSchema schema;
    GIVEN("an agent with one unacknowledged cpu alert")
    {
        PostgresStore store(schema.dsn());
        store.upsert_agent(make_agent("a1"));
        AlertRecord alert;
        alert.agent_id = "a1";
        alert.indicator = "cpu";
        alert.old_status = "green";
        alert.new_status = "red";
        alert.created_at = 1000;
        const auto id = store.insert_alert(alert);

        WHEN("the alert is open")
        {
            THEN("has_open_alert matches only the same agent, indicator and severity")
            {
                CHECK(store.has_open_alert("a1", "cpu", "red"));
                CHECK_FALSE(store.has_open_alert("a1", "cpu", "amber"));
                CHECK_FALSE(store.has_open_alert("a1", "memory", "red"));
                CHECK_FALSE(store.has_open_alert("a2", "cpu", "red"));
            }
        }

        WHEN("the alert is acknowledged")
        {
            store.acknowledge_alert(id, "admin", 2000);

            THEN("it is no longer open")
            {
                CHECK_FALSE(store.has_open_alert("a1", "cpu", "red"));
            }
        }

        WHEN("the alert is deleted")
        {
            store.soft_delete_alert(id, 2000);

            THEN("it is no longer open")
            {
                CHECK_FALSE(store.has_open_alert("a1", "cpu", "red"));
            }
        }

        WHEN("a heartbeat alert is inserted twice")
        {
            const auto first = store.insert_heartbeat_alert_if_absent(heartbeat_alert("a1"));
            const auto second = store.insert_heartbeat_alert_if_absent(heartbeat_alert("a1"));

            THEN("the first returns the new alert id and the second returns 0")
            {
                CHECK(first > 0);
                CHECK(second == 0);
                CHECK(store.has_open_alert("a1", "Heartbeat", "red"));
            }
        }
    }
}

// ── Threads ──────────────────────────────────────────────────────────────────

SCENARIO("[postgres] Eight threads can use one synchronized store at the same time")
{
    test::TestSchema schema;
    GIVEN("a shared store with an agent and two groups")
    {
        SynchronizedStore store(std::make_unique<PostgresStore>(schema.dsn()));
        const std::string id = "pg-concurrent";
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
    }
}

#endif // HAVE_LIBPQ
