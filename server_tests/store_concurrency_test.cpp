// Store concurrency, transaction and RAII behaviour (issues #73, #72, #78,
// #79, #51 and the store part of #100).
#include "../server/sqlite_raii.hpp"
#include "../server/store_sqlite.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::server;

namespace
{

AgentRecord make_agent(const std::string& id)
{
    AgentRecord r;
    r.agent_id = id;
    r.hostname = "host-" + id;
    r.platform = "linux";
    r.curve_public_key_z85 = "key-" + id;
    r.approved = true;
    r.first_seen = 1000;
    r.last_seen = 1000;
    return r;
}

// Raw second connection to the same database file, for fault injection.
struct SideConnection
{
    explicit SideConnection(const std::string& path)
        : db(path)
    {
    }
    void run(const char* sql)
    {
        sqlite::exec(db, sql);
    }
    sqlite::Connection db;
};

std::string db_file(thewatcher::test::ScopedTempDir& dir)
{
    return (dir.path() / "store.db").string();
}

} // namespace

// ── #100: RAII helpers ───────────────────────────────────────────────────────

SCENARIO("A prepared statement can be moved and is finalized exactly once")
{
    GIVEN("an in-memory connection and a prepared statement")
    {
        sqlite::Connection db(":memory:");
        sqlite::Stmt first;
        REQUIRE(sqlite3_prepare_v2(db, "SELECT 42;", -1, &first.s, nullptr) == SQLITE_OK);

        WHEN("the statement is move-constructed")
        {
            sqlite::Stmt second(std::move(first));

            THEN("the source is empty and the target still runs the query")
            {
                REQUIRE(first.s == nullptr);
                REQUIRE(sqlite::step_row(second.s, db, "step"));
                REQUIRE(sqlite3_column_int(second.s, 0) == 42);
            }
        }

        WHEN("the statement is move-assigned over another one")
        {
            sqlite::Stmt other;
            REQUIRE(sqlite3_prepare_v2(db, "SELECT 7;", -1, &other.s, nullptr) == SQLITE_OK);
            other = std::move(first);

            THEN("the target runs the moved query")
            {
                REQUIRE(sqlite::step_row(other.s, db, "step"));
                REQUIRE(sqlite3_column_int(other.s, 0) == 42);
            }
        }
    }
}

SCENARIO("step_row reports a failing statement instead of an empty result")
{
    GIVEN("a statement that fails while stepping")
    {
        sqlite::Connection db(":memory:");
        sqlite::Stmt failing;
        // abs() of the smallest 64-bit integer overflows at step time.
        REQUIRE(sqlite3_prepare_v2(db, "SELECT abs(-9223372036854775808);", -1, &failing.s, nullptr) == SQLITE_OK);

        THEN("step_row throws")
        {
            REQUIRE_THROWS_AS(sqlite::step_row(failing.s, db, "failing"), std::runtime_error);
        }
    }

    GIVEN("a statement that returns no rows")
    {
        sqlite::Connection db(":memory:");
        sqlite::Stmt empty;
        REQUIRE(sqlite3_prepare_v2(db, "SELECT 1 WHERE 0;", -1, &empty.s, nullptr) == SQLITE_OK);

        THEN("step_row returns false")
        {
            REQUIRE_FALSE(sqlite::step_row(empty.s, db, "empty"));
        }
    }
}

SCENARIO("A transaction guard rolls back unless it is committed")
{
    GIVEN("an in-memory table")
    {
        sqlite::Connection db(":memory:");
        sqlite::exec(db, "CREATE TABLE t(v INTEGER);");

        auto count = [&] {
            sqlite::Stmt st;
            REQUIRE(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &st.s, nullptr) == SQLITE_OK);
            REQUIRE(sqlite::step_row(st.s, db, "count"));
            return sqlite3_column_int(st.s, 0);
        };

        WHEN("the guard goes out of scope without commit")
        {
            {
                sqlite::Transaction tx(db);
                sqlite::exec(db, "INSERT INTO t VALUES(1);");
            }

            THEN("the insert is rolled back")
            {
                REQUIRE(count() == 0);
            }
        }

        WHEN("an exception unwinds through the guard")
        {
            try
            {
                sqlite::Transaction tx(db);
                sqlite::exec(db, "INSERT INTO t VALUES(1);");
                throw std::runtime_error("original error");
            }
            catch (const std::runtime_error& e)
            {
                THEN("the original error is the one seen and nothing is stored")
                {
                    REQUIRE(std::string(e.what()) == "original error");
                    REQUIRE(count() == 0);
                }
            }
        }

        WHEN("the guard is committed")
        {
            {
                sqlite::Transaction tx(db);
                sqlite::exec(db, "INSERT INTO t VALUES(1);");
                tx.commit();
            }

            THEN("the insert persists")
            {
                REQUIRE(count() == 1);
            }
        }
    }
}

// ── #51: narrow agent updates ────────────────────────────────────────────────

SCENARIO("A heartbeat touch does not revert operator-saved agent settings")
{
    GIVEN("an approved agent whose operator saved new settings")
    {
        SqliteStore store(":memory:");
        store.upsert_agent(make_agent("a1"));
        store.set_agent_heartbeat_interval("a1", 17);
        store.set_agent_collection_interval("a1", 45);
        store.set_agent_process_limit("a1", 99);
        store.set_agent_runbook("a1", "# runbook");
        store.set_agent_maintenance("a1", true, "patching", 5000);
        auto cfg = default_collector_config();
        cfg.cpu_readings = 4;
        store.set_agent_collector_config("a1", cfg);

        WHEN("a heartbeat touches the agent")
        {
            store.touch_agent("a1", 9999, true);

            THEN("only last_seen and connected change")
            {
                const auto rec = store.get_agent("a1");
                REQUIRE(rec.has_value());
                CHECK(rec->last_seen == 9999);
                CHECK(rec->connected);
                CHECK(rec->heartbeat_interval == 17);
                CHECK(rec->collection_interval == 45);
                CHECK(rec->process_limit == 99);
                CHECK(rec->runbook_markdown == "# runbook");
                CHECK(rec->maintenance);
                CHECK(rec->maintenance_reason == "patching");
                CHECK(rec->collector_config.cpu_readings == 4);
            }
        }
    }
}

SCENARIO("A saved heartbeat interval survives concurrent heartbeats")
{
    GIVEN("a shared store and an agent that keeps sending heartbeats")
    {
        auto store = make_store("sqlite", ":memory:");
        store->upsert_agent(make_agent("a1"));
        std::atomic<bool> stop{false};
        std::atomic<int> errors{0};
        std::thread heartbeats([&] {
            int64_t t = 2000;
            while (!stop.load())
            {
                try
                {
                    store->touch_agent("a1", t++, true);
                }
                catch (...)
                {
                    ++errors;
                }
            }
        });

        WHEN("an operator saves a new heartbeat interval meanwhile")
        {
            for (int i = 0; i < 50; ++i)
                store->set_agent_heartbeat_interval("a1", 20 + (i % 5));
            store->set_agent_heartbeat_interval("a1", 33);
            stop = true;
            heartbeats.join();

            THEN("the saved value persists and no call failed")
            {
                REQUIRE(errors == 0);
                REQUIRE(store->get_agent("a1")->heartbeat_interval == 33);
            }
        }
        if (heartbeats.joinable())
        {
            stop = true;
            heartbeats.join();
        }
    }
}

// ── #72: atomic group and configuration saves ────────────────────────────────

SCENARIO("A failed group assignment leaves the previous memberships intact")
{
    GIVEN("an agent and a user, each in group A")
    {
        SqliteStore store(":memory:");
        store.upsert_agent(make_agent("a1"));
        const auto group_a = store.create_group("A");
        store.set_agent_groups("a1", {group_a});
        const auto user = store.create_user("alice", "hash", "operator");
        store.set_user_groups(user, {group_a});

        WHEN("an assignment names a group that does not exist")
        {
            THEN("the agent assignment throws and the agent is still in A")
            {
                REQUIRE_THROWS(store.set_agent_groups("a1", {group_a, 999}));
                REQUIRE(store.get_agent_groups("a1") == std::vector<int64_t>{group_a});
            }
            THEN("the user assignment throws and the user is still in A")
            {
                REQUIRE_THROWS(store.set_user_groups(user, {999}));
                REQUIRE(store.get_user_groups(user) == std::vector<int64_t>{group_a});
            }
        }
    }
}

SCENARIO("The configure-modal save is applied atomically")
{
    GIVEN("an agent in group A with known settings")
    {
        SqliteStore store(":memory:");
        store.upsert_agent(make_agent("a1"));
        const auto group_a = store.create_group("A");
        const auto group_b = store.create_group("B");
        store.set_agent_groups("a1", {group_a});

        AgentConfigUpdate update;
        update.collection_interval = 60;
        update.heartbeat_interval = 12;
        update.process_limit = 7;
        update.collector_config = default_collector_config();
        update.collector_config.cpu_readings = 5;
        update.runbook_markdown = "# new";

        WHEN("every part is valid")
        {
            update.group_ids = std::vector<int64_t>{group_b};
            REQUIRE(store.save_agent_configuration("a1", update));

            THEN("all parts are stored")
            {
                const auto rec = store.get_agent("a1");
                CHECK(rec->collection_interval == 60);
                CHECK(rec->heartbeat_interval == 12);
                CHECK(rec->process_limit == 7);
                CHECK(rec->collector_config.cpu_readings == 5);
                CHECK(rec->runbook_markdown == "# new");
                CHECK(store.get_agent_groups("a1") == std::vector<int64_t>{group_b});
            }
        }

        WHEN("the group ids contain an unknown group")
        {
            update.group_ids = std::vector<int64_t>{999};

            THEN("the save throws and no part is stored")
            {
                REQUIRE_THROWS(store.save_agent_configuration("a1", update));
                const auto rec = store.get_agent("a1");
                CHECK(rec->collection_interval == 30);
                CHECK(rec->heartbeat_interval == 5);
                CHECK(rec->process_limit == 25);
                CHECK(rec->runbook_markdown.empty());
                CHECK(store.get_agent_groups("a1") == std::vector<int64_t>{group_a});
            }
        }

        WHEN("group ids and runbook are omitted")
        {
            update.runbook_markdown.reset();
            REQUIRE(store.save_agent_configuration("a1", update));

            THEN("the runbook and memberships are kept")
            {
                CHECK(store.get_agent_groups("a1") == std::vector<int64_t>{group_a});
                CHECK(store.get_agent("a1")->process_limit == 7);
            }
        }

        WHEN("the agent does not exist")
        {
            THEN("the save reports false")
            {
                REQUIRE_FALSE(store.save_agent_configuration("missing", update));
            }
        }
    }
}

// ── #79: bulk and dead-agent alert operations ────────────────────────────────

SCENARIO("A bulk alert operation that fails part-way changes nothing")
{
    GIVEN("a file-backed store with three alerts and a trigger that rejects the third")
    {
        thewatcher::test::ScopedTempDir dir("store-bulk");
        SqliteStore store(db_file(dir));
        store.upsert_agent(make_agent("a1"));
        std::vector<int64_t> ids;
        for (int i = 0; i < 3; ++i)
        {
            AlertRecord alert;
            alert.agent_id = "a1";
            alert.indicator = "cpu";
            alert.old_status = "green";
            alert.new_status = "red";
            alert.created_at = 100 + i;
            ids.push_back(store.insert_alert(alert));
        }
        SideConnection side(db_file(dir));
        const auto sql =
            "CREATE TRIGGER reject_third BEFORE UPDATE ON alerts WHEN NEW.alert_id = " + std::to_string(ids[2]) +
            " BEGIN SELECT RAISE(ABORT, 'rejected'); END;";
        side.run(sql.c_str());

        WHEN("they are acknowledged in bulk")
        {
            THEN("the call throws and none is acknowledged")
            {
                REQUIRE_THROWS(store.bulk_acknowledge_alerts(ids, "bob", 500));
                for (const auto& alert : store.list_alerts(true))
                    CHECK(alert.acknowledged_at == 0);
            }
        }

        WHEN("they are soft-deleted in bulk")
        {
            THEN("the call throws and none is deleted")
            {
                REQUIRE_THROWS(store.bulk_soft_delete_alerts(ids, 500));
                for (const auto& alert : store.list_alerts(true))
                    CHECK(alert.deleted_at == 0);
            }
        }
    }
}

SCENARIO("A dead-agent heartbeat alert is created at most once")
{
    GIVEN("an offline approved agent")
    {
        SqliteStore store(":memory:");
        auto agent = make_agent("a1");
        agent.connected = false;
        store.upsert_agent(agent);
        AlertRecord alert;
        alert.agent_id = "a1";
        alert.indicator = "Heartbeat";
        alert.old_status = "green";
        alert.new_status = "red";
        alert.created_at = 2000;

        WHEN("the alert is inserted twice")
        {
            const bool first = store.insert_heartbeat_alert_if_absent(alert);
            const bool second = store.insert_heartbeat_alert_if_absent(alert);

            THEN("only the first insert creates a row")
            {
                CHECK(first);
                CHECK_FALSE(second);
                CHECK(store.list_alerts(false).size() == 1);
            }
        }

        WHEN("the previous heartbeat alert was archived")
        {
            REQUIRE(store.insert_heartbeat_alert_if_absent(alert));
            store.archive_heartbeat_alerts_for_agent("a1", 3000);

            THEN("a new one can be created")
            {
                CHECK(store.insert_heartbeat_alert_if_absent(alert));
            }
        }
    }
}

// ── #78: SQLite errors are not "no rows" ─────────────────────────────────────

SCENARIO("A read that fails is reported, never treated as an empty result")
{
    GIVEN("a file-backed store whose silences table now errors when read")
    {
        thewatcher::test::ScopedTempDir dir("store-readerr");
        SqliteStore store(db_file(dir));
        SideConnection side(db_file(dir));
        side.run("DROP TABLE silences;");
        side.run("CREATE VIEW silences AS SELECT 1 AS silence_id, '*' AS agent_id, '*' AS indicator, '' AS reason, "
                 "abs(-9223372036854775808) AS until_ms, '' AS created_by, 0 AS created_at;");

        THEN("is_silenced throws instead of returning false")
        {
            REQUIRE_THROWS(store.is_silenced("a1", "cpu", 1000));
        }
    }
}

SCENARIO("A write waits for a briefly held lock instead of failing")
{
    GIVEN("a file-backed store and another connection holding the write lock")
    {
        thewatcher::test::ScopedTempDir dir("store-busy");
        SqliteStore store(db_file(dir));
        SideConnection side(db_file(dir));
        side.run("BEGIN IMMEDIATE;");

        WHEN("the lock is released shortly after a setting is written")
        {
            std::thread releaser([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                side.run("COMMIT;");
            });

            THEN("the write succeeds")
            {
                REQUIRE_NOTHROW(store.set_setting("k", "v"));
                releaser.join();
                REQUIRE(store.get_setting("k", "") == "v");
            }
            if (releaser.joinable())
                releaser.join();
        }
    }
}

// ── #73: concurrent access ───────────────────────────────────────────────────

SCENARIO("Eight threads can use one store at the same time")
{
    GIVEN("a shared store with an agent, two groups and a user")
    {
        auto store = make_store("sqlite", ":memory:");
        store->upsert_agent(make_agent("a1"));
        const auto group_a = store->create_group("A");
        const auto group_b = store->create_group("B");
        const auto user = store->create_user("alice", "hash", "operator");
        std::atomic<int> errors{0};

        WHEN("threads mix transactional and plain calls")
        {
            std::vector<std::thread> threads;
            for (int t = 0; t < 8; ++t)
            {
                threads.emplace_back([&, t] {
                    for (int i = 0; i < 100; ++i)
                    {
                        try
                        {
                            switch ((t + i) % 4)
                            {
                            case 0:
                                store->set_agent_groups("a1", (i % 2) ? std::vector<int64_t>{group_a}
                                                                      : std::vector<int64_t>{group_a, group_b});
                                break;
                            case 1:
                                store->set_user_groups(user, {group_b});
                                break;
                            case 2:
                                store->touch_agent("a1", i, true);
                                break;
                            default:
                                (void)store->get_agent("a1");
                                (void)store->list_alerts(false);
                                break;
                            }
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
                const auto groups = store->get_agent_groups("a1");
                REQUIRE((groups == std::vector<int64_t>{group_a} || groups == std::vector<int64_t>{group_a, group_b}));
                REQUIRE(store->get_user_groups(user) == std::vector<int64_t>{group_b});
            }
        }
    }
}
