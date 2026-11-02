#include "store_sqlite.hpp"

#ifdef HAVE_LIBPQ
#include "store_postgres.hpp"
#endif

#include "common/SingleLog.hpp"

#include <cctype>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <sodium.h>

namespace thewatcher::server
{

namespace
{
    constexpr const char* AGENT_COLUMNS =
        "agent_id,hostname,platform,curve_public_key_z85,approved,rejected,connected,maintenance,"
        "maintenance_reason,maintenance_until,collection_interval,process_limit,first_seen,last_seen,"
        "cpu_warning_pct_of_avg,cpu_degraded_pct_of_avg,cpu_critical_pct_of_avg,"
        "memory_warning_pct_of_avg,memory_degraded_pct_of_avg,memory_critical_pct_of_avg,"
        "disk_warning_pct_of_avg,disk_degraded_pct_of_avg,disk_critical_pct_of_avg,"
        "network_warning_pct_of_avg,network_degraded_pct_of_avg,network_critical_pct_of_avg,"
        "collector_config_json,description,heartbeat_interval,runbook_markdown";

    struct Stmt
    {
        sqlite3_stmt* s = nullptr;
        ~Stmt()
        {
            if (s)
                sqlite3_finalize(s);
        }
        sqlite3_stmt* operator->()
        {
            return s;
        }
    };

    void check(int rc, sqlite3* db, const char* ctx)
    {
        if (rc != SQLITE_OK && rc != SQLITE_ROW && rc != SQLITE_DONE)
            throw std::runtime_error(std::string(ctx) + ": " + sqlite3_errmsg(db));
    }

    std::string text_col(sqlite3_stmt* s, int col)
    {
        const auto* value = sqlite3_column_text(s, col);
        return value ? reinterpret_cast<const char*>(value) : "";
    }

    AgentRecord row_to_agent(sqlite3_stmt* s)
    {
        AgentRecord r;
        r.agent_id = text_col(s, 0);
        r.hostname = text_col(s, 1);
        r.platform = text_col(s, 2);
        r.curve_public_key_z85 = text_col(s, 3);
        r.approved = sqlite3_column_int(s, 4) != 0;
        r.rejected = sqlite3_column_int(s, 5) != 0;
        r.connected = sqlite3_column_int(s, 6) != 0;
        r.maintenance = sqlite3_column_int(s, 7) != 0;
        r.maintenance_reason = text_col(s, 8);
        r.maintenance_until = sqlite3_column_int64(s, 9);
        r.collection_interval = sqlite3_column_int(s, 10);
        r.process_limit = sqlite3_column_int(s, 11);
        r.first_seen = sqlite3_column_int64(s, 12);
        r.last_seen = sqlite3_column_int64(s, 13);
        r.cpu_warning_pct_of_avg = sqlite3_column_double(s, 14);
        r.cpu_degraded_pct_of_avg = sqlite3_column_double(s, 15);
        r.cpu_critical_pct_of_avg = sqlite3_column_double(s, 16);
        r.memory_warning_pct_of_avg = sqlite3_column_double(s, 17);
        r.memory_degraded_pct_of_avg = sqlite3_column_double(s, 18);
        r.memory_critical_pct_of_avg = sqlite3_column_double(s, 19);
        r.disk_warning_pct_of_avg = sqlite3_column_double(s, 20);
        r.disk_degraded_pct_of_avg = sqlite3_column_double(s, 21);
        r.disk_critical_pct_of_avg = sqlite3_column_double(s, 22);
        r.network_warning_pct_of_avg = sqlite3_column_double(s, 23);
        r.network_degraded_pct_of_avg = sqlite3_column_double(s, 24);
        r.network_critical_pct_of_avg = sqlite3_column_double(s, 25);
        const auto collector_config_json = text_col(s, 26);
        if (!collector_config_json.empty())
        {
            try
            {
                r.collector_config = nlohmann::json::parse(collector_config_json).get<CollectorConfig>();
            }
            catch (const std::exception& e)
            {
                LOGF_WARNING("Invalid collector config for agent_id=%s error=%s", r.agent_id.c_str(), e.what());
                r.collector_config = default_collector_config();
            }
        }
        r.description = text_col(s, 27);
        r.heartbeat_interval = sqlite3_column_int(s, 28);
        r.runbook_markdown = text_col(s, 29);
        return r;
    }

    PendingStatusRecord row_to_pending_status(sqlite3_stmt* s)
    {
        return {text_col(s, 0), text_col(s, 1), text_col(s, 2), sqlite3_column_int(s, 3)};
    }

    MetricsRow row_to_metrics(sqlite3_stmt* s)
    {
        MetricsRow r;
        r.agent_id = text_col(s, 0);
        r.timestamp_ms = sqlite3_column_int64(s, 1);
        const auto* blob = sqlite3_column_blob(s, 2);
        const auto bytes = sqlite3_column_bytes(s, 2);
        if (blob != nullptr && bytes > 0)
        {
            const auto* data = static_cast<const uint8_t*>(blob);
            r.metrics_cbor.assign(data, data + bytes);
        }
        return r;
    }

    GroupRecord row_to_group(sqlite3_stmt* s)
    {
        return {sqlite3_column_int64(s, 0), text_col(s, 1), sqlite3_column_int(s, 2) != 0};
    }

    UserRecord row_to_user(sqlite3_stmt* s)
    {
        return {
            sqlite3_column_int64(s, 0),   text_col(s, 1), text_col(s, 2), text_col(s, 3), sqlite3_column_int(s, 4) != 0,
            sqlite3_column_int(s, 5) != 0};
    }

    SessionRecord row_to_session(sqlite3_stmt* s)
    {
        return {text_col(s, 0), sqlite3_column_int64(s, 1), text_col(s, 2),
                text_col(s, 3), sqlite3_column_int64(s, 4), sqlite3_column_int64(s, 5)};
    }

    StatusHistoryRow row_to_status(sqlite3_stmt* s)
    {
        return {
            sqlite3_column_int64(s, 0), text_col(s, 1), text_col(s, 2), text_col(s, 3), text_col(s, 4), text_col(s, 5),
            sqlite3_column_int64(s, 6)};
    }

    AlertRecord row_to_alert(sqlite3_stmt* s)
    {
        return {sqlite3_column_int64(s, 0),
                text_col(s, 1),
                text_col(s, 2),
                text_col(s, 3),
                text_col(s, 4),
                text_col(s, 5),
                sqlite3_column_int64(s, 6),
                text_col(s, 7),
                sqlite3_column_int64(s, 8),
                sqlite3_column_int64(s, 9),
                text_col(s, 10),
                sqlite3_column_int64(s, 11),
                text_col(s, 12),
                {}};
    }

    MaintenanceWindowRecord row_to_maintenance_window(sqlite3_stmt* s)
    {
        return {sqlite3_column_int64(s, 0), text_col(s, 1), sqlite3_column_int64(s, 2),
                sqlite3_column_int64(s, 3), text_col(s, 4), text_col(s, 5),
                sqlite3_column_int64(s, 6)};
    }

    std::string hash_default_password()
    {
        char hash[crypto_pwhash_STRBYTES];
        if (crypto_pwhash_str(hash, "look_at_me", 10, crypto_pwhash_OPSLIMIT_INTERACTIVE,
                              crypto_pwhash_MEMLIMIT_INTERACTIVE) != 0)
            throw std::runtime_error("Unable to hash default admin password");
        return hash;
    }
} // namespace

SqliteStore::SqliteStore(const std::string& path)
{
    LOG_FUNCTION_TRACE
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium initialization failed");
    LOGF_DEBUG("Opening SQLite store path=%s", path.c_str());
    check(sqlite3_open(path.c_str(), &db_), db_, "sqlite3_open");
    init_schema();
    bootstrap_defaults();
}

SqliteStore::~SqliteStore()
{
    LOG_FUNCTION_TRACE
    if (db_)
    {
        sqlite3_close(db_);
        LOG_DEBUG("SQLite store closed");
    }
}

void SqliteStore::exec(const char* sql)
{
    LOGF_TRACE("Executing SQL statement prefix=%.80s", sql);
    char* err = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        std::string msg = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error(std::string("sqlite exec: ") + msg);
    }
}

bool SqliteStore::column_exists(const std::string& table, const std::string& column)
{
    // M-5: PRAGMA table_info() does not support parameterized binding; validate
    // that table/column are safe identifiers ([a-z_] only) before concatenating.
    auto is_safe_identifier = [](const std::string& s) {
        for (unsigned char c : s)
            if (!std::islower(c) && c != '_')
                return false;
        return !s.empty();
    };
    if (!is_safe_identifier(table) || !is_safe_identifier(column))
        throw std::runtime_error("column_exists: invalid identifier '" + table + "'/'" + column + "'");

    const auto sql = "PRAGMA table_info(" + table + ");";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr), db_, "prepare table_info");
    while (sqlite3_step(st.s) == SQLITE_ROW)
    {
        if (column == text_col(st.s, 1))
            return true;
    }
    return false;
}

void SqliteStore::add_column_if_missing(const std::string& table, const std::string& column, const char* ddl)
{
    if (!column_exists(table, column))
        exec(ddl);
}

void SqliteStore::init_schema()
{
    LOG_FUNCTION_TRACE
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec(R"(
        CREATE TABLE IF NOT EXISTS agents (
            agent_id             TEXT PRIMARY KEY,
            hostname             TEXT NOT NULL DEFAULT '',
            platform             TEXT NOT NULL DEFAULT '',
            curve_public_key_z85 TEXT NOT NULL DEFAULT '',
            approved             INTEGER NOT NULL DEFAULT 0,
            rejected             INTEGER NOT NULL DEFAULT 0,
            connected            INTEGER NOT NULL DEFAULT 0,
            maintenance          INTEGER NOT NULL DEFAULT 0,
            maintenance_reason   TEXT NOT NULL DEFAULT '',
            maintenance_until    INTEGER NOT NULL DEFAULT 0,
            collection_interval  INTEGER NOT NULL DEFAULT 30,
            process_limit        INTEGER NOT NULL DEFAULT 25,
            first_seen           INTEGER NOT NULL DEFAULT 0,
            last_seen            INTEGER NOT NULL DEFAULT 0,
            cpu_warning_pct_of_avg      REAL NOT NULL DEFAULT 125.0,
            cpu_degraded_pct_of_avg     REAL NOT NULL DEFAULT 150.0,
            cpu_critical_pct_of_avg     REAL NOT NULL DEFAULT 200.0,
            memory_warning_pct_of_avg   REAL NOT NULL DEFAULT 125.0,
            memory_degraded_pct_of_avg  REAL NOT NULL DEFAULT 150.0,
            memory_critical_pct_of_avg  REAL NOT NULL DEFAULT 200.0,
            disk_warning_pct_of_avg     REAL NOT NULL DEFAULT 125.0,
            disk_degraded_pct_of_avg    REAL NOT NULL DEFAULT 150.0,
            disk_critical_pct_of_avg    REAL NOT NULL DEFAULT 200.0,
            network_warning_pct_of_avg  REAL NOT NULL DEFAULT 125.0,
            network_degraded_pct_of_avg REAL NOT NULL DEFAULT 150.0,
            network_critical_pct_of_avg REAL NOT NULL DEFAULT 200.0,
            collector_config_json       TEXT NOT NULL DEFAULT '',
            description                 TEXT NOT NULL DEFAULT '',
            heartbeat_interval          INTEGER NOT NULL DEFAULT 5,
            runbook_markdown            TEXT NOT NULL DEFAULT ''
        );
    )");
    add_column_if_missing("agents", "rejected", "ALTER TABLE agents ADD COLUMN rejected INTEGER NOT NULL DEFAULT 0;");
    add_column_if_missing("agents", "description",
                          "ALTER TABLE agents ADD COLUMN description TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("agents", "connected", "ALTER TABLE agents ADD COLUMN connected INTEGER NOT NULL DEFAULT 0;");
    add_column_if_missing("agents", "maintenance",
                          "ALTER TABLE agents ADD COLUMN maintenance INTEGER NOT NULL DEFAULT 0;");
    add_column_if_missing("agents", "maintenance_reason",
                          "ALTER TABLE agents ADD COLUMN maintenance_reason TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("agents", "maintenance_until",
                          "ALTER TABLE agents ADD COLUMN maintenance_until INTEGER NOT NULL DEFAULT 0;");
    add_column_if_missing("agents", "collection_interval",
                          "ALTER TABLE agents ADD COLUMN collection_interval INTEGER NOT NULL DEFAULT 30;");
    add_column_if_missing("agents", "process_limit",
                          "ALTER TABLE agents ADD COLUMN process_limit INTEGER NOT NULL DEFAULT 25;");
    add_column_if_missing("agents", "cpu_warning_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN cpu_warning_pct_of_avg REAL NOT NULL DEFAULT 125.0;");
    add_column_if_missing("agents", "cpu_degraded_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN cpu_degraded_pct_of_avg REAL NOT NULL DEFAULT 150.0;");
    add_column_if_missing("agents", "cpu_critical_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN cpu_critical_pct_of_avg REAL NOT NULL DEFAULT 200.0;");
    add_column_if_missing("agents", "memory_warning_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN memory_warning_pct_of_avg REAL NOT NULL DEFAULT 125.0;");
    add_column_if_missing("agents", "memory_degraded_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN memory_degraded_pct_of_avg REAL NOT NULL DEFAULT 150.0;");
    add_column_if_missing("agents", "memory_critical_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN memory_critical_pct_of_avg REAL NOT NULL DEFAULT 200.0;");
    add_column_if_missing("agents", "disk_warning_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN disk_warning_pct_of_avg REAL NOT NULL DEFAULT 125.0;");
    add_column_if_missing("agents", "disk_degraded_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN disk_degraded_pct_of_avg REAL NOT NULL DEFAULT 150.0;");
    add_column_if_missing("agents", "disk_critical_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN disk_critical_pct_of_avg REAL NOT NULL DEFAULT 200.0;");
    add_column_if_missing("agents", "network_warning_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN network_warning_pct_of_avg REAL NOT NULL DEFAULT 125.0;");
    add_column_if_missing("agents", "network_degraded_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN network_degraded_pct_of_avg REAL NOT NULL DEFAULT 150.0;");
    add_column_if_missing("agents", "network_critical_pct_of_avg",
                          "ALTER TABLE agents ADD COLUMN network_critical_pct_of_avg REAL NOT NULL DEFAULT 200.0;");
    add_column_if_missing("agents", "collector_config_json",
                          "ALTER TABLE agents ADD COLUMN collector_config_json TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("agents", "heartbeat_interval",
                          "ALTER TABLE agents ADD COLUMN heartbeat_interval INTEGER NOT NULL DEFAULT 5;");
    add_column_if_missing("agents", "runbook_markdown",
                          "ALTER TABLE agents ADD COLUMN runbook_markdown TEXT NOT NULL DEFAULT '';");
    // Schema break in 0.3.0: metrics column changed from JSON TEXT to CBOR BLOB.
    // Existing 0.2.x dev databases drop and recreate the metrics table on first start.
    if (column_exists("metrics", "metrics_json") && !column_exists("metrics", "metrics_cbor"))
    {
        exec("DROP TABLE metrics;");
    }
    exec(R"(
        CREATE TABLE IF NOT EXISTS metrics (
            id           INTEGER PRIMARY KEY AUTOINCREMENT,
            agent_id     TEXT NOT NULL,
            timestamp_ms INTEGER NOT NULL,
            metrics_cbor BLOB NOT NULL,
            FOREIGN KEY (agent_id) REFERENCES agents(agent_id) ON DELETE CASCADE
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_metrics_agent_ts ON metrics(agent_id, timestamp_ms DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS groups (
            group_id INTEGER PRIMARY KEY AUTOINCREMENT,
            name     TEXT NOT NULL UNIQUE,
            built_in INTEGER NOT NULL DEFAULT 0
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS users (
            user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
            username      TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            role          TEXT NOT NULL,
            built_in      INTEGER NOT NULL DEFAULT 0,
            disabled      INTEGER NOT NULL DEFAULT 0
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS user_groups (
            user_id  INTEGER NOT NULL,
            group_id INTEGER NOT NULL,
            PRIMARY KEY(user_id, group_id),
            FOREIGN KEY(user_id) REFERENCES users(user_id) ON DELETE CASCADE,
            FOREIGN KEY(group_id) REFERENCES groups(group_id) ON DELETE CASCADE
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS agent_groups (
            agent_id TEXT NOT NULL,
            group_id INTEGER NOT NULL,
            PRIMARY KEY(agent_id, group_id),
            FOREIGN KEY(agent_id) REFERENCES agents(agent_id) ON DELETE CASCADE,
            FOREIGN KEY(group_id) REFERENCES groups(group_id) ON DELETE CASCADE
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            token      TEXT PRIMARY KEY,
            user_id    INTEGER NOT NULL,
            created_at INTEGER NOT NULL,
            expires_at INTEGER NOT NULL,
            FOREIGN KEY(user_id) REFERENCES users(user_id) ON DELETE CASCADE
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS status_history (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            agent_id    TEXT NOT NULL,
            indicator   TEXT NOT NULL,
            old_status  TEXT NOT NULL,
            new_status  TEXT NOT NULL,
            message     TEXT NOT NULL DEFAULT '',
            created_at  INTEGER NOT NULL,
            FOREIGN KEY(agent_id) REFERENCES agents(agent_id) ON DELETE CASCADE
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_status_history_agent_indicator ON status_history(agent_id, indicator, "
         "created_at DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS pending_status (
            agent_id      TEXT NOT NULL,
            indicator     TEXT NOT NULL,
            target_status TEXT NOT NULL,
            count         INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(agent_id, indicator),
            FOREIGN KEY(agent_id) REFERENCES agents(agent_id) ON DELETE CASCADE
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS alerts (
            alert_id        INTEGER PRIMARY KEY AUTOINCREMENT,
            agent_id        TEXT NOT NULL,
            indicator       TEXT NOT NULL,
            old_status      TEXT NOT NULL,
            new_status      TEXT NOT NULL,
            message         TEXT NOT NULL DEFAULT '',
            created_at      INTEGER NOT NULL,
            acknowledged_by TEXT NOT NULL DEFAULT '',
            acknowledged_at INTEGER NOT NULL DEFAULT 0,
            deleted_at      INTEGER NOT NULL DEFAULT 0,
            note            TEXT NOT NULL DEFAULT '',
            escalated_at    INTEGER NOT NULL DEFAULT 0,
            FOREIGN KEY(agent_id) REFERENCES agents(agent_id) ON DELETE CASCADE
        );
    )");
    add_column_if_missing("alerts", "note", "ALTER TABLE alerts ADD COLUMN note TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("alerts", "escalated_at",
                          "ALTER TABLE alerts ADD COLUMN escalated_at INTEGER NOT NULL DEFAULT 0;");
    exec("CREATE INDEX IF NOT EXISTS idx_alerts_active ON alerts(deleted_at, acknowledged_at, created_at DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS maintenance_windows (
            window_id  INTEGER PRIMARY KEY AUTOINCREMENT,
            agent_id   TEXT NOT NULL DEFAULT '*',
            start_ms   INTEGER NOT NULL,
            end_ms     INTEGER NOT NULL,
            reason     TEXT NOT NULL DEFAULT '',
            created_by TEXT NOT NULL DEFAULT '',
            created_at INTEGER NOT NULL
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_maint_windows_time ON maintenance_windows(start_ms, end_ms);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS server_settings (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS silences (
            silence_id INTEGER PRIMARY KEY AUTOINCREMENT,
            agent_id   TEXT NOT NULL DEFAULT '*',
            indicator  TEXT NOT NULL DEFAULT '*',
            reason     TEXT NOT NULL DEFAULT '',
            until_ms   INTEGER NOT NULL,
            created_by TEXT NOT NULL DEFAULT '',
            created_at INTEGER NOT NULL
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_silences_until ON silences(until_ms);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS log_matches (
            match_id       INTEGER PRIMARY KEY AUTOINCREMENT,
            agent_id       TEXT NOT NULL,
            indicator_name TEXT NOT NULL,
            path           TEXT NOT NULL DEFAULT '',
            matched_line   TEXT NOT NULL DEFAULT '',
            severity       TEXT NOT NULL DEFAULT 'red',
            created_at     INTEGER NOT NULL,
            FOREIGN KEY(agent_id) REFERENCES agents(agent_id) ON DELETE CASCADE
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_log_matches_agent_ts ON log_matches(agent_id, created_at DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS views (
            view_id        INTEGER PRIMARY KEY AUTOINCREMENT,
            name           TEXT NOT NULL,
            owner_user_id  INTEGER NOT NULL DEFAULT 0,
            is_public      INTEGER NOT NULL DEFAULT 0,
            group_id       INTEGER NOT NULL DEFAULT 0,
            config_json    TEXT NOT NULL DEFAULT '{}',
            created_at     INTEGER NOT NULL
        );
    )");
    add_column_if_missing("views", "group_id", "ALTER TABLE views ADD COLUMN group_id INTEGER NOT NULL DEFAULT 0;");
    exec(R"(
        CREATE TABLE IF NOT EXISTS runbooks (
            runbook_id  INTEGER PRIMARY KEY AUTOINCREMENT,
            agent_id    TEXT NOT NULL DEFAULT '',
            indicator   TEXT NOT NULL DEFAULT '*',
            status      TEXT NOT NULL,
            url         TEXT NOT NULL,
            markdown    TEXT NOT NULL DEFAULT '',
            notes       TEXT NOT NULL DEFAULT '',
            created_by  TEXT NOT NULL DEFAULT '',
            created_at  INTEGER NOT NULL DEFAULT 0
        );
    )");
    add_column_if_missing("runbooks", "agent_id", "ALTER TABLE runbooks ADD COLUMN agent_id TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("runbooks", "markdown", "ALTER TABLE runbooks ADD COLUMN markdown TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("alerts", "runbook_url",
                          "ALTER TABLE alerts ADD COLUMN runbook_url TEXT NOT NULL DEFAULT '';");
}

void SqliteStore::bootstrap_defaults()
{
    LOG_FUNCTION_TRACE
    const auto admin_group = create_group("Admins");
    Stmt st;
    check(sqlite3_prepare_v2(db_, "SELECT user_id FROM users WHERE username='thewatcher';", -1, &st.s, nullptr), db_,
          "prepare default admin lookup");
    if (sqlite3_step(st.s) != SQLITE_ROW)
    {
        const auto admin_id = create_user("thewatcher", hash_default_password(), "global_admin");
        set_user_groups(admin_id, {admin_group});
        exec("UPDATE users SET built_in=1 WHERE username='thewatcher';");
        LOG_INFO("Bootstrapped default admin user thewatcher");
    }
    exec("UPDATE groups SET built_in=1 WHERE name='Admins';");
}

void SqliteStore::upsert_agent(const AgentRecord& rec)
{
    LOGF_TRACE("Upserting agent id=%s approved=%d rejected=%d connected=%d maintenance=%d interval=%d process_limit=%d",
               rec.agent_id.c_str(), rec.approved ? 1 : 0, rec.rejected ? 1 : 0, rec.connected ? 1 : 0,
               rec.maintenance ? 1 : 0, rec.collection_interval, rec.process_limit);
    const char* sql = R"(
        INSERT INTO agents(agent_id,hostname,platform,curve_public_key_z85,approved,rejected,connected,maintenance,maintenance_reason,maintenance_until,collection_interval,process_limit,first_seen,last_seen,cpu_warning_pct_of_avg,cpu_degraded_pct_of_avg,cpu_critical_pct_of_avg,memory_warning_pct_of_avg,memory_degraded_pct_of_avg,memory_critical_pct_of_avg,disk_warning_pct_of_avg,disk_degraded_pct_of_avg,disk_critical_pct_of_avg,network_warning_pct_of_avg,network_degraded_pct_of_avg,network_critical_pct_of_avg,collector_config_json,description,heartbeat_interval,runbook_markdown)
        VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)
        ON CONFLICT(agent_id) DO UPDATE SET
            hostname=excluded.hostname,
            platform=excluded.platform,
            curve_public_key_z85=excluded.curve_public_key_z85,
            connected=excluded.connected,
            maintenance=excluded.maintenance,
            maintenance_reason=excluded.maintenance_reason,
            maintenance_until=excluded.maintenance_until,
            collection_interval=excluded.collection_interval,
            process_limit=excluded.process_limit,
            collector_config_json=excluded.collector_config_json,
            heartbeat_interval=excluded.heartbeat_interval,
            runbook_markdown=excluded.runbook_markdown,
            last_seen=excluded.last_seen;
    )";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql, -1, &st.s, nullptr), db_, "prepare upsert_agent");
    sqlite3_bind_text(st.s, 1, rec.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, rec.hostname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, rec.platform.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 4, rec.curve_public_key_z85.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 5, rec.approved ? 1 : 0);
    sqlite3_bind_int(st.s, 6, rec.rejected ? 1 : 0);
    sqlite3_bind_int(st.s, 7, rec.connected ? 1 : 0);
    sqlite3_bind_int(st.s, 8, rec.maintenance ? 1 : 0);
    sqlite3_bind_text(st.s, 9, rec.maintenance_reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 10, rec.maintenance_until);
    sqlite3_bind_int(st.s, 11, rec.collection_interval);
    sqlite3_bind_int(st.s, 12, rec.process_limit);
    sqlite3_bind_int64(st.s, 13, rec.first_seen);
    sqlite3_bind_int64(st.s, 14, rec.last_seen);
    sqlite3_bind_double(st.s, 15, rec.cpu_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 16, rec.cpu_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 17, rec.cpu_critical_pct_of_avg);
    sqlite3_bind_double(st.s, 18, rec.memory_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 19, rec.memory_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 20, rec.memory_critical_pct_of_avg);
    sqlite3_bind_double(st.s, 21, rec.disk_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 22, rec.disk_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 23, rec.disk_critical_pct_of_avg);
    sqlite3_bind_double(st.s, 24, rec.network_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 25, rec.network_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 26, rec.network_critical_pct_of_avg);
    const auto collector_config_json = nlohmann::json(rec.collector_config).dump();
    sqlite3_bind_text(st.s, 27, collector_config_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 28, rec.description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 29, rec.heartbeat_interval);
    sqlite3_bind_text(st.s, 30, rec.runbook_markdown.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step upsert_agent");
}

std::optional<AgentRecord> SqliteStore::get_agent(const std::string& agent_id)
{
    const auto sql = std::string("SELECT ") + AGENT_COLUMNS + " FROM agents WHERE agent_id=?;";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr), db_, "prepare get_agent");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return row_to_agent(st.s);
    return std::nullopt;
}

std::vector<AgentRecord> SqliteStore::list_agents()
{
    const auto sql = std::string("SELECT ") + AGENT_COLUMNS + " FROM agents ORDER BY last_seen DESC;";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr), db_, "prepare list_agents");
    std::vector<AgentRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_agent(st.s));
    return out;
}

std::vector<AgentRecord> SqliteStore::list_approved_agents()
{
    const auto sql = std::string("SELECT ") + AGENT_COLUMNS +
                     " FROM agents WHERE approved=1 AND rejected=0 ORDER BY last_seen DESC;";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr), db_, "prepare list_approved_agents");
    std::vector<AgentRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_agent(st.s));
    return out;
}

std::vector<AgentRecord> SqliteStore::list_pending_agents()
{
    const auto sql = std::string("SELECT ") + AGENT_COLUMNS +
                     " FROM agents WHERE approved=0 AND rejected=0 ORDER BY first_seen DESC;";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql.c_str(), -1, &st.s, nullptr), db_, "prepare list_pending_agents");
    std::vector<AgentRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_agent(st.s));
    return out;
}

void SqliteStore::approve_agent(const std::string& agent_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE agents SET approved=1,rejected=0 WHERE agent_id=?;", -1, &st.s, nullptr), db_,
          "prepare approve_agent");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step approve_agent");
}

void SqliteStore::reject_agent(const std::string& agent_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE agents SET approved=0,rejected=1 WHERE agent_id=?;", -1, &st.s, nullptr), db_,
          "prepare reject_agent");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step reject_agent");
}

void SqliteStore::delete_agent(const std::string& agent_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM agents WHERE agent_id=?;", -1, &st.s, nullptr), db_,
          "prepare delete_agent");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step delete_agent");
}

void SqliteStore::mark_agents_offline_before(int64_t cutoff_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "UPDATE agents SET connected=0 WHERE approved=1 AND connected=1 AND last_seen>0 "
                             "AND last_seen<? AND maintenance=0;",
                             -1, &st.s, nullptr),
          db_, "prepare mark_agents_offline_before");
    sqlite3_bind_int64(st.s, 1, cutoff_ms);
    check(sqlite3_step(st.s), db_, "step mark_agents_offline_before");
}

void SqliteStore::set_agent_maintenance(const std::string& agent_id, bool maintenance, const std::string& reason,
                                        int64_t until_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "UPDATE agents SET maintenance=?,maintenance_reason=?,maintenance_until=? WHERE "
                             "agent_id=?;",
                             -1, &st.s, nullptr),
          db_, "prepare set_agent_maintenance");
    sqlite3_bind_int(st.s, 1, maintenance ? 1 : 0);
    sqlite3_bind_text(st.s, 2, reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 3, until_ms);
    sqlite3_bind_text(st.s, 4, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step set_agent_maintenance");
}

void SqliteStore::set_agent_thresholds(const AgentRecord& rec)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             R"(
                             UPDATE agents SET
                                cpu_warning_pct_of_avg=?,
                                cpu_degraded_pct_of_avg=?,
                                cpu_critical_pct_of_avg=?,
                                memory_warning_pct_of_avg=?,
                                memory_degraded_pct_of_avg=?,
                                memory_critical_pct_of_avg=?,
                                disk_warning_pct_of_avg=?,
                                disk_degraded_pct_of_avg=?,
                                disk_critical_pct_of_avg=?,
                                network_warning_pct_of_avg=?,
                                network_degraded_pct_of_avg=?,
                                network_critical_pct_of_avg=?
                             WHERE agent_id=?;
                             )",
                             -1, &st.s, nullptr),
          db_, "prepare set_agent_thresholds");
    sqlite3_bind_double(st.s, 1, rec.cpu_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 2, rec.cpu_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 3, rec.cpu_critical_pct_of_avg);
    sqlite3_bind_double(st.s, 4, rec.memory_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 5, rec.memory_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 6, rec.memory_critical_pct_of_avg);
    sqlite3_bind_double(st.s, 7, rec.disk_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 8, rec.disk_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 9, rec.disk_critical_pct_of_avg);
    sqlite3_bind_double(st.s, 10, rec.network_warning_pct_of_avg);
    sqlite3_bind_double(st.s, 11, rec.network_degraded_pct_of_avg);
    sqlite3_bind_double(st.s, 12, rec.network_critical_pct_of_avg);
    sqlite3_bind_text(st.s, 13, rec.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step set_agent_thresholds");
}

void SqliteStore::set_agent_collector_config(const std::string& agent_id, const CollectorConfig& config)
{
    const auto collector_config_json = nlohmann::json(config).dump();
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE agents SET collector_config_json=? WHERE agent_id=?;", -1, &st.s, nullptr),
          db_, "prepare set_agent_collector_config");
    sqlite3_bind_text(st.s, 1, collector_config_json.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step set_agent_collector_config");
}

std::vector<GroupRecord> SqliteStore::list_groups()
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "SELECT group_id,name,built_in FROM groups ORDER BY name;", -1, &st.s, nullptr), db_,
          "prepare list_groups");
    std::vector<GroupRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_group(st.s));
    return out;
}

int64_t SqliteStore::create_group(const std::string& name)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO groups(name) VALUES(?);", -1, &st.s, nullptr), db_,
          "prepare create_group");
    sqlite3_bind_text(st.s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step create_group");

    Stmt lookup;
    check(sqlite3_prepare_v2(db_, "SELECT group_id FROM groups WHERE name=?;", -1, &lookup.s, nullptr), db_,
          "prepare lookup_group");
    sqlite3_bind_text(lookup.s, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(lookup.s) == SQLITE_ROW)
        return sqlite3_column_int64(lookup.s, 0);
    throw std::runtime_error("group lookup failed after insert");
}

std::vector<int64_t> SqliteStore::get_agent_groups(const std::string& agent_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "SELECT group_id FROM agent_groups WHERE agent_id=? ORDER BY group_id;", -1, &st.s,
                             nullptr),
          db_, "prepare get_agent_groups");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<int64_t> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(st.s, 0));
    return out;
}

void SqliteStore::set_agent_groups(const std::string& agent_id, const std::vector<int64_t>& group_ids)
{
    exec("BEGIN IMMEDIATE;");
    try
    {
        Stmt del;
        check(sqlite3_prepare_v2(db_, "DELETE FROM agent_groups WHERE agent_id=?;", -1, &del.s, nullptr), db_,
              "prepare delete agent groups");
        sqlite3_bind_text(del.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
        check(sqlite3_step(del.s), db_, "step delete agent groups");
        for (auto group_id : group_ids)
        {
            Stmt ins;
            check(sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO agent_groups(agent_id,group_id) VALUES(?,?);", -1,
                                     &ins.s, nullptr),
                  db_, "prepare insert agent group");
            sqlite3_bind_text(ins.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins.s, 2, group_id);
            check(sqlite3_step(ins.s), db_, "step insert agent group");
        }
        exec("COMMIT;");
    }
    catch (...)
    {
        exec("ROLLBACK;");
        throw;
    }
}

std::optional<UserRecord> SqliteStore::get_user_by_username(const std::string& username)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT user_id,username,password_hash,role,built_in,disabled FROM users WHERE "
                             "username=?;",
                             -1, &st.s, nullptr),
          db_, "prepare get_user_by_username");
    sqlite3_bind_text(st.s, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return row_to_user(st.s);
    return std::nullopt;
}

std::vector<UserRecord> SqliteStore::list_users()
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT user_id,username,password_hash,role,built_in,disabled FROM users ORDER BY "
                             "username;",
                             -1, &st.s, nullptr),
          db_, "prepare list_users");
    std::vector<UserRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_user(st.s));
    return out;
}

int64_t SqliteStore::create_user(const std::string& username, const std::string& password_hash, const std::string& role)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "INSERT INTO users(username,password_hash,role) VALUES(?,?,?);", -1, &st.s, nullptr),
          db_, "prepare create_user");
    sqlite3_bind_text(st.s, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, role.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step create_user");
    return sqlite3_last_insert_rowid(db_);
}

void SqliteStore::set_user_groups(int64_t user_id, const std::vector<int64_t>& group_ids)
{
    exec("BEGIN IMMEDIATE;");
    try
    {
        Stmt del;
        check(sqlite3_prepare_v2(db_, "DELETE FROM user_groups WHERE user_id=?;", -1, &del.s, nullptr), db_,
              "prepare delete user groups");
        sqlite3_bind_int64(del.s, 1, user_id);
        check(sqlite3_step(del.s), db_, "step delete user groups");
        for (auto group_id : group_ids)
        {
            Stmt ins;
            check(sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO user_groups(user_id,group_id) VALUES(?,?);", -1,
                                     &ins.s, nullptr),
                  db_, "prepare insert user group");
            sqlite3_bind_int64(ins.s, 1, user_id);
            sqlite3_bind_int64(ins.s, 2, group_id);
            check(sqlite3_step(ins.s), db_, "step insert user group");
        }
        exec("COMMIT;");
    }
    catch (...)
    {
        exec("ROLLBACK;");
        throw;
    }
}

std::vector<int64_t> SqliteStore::get_user_groups(int64_t user_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "SELECT group_id FROM user_groups WHERE user_id=? ORDER BY group_id;", -1, &st.s,
                             nullptr),
          db_, "prepare get_user_groups");
    sqlite3_bind_int64(st.s, 1, user_id);
    std::vector<int64_t> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(sqlite3_column_int64(st.s, 0));
    return out;
}

int SqliteStore::disable_user(int64_t user_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE users SET disabled=1 WHERE user_id=? AND built_in=0;", -1, &st.s, nullptr),
          db_, "prepare disable_user");
    sqlite3_bind_int64(st.s, 1, user_id);
    check(sqlite3_step(st.s), db_, "step disable_user");
    // SEC-13/14: report affected rows so the API can reject no-ops.
    return sqlite3_changes(db_);
}

void SqliteStore::enable_user(int64_t user_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE users SET disabled=0 WHERE user_id=?;", -1, &st.s, nullptr), db_,
          "prepare enable_user");
    sqlite3_bind_int64(st.s, 1, user_id);
    check(sqlite3_step(st.s), db_, "step enable_user");
}

int SqliteStore::delete_user(int64_t user_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM users WHERE user_id=? AND built_in=0;", -1, &st.s, nullptr), db_,
          "prepare delete_user");
    sqlite3_bind_int64(st.s, 1, user_id);
    check(sqlite3_step(st.s), db_, "step delete_user");
    return sqlite3_changes(db_);
}

void SqliteStore::update_user_password(int64_t user_id, const std::string& password_hash)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE users SET password_hash=? WHERE user_id=?;", -1, &st.s, nullptr), db_,
          "prepare update_user_password");
    sqlite3_bind_text(st.s, 1, password_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, user_id);
    check(sqlite3_step(st.s), db_, "step update_user_password");
}

void SqliteStore::create_session(const SessionRecord& session)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT OR REPLACE INTO sessions(token,user_id,created_at,expires_at) "
                             "VALUES(?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare create_session");
    sqlite3_bind_text(st.s, 1, session.token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, session.user_id);
    sqlite3_bind_int64(st.s, 3, session.created_at);
    sqlite3_bind_int64(st.s, 4, session.expires_at);
    check(sqlite3_step(st.s), db_, "step create_session");
}

std::optional<SessionRecord> SqliteStore::get_session(const std::string& token, int64_t now_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT s.token,u.user_id,u.username,u.role,s.created_at,s.expires_at FROM "
                             "sessions s JOIN users u ON s.user_id=u.user_id WHERE s.token=? AND "
                             "s.expires_at>? AND u.disabled=0;",
                             -1, &st.s, nullptr),
          db_, "prepare get_session");
    sqlite3_bind_text(st.s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, now_ms);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return row_to_session(st.s);
    return std::nullopt;
}

void SqliteStore::delete_session(const std::string& token)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE token=?;", -1, &st.s, nullptr), db_,
          "prepare delete_session");
    sqlite3_bind_text(st.s, 1, token.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step delete_session");
}

void SqliteStore::insert_metrics(const MetricsRow& row)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "INSERT INTO metrics(agent_id,timestamp_ms,metrics_cbor) VALUES(?,?,?);", -1, &st.s,
                             nullptr),
          db_, "prepare insert_metrics");
    sqlite3_bind_text(st.s, 1, row.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, row.timestamp_ms);
    sqlite3_bind_blob(st.s, 3, row.metrics_cbor.data(), static_cast<int>(row.metrics_cbor.size()), SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step insert_metrics");
}

std::vector<MetricsRow> SqliteStore::get_metrics(const std::string& agent_id, int limit)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT agent_id,timestamp_ms,metrics_cbor FROM metrics WHERE agent_id=? ORDER BY "
                             "timestamp_ms DESC LIMIT ?;",
                             -1, &st.s, nullptr),
          db_, "prepare get_metrics");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 2, limit);
    std::vector<MetricsRow> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_metrics(st.s));
    return out;
}

std::vector<MetricsRow> SqliteStore::get_metrics_in_window(const std::string& agent_id, int64_t since_ms,
                                                           int64_t until_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT agent_id,timestamp_ms,metrics_cbor FROM metrics "
                             "WHERE agent_id=? AND timestamp_ms>=? AND timestamp_ms<=? "
                             "ORDER BY timestamp_ms ASC LIMIT 2880;",
                             -1, &st.s, nullptr),
          db_, "prepare get_metrics_in_window");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, since_ms);
    sqlite3_bind_int64(st.s, 3, until_ms);
    std::vector<MetricsRow> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_metrics(st.s));
    return out;
}

std::vector<MetricsRow> SqliteStore::latest_metrics()
{
    const char* sql = R"(
        SELECT m.agent_id, m.timestamp_ms, m.metrics_cbor
        FROM metrics m
        INNER JOIN (
            SELECT agent_id, MAX(timestamp_ms) AS ts FROM metrics GROUP BY agent_id
        ) latest ON m.agent_id = latest.agent_id AND m.timestamp_ms = latest.ts
        ORDER BY m.agent_id;
    )";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql, -1, &st.s, nullptr), db_, "prepare latest_metrics");
    std::vector<MetricsRow> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_metrics(st.s));
    return out;
}

void SqliteStore::insert_status_history(const StatusHistoryRow& row)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO status_history(agent_id,indicator,old_status,new_status,message,"
                             "created_at) VALUES(?,?,?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare insert_status_history");
    sqlite3_bind_text(st.s, 1, row.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, row.indicator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, row.old_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 4, row.new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 5, row.message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 6, row.created_at);
    check(sqlite3_step(st.s), db_, "step insert_status_history");
}

std::optional<StatusHistoryRow> SqliteStore::latest_status_for_indicator(const std::string& agent_id,
                                                                         const std::string& indicator)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT id,agent_id,indicator,old_status,new_status,message,created_at FROM "
                             "status_history WHERE agent_id=? AND indicator=? ORDER BY created_at DESC,id DESC "
                             "LIMIT 1;",
                             -1, &st.s, nullptr),
          db_, "prepare latest_status_for_indicator");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, indicator.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return row_to_status(st.s);
    return std::nullopt;
}

std::vector<StatusHistoryRow> SqliteStore::list_status_history(const std::string& agent_id, int limit)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT id,agent_id,indicator,old_status,new_status,message,created_at FROM "
                             "status_history WHERE agent_id=? ORDER BY created_at DESC,id DESC LIMIT ?;",
                             -1, &st.s, nullptr),
          db_, "prepare list_status_history");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 2, limit);
    std::vector<StatusHistoryRow> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_status(st.s));
    return out;
}

std::optional<PendingStatusRecord> SqliteStore::get_pending_status(const std::string& agent_id,
                                                                   const std::string& indicator)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT agent_id,indicator,target_status,count FROM pending_status WHERE agent_id=? AND "
                             "indicator=?;",
                             -1, &st.s, nullptr),
          db_, "prepare get_pending_status");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, indicator.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return row_to_pending_status(st.s);
    return std::nullopt;
}

void SqliteStore::set_pending_status(const std::string& agent_id, const std::string& indicator,
                                     const std::string& target_status, int count)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO pending_status(agent_id,indicator,target_status,count) VALUES(?,?,?,?) "
                             "ON CONFLICT(agent_id,indicator) DO UPDATE SET "
                             "target_status=excluded.target_status,count=excluded.count;",
                             -1, &st.s, nullptr),
          db_, "prepare set_pending_status");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, indicator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, target_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 4, count);
    check(sqlite3_step(st.s), db_, "step set_pending_status");
}

void SqliteStore::clear_pending_status(const std::string& agent_id, const std::string& indicator)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM pending_status WHERE agent_id=? AND indicator=?;", -1, &st.s, nullptr),
          db_, "prepare clear_pending_status");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, indicator.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step clear_pending_status");
}

int64_t SqliteStore::insert_alert(const AlertRecord& alert)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO alerts(agent_id,indicator,old_status,new_status,message,created_at,"
                             "acknowledged_by,acknowledged_at,deleted_at,note,escalated_at,runbook_url) "
                             "VALUES(?,?,?,?,?,?,?,?,?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare insert_alert");
    sqlite3_bind_text(st.s, 1, alert.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, alert.indicator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, alert.old_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 4, alert.new_status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 5, alert.message.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 6, alert.created_at);
    sqlite3_bind_text(st.s, 7, alert.acknowledged_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 8, alert.acknowledged_at);
    sqlite3_bind_int64(st.s, 9, alert.deleted_at);
    sqlite3_bind_text(st.s, 10, alert.note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 11, alert.escalated_at);
    sqlite3_bind_text(st.s, 12, alert.runbook_url.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step insert_alert");
    return sqlite3_last_insert_rowid(db_);
}

std::vector<AlertRecord> SqliteStore::list_alerts(bool include_deleted)
{
    const char* sql =
        include_deleted ? "SELECT alert_id,agent_id,indicator,old_status,new_status,message,created_at,acknowledged_by,"
                          "acknowledged_at,deleted_at,note,escalated_at,runbook_url FROM alerts ORDER BY created_at "
                          "DESC,alert_id DESC;"
                        : "SELECT alert_id,agent_id,indicator,old_status,new_status,message,created_at,acknowledged_by,"
                          "acknowledged_at,deleted_at,note,escalated_at,runbook_url FROM alerts WHERE deleted_at=0 "
                          "ORDER BY created_at "
                          "DESC,alert_id DESC;";
    Stmt st;
    check(sqlite3_prepare_v2(db_, sql, -1, &st.s, nullptr), db_, "prepare list_alerts");
    std::vector<AlertRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_alert(st.s));
    return out;
}

std::vector<AlertRecord> SqliteStore::list_unacknowledged_alerts()
{
    Stmt st;
    check(
        sqlite3_prepare_v2(db_,
                           "SELECT alert_id,agent_id,indicator,old_status,new_status,message,created_at,"
                           "acknowledged_by,acknowledged_at,deleted_at,note,escalated_at,runbook_url FROM alerts WHERE "
                           "deleted_at=0 AND acknowledged_at=0 ORDER BY created_at DESC,alert_id DESC;",
                           -1, &st.s, nullptr),
        db_, "prepare list_unacknowledged_alerts");
    std::vector<AlertRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_alert(st.s));
    return out;
}

void SqliteStore::acknowledge_alert(int64_t alert_id, const std::string& username, int64_t acknowledged_at,
                                    const std::string& note)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "UPDATE alerts SET acknowledged_by=?,acknowledged_at=?,note=? WHERE alert_id=? AND "
                             "deleted_at=0;",
                             -1, &st.s, nullptr),
          db_, "prepare acknowledge_alert");
    sqlite3_bind_text(st.s, 1, username.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, acknowledged_at);
    sqlite3_bind_text(st.s, 3, note.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 4, alert_id);
    check(sqlite3_step(st.s), db_, "step acknowledge_alert");
}

void SqliteStore::bulk_acknowledge_alerts(const std::vector<int64_t>& alert_ids, const std::string& username,
                                          int64_t acknowledged_at, const std::string& note)
{
    for (const auto id : alert_ids)
        acknowledge_alert(id, username, acknowledged_at, note);
}

void SqliteStore::bulk_soft_delete_alerts(const std::vector<int64_t>& alert_ids, int64_t deleted_at)
{
    for (const auto id : alert_ids)
        soft_delete_alert(id, deleted_at);
}

void SqliteStore::soft_delete_alert(int64_t alert_id, int64_t deleted_at)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE alerts SET deleted_at=? WHERE alert_id=?;", -1, &st.s, nullptr), db_,
          "prepare soft_delete_alert");
    sqlite3_bind_int64(st.s, 1, deleted_at);
    sqlite3_bind_int64(st.s, 2, alert_id);
    check(sqlite3_step(st.s), db_, "step soft_delete_alert");
}

void SqliteStore::clear_active_alerts_for_agent(const std::string& agent_id, int64_t cleared_at)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "UPDATE alerts SET acknowledged_by='maintenance',acknowledged_at=? WHERE agent_id=? "
                             "AND acknowledged_at=0 AND deleted_at=0;",
                             -1, &st.s, nullptr),
          db_, "prepare clear_active_alerts_for_agent");
    sqlite3_bind_int64(st.s, 1, cleared_at);
    sqlite3_bind_text(st.s, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step clear_active_alerts_for_agent");
}

void SqliteStore::set_agent_description(const std::string& agent_id, const std::string& description)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE agents SET description=? WHERE agent_id=?;", -1, &st.s, nullptr), db_,
          "prepare set_agent_description");
    sqlite3_bind_text(st.s, 1, description.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step set_agent_description");
}

void SqliteStore::set_agent_runbook(const std::string& agent_id, const std::string& markdown)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE agents SET runbook_markdown=? WHERE agent_id=?;", -1, &st.s, nullptr), db_,
          "prepare set_agent_runbook");
    sqlite3_bind_text(st.s, 1, markdown.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step set_agent_runbook");
}

std::vector<std::string> SqliteStore::get_offline_unalerted_agent_ids()
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT a.agent_id FROM agents a WHERE a.approved=1 AND a.connected=0 "
                             "AND a.maintenance=0 AND a.last_seen>0 "
                             "AND NOT EXISTS ("
                             "  SELECT 1 FROM alerts al WHERE al.agent_id=a.agent_id "
                             "  AND al.indicator='Heartbeat' AND al.deleted_at=0"
                             ");",
                             -1, &st.s, nullptr),
          db_, "prepare get_offline_unalerted_agent_ids");
    std::vector<std::string> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(text_col(st.s, 0));
    return out;
}

void SqliteStore::archive_heartbeat_alerts_for_agent(const std::string& agent_id, int64_t deleted_at)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "UPDATE alerts SET deleted_at=? WHERE agent_id=? AND indicator='Heartbeat' "
                             "AND deleted_at=0;",
                             -1, &st.s, nullptr),
          db_, "prepare archive_heartbeat_alerts_for_agent");
    sqlite3_bind_int64(st.s, 1, deleted_at);
    sqlite3_bind_text(st.s, 2, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step archive_heartbeat_alerts_for_agent");
}

void SqliteStore::escalate_old_alerts(int64_t cutoff_ms, int64_t now_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "UPDATE alerts SET escalated_at=? WHERE deleted_at=0 AND acknowledged_at=0 AND "
                             "escalated_at=0 AND created_at<?;",
                             -1, &st.s, nullptr),
          db_, "prepare escalate_old_alerts");
    sqlite3_bind_int64(st.s, 1, now_ms);
    sqlite3_bind_int64(st.s, 2, cutoff_ms);
    check(sqlite3_step(st.s), db_, "step escalate_old_alerts");
}

int64_t SqliteStore::count_metrics_in_window(const std::string& agent_id, int64_t since_ms, int64_t until_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT COUNT(*) FROM metrics WHERE agent_id=? AND timestamp_ms>=? AND timestamp_ms<=?;",
                             -1, &st.s, nullptr),
          db_, "prepare count_metrics_in_window");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, since_ms);
    sqlite3_bind_int64(st.s, 3, until_ms);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return sqlite3_column_int64(st.s, 0);
    return 0;
}

int64_t SqliteStore::create_maintenance_window(const MaintenanceWindowRecord& rec)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO maintenance_windows(agent_id,start_ms,end_ms,reason,created_by,created_at) "
                             "VALUES(?,?,?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare create_maintenance_window");
    sqlite3_bind_text(st.s, 1, rec.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, rec.start_ms);
    sqlite3_bind_int64(st.s, 3, rec.end_ms);
    sqlite3_bind_text(st.s, 4, rec.reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 5, rec.created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 6, rec.created_at);
    check(sqlite3_step(st.s), db_, "step create_maintenance_window");
    return sqlite3_last_insert_rowid(db_);
}

std::vector<MaintenanceWindowRecord> SqliteStore::list_maintenance_windows()
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT window_id,agent_id,start_ms,end_ms,reason,created_by,created_at FROM "
                             "maintenance_windows ORDER BY start_ms DESC;",
                             -1, &st.s, nullptr),
          db_, "prepare list_maintenance_windows");
    std::vector<MaintenanceWindowRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_maintenance_window(st.s));
    return out;
}

void SqliteStore::delete_maintenance_window(int64_t window_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM maintenance_windows WHERE window_id=?;", -1, &st.s, nullptr), db_,
          "prepare delete_maintenance_window");
    sqlite3_bind_int64(st.s, 1, window_id);
    check(sqlite3_step(st.s), db_, "step delete_maintenance_window");
}

std::vector<MaintenanceWindowRecord> SqliteStore::active_maintenance_windows(int64_t now_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT window_id,agent_id,start_ms,end_ms,reason,created_by,created_at FROM "
                             "maintenance_windows WHERE start_ms<=? AND end_ms>? ORDER BY start_ms DESC;",
                             -1, &st.s, nullptr),
          db_, "prepare active_maintenance_windows");
    sqlite3_bind_int64(st.s, 1, now_ms);
    sqlite3_bind_int64(st.s, 2, now_ms);
    std::vector<MaintenanceWindowRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_maintenance_window(st.s));
    return out;
}

std::string SqliteStore::get_setting(const std::string& key, const std::string& fallback)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "SELECT value FROM server_settings WHERE key=?;", -1, &st.s, nullptr), db_,
          "prepare get_setting");
    sqlite3_bind_text(st.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return text_col(st.s, 0);
    return fallback;
}

void SqliteStore::set_setting(const std::string& key, const std::string& value)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO server_settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET "
                             "value=excluded.value;",
                             -1, &st.s, nullptr),
          db_, "prepare set_setting");
    sqlite3_bind_text(st.s, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    check(sqlite3_step(st.s), db_, "step set_setting");
}

namespace
{
    SilenceRecord row_to_silence(sqlite3_stmt* s)
    {
        SilenceRecord r;
        r.silence_id = sqlite3_column_int64(s, 0);
        r.agent_id = text_col(s, 1);
        r.indicator = text_col(s, 2);
        r.reason = text_col(s, 3);
        r.until_ms = sqlite3_column_int64(s, 4);
        r.created_by = text_col(s, 5);
        r.created_at = sqlite3_column_int64(s, 6);
        return r;
    }
} // namespace

int64_t SqliteStore::create_silence(const SilenceRecord& rec)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO silences(agent_id,indicator,reason,until_ms,created_by,created_at) "
                             "VALUES(?,?,?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare create_silence");
    sqlite3_bind_text(st.s, 1, rec.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, rec.indicator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, rec.reason.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 4, rec.until_ms);
    sqlite3_bind_text(st.s, 5, rec.created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 6, rec.created_at);
    check(sqlite3_step(st.s), db_, "step create_silence");
    return sqlite3_last_insert_rowid(db_);
}

std::vector<SilenceRecord> SqliteStore::list_silences()
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT silence_id,agent_id,indicator,reason,until_ms,created_by,created_at "
                             "FROM silences ORDER BY created_at DESC;",
                             -1, &st.s, nullptr),
          db_, "prepare list_silences");
    std::vector<SilenceRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_silence(st.s));
    return out;
}

void SqliteStore::delete_silence(int64_t silence_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM silences WHERE silence_id=?;", -1, &st.s, nullptr), db_,
          "prepare delete_silence");
    sqlite3_bind_int64(st.s, 1, silence_id);
    check(sqlite3_step(st.s), db_, "step delete_silence");
}

bool SqliteStore::is_silenced(const std::string& agent_id, const std::string& indicator, int64_t now_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT 1 FROM silences "
                             "WHERE (agent_id='*' OR agent_id=?) "
                             "  AND (indicator='*' OR indicator=?) "
                             "  AND until_ms > ? LIMIT 1;",
                             -1, &st.s, nullptr),
          db_, "prepare is_silenced");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, indicator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 3, now_ms);
    return sqlite3_step(st.s) == SQLITE_ROW;
}

void SqliteStore::prune_metrics_before(int64_t cutoff_ms)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM metrics WHERE timestamp_ms < ?;", -1, &st.s, nullptr), db_,
          "prepare prune_metrics_before");
    sqlite3_bind_int64(st.s, 1, cutoff_ms);
    check(sqlite3_step(st.s), db_, "step prune_metrics_before");
}

void SqliteStore::insert_log_match(const LogMatchRecord& rec)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO log_matches(agent_id,indicator_name,path,matched_line,severity,created_at) "
                             "VALUES(?,?,?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare insert_log_match");
    sqlite3_bind_text(st.s, 1, rec.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, rec.indicator_name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, rec.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 4, rec.matched_line.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 5, rec.severity.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 6, rec.created_at);
    check(sqlite3_step(st.s), db_, "step insert_log_match");
}

std::vector<LogMatchRecord> SqliteStore::list_log_matches(const std::string& agent_id, int limit)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT match_id,agent_id,indicator_name,path,matched_line,severity,created_at "
                             "FROM log_matches WHERE agent_id=? ORDER BY created_at DESC LIMIT ?;",
                             -1, &st.s, nullptr),
          db_, "prepare list_log_matches");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 2, limit);
    std::vector<LogMatchRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
    {
        LogMatchRecord r;
        r.match_id = sqlite3_column_int64(st.s, 0);
        r.agent_id = text_col(st.s, 1);
        r.indicator_name = text_col(st.s, 2);
        r.path = text_col(st.s, 3);
        r.matched_line = text_col(st.s, 4);
        r.severity = text_col(st.s, 5);
        r.created_at = sqlite3_column_int64(st.s, 6);
        out.push_back(std::move(r));
    }
    return out;
}

// ── Views ─────────────────────────────────────────────────────────────────────

namespace
{
    nlohmann::json view_to_config_json(const ViewRecord& rec)
    {
        nlohmann::json j;
        j["agent_ids"] = rec.agent_ids;
        j["group_id"] = rec.group_id;
        return j;
    }

    ViewRecord row_to_view(sqlite3_stmt* s)
    {
        ViewRecord r;
        r.view_id = sqlite3_column_int64(s, 0);
        r.name = text_col(s, 1);
        r.owner_user_id = sqlite3_column_int64(s, 2);
        r.owner_username = text_col(s, 3);
        r.is_public = sqlite3_column_int(s, 4) != 0;
        r.group_id = sqlite3_column_int64(s, 5);
        r.created_at = sqlite3_column_int64(s, 6);
        try
        {
            const auto cfg = nlohmann::json::parse(text_col(s, 7));
            if (cfg.contains("agent_ids") && cfg["agent_ids"].is_array())
                r.agent_ids = cfg["agent_ids"].get<std::vector<std::string>>();
            if (r.group_id == 0 && cfg.contains("group_id"))
                r.group_id = cfg["group_id"].get<int64_t>();
        }
        catch (...)
        {
        }
        return r;
    }
} // namespace

int64_t SqliteStore::create_view(const ViewRecord& rec)
{
    const auto cfg = view_to_config_json(rec).dump();
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO views(name,owner_user_id,is_public,group_id,config_json,created_at) "
                             "VALUES(?,?,?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare create_view");
    sqlite3_bind_text(st.s, 1, rec.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 2, rec.owner_user_id);
    sqlite3_bind_int(st.s, 3, rec.is_public ? 1 : 0);
    sqlite3_bind_int64(st.s, 4, rec.group_id);
    sqlite3_bind_text(st.s, 5, cfg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 6, rec.created_at);
    check(sqlite3_step(st.s), db_, "step create_view");
    return sqlite3_last_insert_rowid(db_);
}

std::optional<ViewRecord> SqliteStore::get_view(int64_t view_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT v.view_id,v.name,v.owner_user_id,COALESCE(u.username,''),v.is_public,"
                             "v.group_id,v.created_at,v.config_json "
                             "FROM views v LEFT JOIN users u ON u.user_id=v.owner_user_id "
                             "WHERE v.view_id=?;",
                             -1, &st.s, nullptr),
          db_, "prepare get_view");
    sqlite3_bind_int64(st.s, 1, view_id);
    if (sqlite3_step(st.s) != SQLITE_ROW)
        return std::nullopt;
    return row_to_view(st.s);
}

std::vector<ViewRecord> SqliteStore::list_views(int64_t user_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT v.view_id,v.name,v.owner_user_id,COALESCE(u.username,''),v.is_public,"
                             "v.group_id,v.created_at,v.config_json "
                             "FROM views v LEFT JOIN users u ON u.user_id=v.owner_user_id "
                             "WHERE ? <= 0 OR v.owner_user_id=? OR v.is_public=1 OR EXISTS ("
                             "SELECT 1 FROM user_groups ug WHERE ug.user_id=? AND ug.group_id=v.group_id) "
                             "ORDER BY v.created_at ASC;",
                             -1, &st.s, nullptr),
          db_, "prepare list_views");
    sqlite3_bind_int64(st.s, 1, user_id);
    sqlite3_bind_int64(st.s, 2, user_id);
    sqlite3_bind_int64(st.s, 3, user_id);
    std::vector<ViewRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_view(st.s));
    return out;
}

void SqliteStore::update_view(const ViewRecord& rec)
{
    const auto cfg = view_to_config_json(rec).dump();
    Stmt st;
    check(sqlite3_prepare_v2(db_, "UPDATE views SET name=?,is_public=?,group_id=?,config_json=? WHERE view_id=?;", -1,
                             &st.s, nullptr),
          db_, "prepare update_view");
    sqlite3_bind_text(st.s, 1, rec.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 2, rec.is_public ? 1 : 0);
    sqlite3_bind_int64(st.s, 3, rec.group_id);
    sqlite3_bind_text(st.s, 4, cfg.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 5, rec.view_id);
    check(sqlite3_step(st.s), db_, "step update_view");
}

void SqliteStore::delete_view(int64_t view_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM views WHERE view_id=?;", -1, &st.s, nullptr), db_,
          "prepare delete_view");
    sqlite3_bind_int64(st.s, 1, view_id);
    check(sqlite3_step(st.s), db_, "step delete_view");
}

// ── Runbooks ──────────────────────────────────────────────────────────────────

namespace
{
    RunbookRecord row_to_runbook(sqlite3_stmt* s)
    {
        RunbookRecord r;
        r.runbook_id = sqlite3_column_int64(s, 0);
        r.agent_id = text_col(s, 1);
        r.indicator = text_col(s, 2);
        r.status = text_col(s, 3);
        r.url = text_col(s, 4);
        r.markdown = text_col(s, 5);
        r.notes = text_col(s, 6);
        r.created_by = text_col(s, 7);
        r.created_at = sqlite3_column_int64(s, 8);
        return r;
    }
} // namespace

int64_t SqliteStore::create_runbook(const RunbookRecord& rec)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "INSERT INTO runbooks(agent_id,indicator,status,url,markdown,notes,created_by,created_at) "
                             "VALUES(?,?,?,?,?,?,?,?);",
                             -1, &st.s, nullptr),
          db_, "prepare create_runbook");
    sqlite3_bind_text(st.s, 1, rec.agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, rec.indicator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, rec.status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 4, rec.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 5, rec.markdown.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 6, rec.notes.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 7, rec.created_by.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st.s, 8, rec.created_at);
    check(sqlite3_step(st.s), db_, "step create_runbook");
    return sqlite3_last_insert_rowid(db_);
}

std::vector<RunbookRecord> SqliteStore::list_runbooks()
{
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT runbook_id,agent_id,indicator,status,url,markdown,notes,created_by,created_at "
                             "FROM runbooks ORDER BY created_at ASC;",
                             -1, &st.s, nullptr),
          db_, "prepare list_runbooks");
    std::vector<RunbookRecord> out;
    while (sqlite3_step(st.s) == SQLITE_ROW)
        out.push_back(row_to_runbook(st.s));
    return out;
}

void SqliteStore::delete_runbook(int64_t runbook_id)
{
    Stmt st;
    check(sqlite3_prepare_v2(db_, "DELETE FROM runbooks WHERE runbook_id=?;", -1, &st.s, nullptr), db_,
          "prepare delete_runbook");
    sqlite3_bind_int64(st.s, 1, runbook_id);
    check(sqlite3_step(st.s), db_, "step delete_runbook");
}

std::optional<RunbookRecord> SqliteStore::get_runbook(const std::string& agent_id, const std::string& indicator,
                                                     const std::string& status)
{
    // SEC-11: only this agent's runbooks or generic ones (agent_id='') may be
    // attached, so an alert never carries another agent's runbook URL. Prefer
    // an exact indicator over '*', then an agent-specific runbook over a
    // generic one, then the newest; limit to 1.
    Stmt st;
    check(sqlite3_prepare_v2(db_,
                             "SELECT runbook_id,agent_id,indicator,status,url,markdown,notes,created_by,created_at "
                             "FROM runbooks "
                             "WHERE (agent_id=? OR agent_id='') AND (indicator=? OR indicator='*') AND status=? "
                             "ORDER BY CASE WHEN indicator='*' THEN 1 ELSE 0 END, "
                             "CASE WHEN agent_id='' THEN 1 ELSE 0 END, runbook_id DESC "
                             "LIMIT 1;",
                             -1, &st.s, nullptr),
          db_, "prepare get_runbook");
    sqlite3_bind_text(st.s, 1, agent_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, indicator.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 3, status.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) == SQLITE_ROW)
        return row_to_runbook(st.s);
    return std::nullopt;
}

std::unique_ptr<Store> make_store(const std::string& db_type, const std::string& db_path_or_dsn)
{
    LOGF_DEBUG("Creating store db_type=%s target=%s", db_type.c_str(), db_path_or_dsn.c_str());
    if (db_type == "sqlite")
        return std::make_unique<SqliteStore>(db_path_or_dsn);
#ifdef HAVE_LIBPQ
    if (db_type == "postgres")
        return std::make_unique<PostgresStore>(db_path_or_dsn);
#endif
    throw std::runtime_error("Unsupported db_type: " + db_type + " (compile with postgres support)");
}

} // namespace thewatcher::server
