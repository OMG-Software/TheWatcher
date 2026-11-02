#ifdef HAVE_LIBPQ

#include "store_postgres.hpp"

#include "common/SingleLog.hpp"

#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>
#include <sodium.h>

namespace thewatcher::server
{

// ── libpq helpers ─────────────────────────────────────────────────────────────

namespace
{
    // RAII wrapper for PGresult.
    struct Res
    {
        PGresult* r = nullptr;
        explicit Res(PGresult* p)
            : r(p)
        {
        }
        ~Res()
        {
            if (r)
                PQclear(r);
        }
        Res(const Res&) = delete;
        Res& operator=(const Res&) = delete;
        PGresult* operator->()
        {
            return r;
        }
    };

    void check_res(PGresult* res, const char* ctx)
    {
        const auto status = PQresultStatus(res);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK)
        {
            // Do NOT call PQclear here — the caller wraps res in a Res RAII
            // guard whose destructor will free it during stack unwinding.
            // Calling PQclear here would cause a double-free.
            throw std::runtime_error(std::string(ctx) + ": " + PQresultErrorMessage(res));
        }
    }

    std::string col(PGresult* r, int row, int col_idx)
    {
        if (PQgetisnull(r, row, col_idx))
            return {};
        return PQgetvalue(r, row, col_idx);
    }

    int64_t col_int64(PGresult* r, int row, int col_idx)
    {
        if (PQgetisnull(r, row, col_idx))
            return 0;
        return std::stoll(PQgetvalue(r, row, col_idx));
    }

    int col_int(PGresult* r, int row, int col_idx)
    {
        if (PQgetisnull(r, row, col_idx))
            return 0;
        return std::stoi(PQgetvalue(r, row, col_idx));
    }

    double col_double(PGresult* r, int row, int col_idx)
    {
        if (PQgetisnull(r, row, col_idx))
            return 0.0;
        return std::stod(PQgetvalue(r, row, col_idx));
    }

    bool col_bool(PGresult* r, int row, int col_idx)
    {
        if (PQgetisnull(r, row, col_idx))
            return false;
        const char* v = PQgetvalue(r, row, col_idx);
        return v[0] == 't' || v[0] == '1';
    }

    // Decode hex BYTEA (\xABCD…) from text mode.
    std::vector<uint8_t> col_bytea(PGresult* r, int row, int col_idx)
    {
        if (PQgetisnull(r, row, col_idx))
            return {};
        const char* val = PQgetvalue(r, row, col_idx);
        // libpq text-mode BYTEA returns \x<hex> in PostgreSQL 9.0+
        if (val[0] == '\\' && val[1] == 'x')
        {
            val += 2;
            size_t hex_len = strlen(val);
            std::vector<uint8_t> out(hex_len / 2);
            for (size_t i = 0; i < out.size(); ++i)
            {
                unsigned int byte = 0;
                sscanf(val + 2 * i, "%02x", &byte);
                out[i] = static_cast<uint8_t>(byte);
            }
            return out;
        }
        // fallback: treat as raw bytes
        const auto* p = reinterpret_cast<const uint8_t*>(val);
        return std::vector<uint8_t>(p, p + strlen(val));
    }

    // Encode bytes to \x<hex> for BYTEA parameter.
    std::string to_bytea_hex(const std::vector<uint8_t>& data)
    {
        std::string out;
        out.reserve(2 + data.size() * 2);
        out = "\\x";
        static constexpr char HEX[] = "0123456789abcdef";
        for (uint8_t b : data)
        {
            out.push_back(HEX[b >> 4]);
            out.push_back(HEX[b & 0xF]);
        }
        return out;
    }

    std::string s(int64_t v)
    {
        return std::to_string(v);
    }
    std::string s(int v)
    {
        return std::to_string(v);
    }
    std::string s(double v)
    {
        // Use enough precision to survive a round-trip.
        char buf[64];
        snprintf(buf, sizeof(buf), "%.17g", v);
        return buf;
    }
    std::string s(bool v)
    {
        return v ? "true" : "false";
    }

    // Strip the password value from a libpq connection string so it is safe
    // to include in log output.
    std::string redact_dsn(const std::string& dsn)
    {
        std::string out = dsn;
        const auto kw = out.find("password=");
        if (kw != std::string::npos)
        {
            const auto val_start = kw + 9;
            const auto val_end = out.find(' ', val_start);
            out.replace(val_start, val_end == std::string::npos ? std::string::npos : val_end - val_start, "***");
        }
        return out;
    }

    AgentRecord row_to_agent(PGresult* r, int row)
    {
        AgentRecord a;
        a.agent_id = col(r, row, 0);
        a.hostname = col(r, row, 1);
        a.platform = col(r, row, 2);
        a.curve_public_key_z85 = col(r, row, 3);
        a.approved = col_bool(r, row, 4);
        a.rejected = col_bool(r, row, 5);
        a.connected = col_bool(r, row, 6);
        a.maintenance = col_bool(r, row, 7);
        a.maintenance_reason = col(r, row, 8);
        a.maintenance_until = col_int64(r, row, 9);
        a.collection_interval = col_int(r, row, 10);
        a.process_limit = col_int(r, row, 11);
        a.first_seen = col_int64(r, row, 12);
        a.last_seen = col_int64(r, row, 13);
        a.cpu_warning_pct_of_avg = col_double(r, row, 14);
        a.cpu_degraded_pct_of_avg = col_double(r, row, 15);
        a.cpu_critical_pct_of_avg = col_double(r, row, 16);
        a.memory_warning_pct_of_avg = col_double(r, row, 17);
        a.memory_degraded_pct_of_avg = col_double(r, row, 18);
        a.memory_critical_pct_of_avg = col_double(r, row, 19);
        a.disk_warning_pct_of_avg = col_double(r, row, 20);
        a.disk_degraded_pct_of_avg = col_double(r, row, 21);
        a.disk_critical_pct_of_avg = col_double(r, row, 22);
        a.network_warning_pct_of_avg = col_double(r, row, 23);
        a.network_degraded_pct_of_avg = col_double(r, row, 24);
        a.network_critical_pct_of_avg = col_double(r, row, 25);
        const auto cfg_json = col(r, row, 26);
        if (!cfg_json.empty())
        {
            try
            {
                a.collector_config = nlohmann::json::parse(cfg_json).get<CollectorConfig>();
            }
            catch (...)
            {
                a.collector_config = default_collector_config();
            }
        }
        a.description = col(r, row, 27);
        a.heartbeat_interval = col_int(r, row, 28);
        a.runbook_markdown = col(r, row, 29);
        return a;
    }

    AlertRecord row_to_alert(PGresult* r, int row)
    {
        return {
            col_int64(r, row, 0),  // alert_id
            col(r, row, 1),        // agent_id
            col(r, row, 2),        // indicator
            col(r, row, 3),        // old_status
            col(r, row, 4),        // new_status
            col(r, row, 5),        // message
            col_int64(r, row, 6),  // created_at
            col(r, row, 7),        // acknowledged_by
            col_int64(r, row, 8),  // acknowledged_at
            col_int64(r, row, 9),  // deleted_at
            col(r, row, 10),       // note
            col_int64(r, row, 11), // escalated_at
            col(r, row, 12),       // runbook_url
            {},                    // runbook_markdown
        };
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

// ── Construction ──────────────────────────────────────────────────────────────

PostgresStore::PostgresStore(const std::string& dsn)
{
    LOG_FUNCTION_TRACE
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium initialization failed");
    LOGF_DEBUG("Opening PostgreSQL store dsn=%s", redact_dsn(dsn).c_str());
    conn_ = PQconnectdb(dsn.c_str());
    if (!conn_ || PQstatus(conn_) != CONNECTION_OK)
    {
        const std::string msg = conn_ ? PQerrorMessage(conn_) : "out of memory";
        if (conn_)
            PQfinish(conn_);
        conn_ = nullptr;
        throw std::runtime_error("PostgreSQL connect failed: " + msg);
    }
    init_schema();
    bootstrap_defaults();
}

PostgresStore::~PostgresStore()
{
    LOG_FUNCTION_TRACE
    if (conn_)
    {
        PQfinish(conn_);
        LOG_DEBUG("PostgreSQL store closed");
    }
}

void PostgresStore::exec(const char* sql)
{
    LOGF_TRACE("Executing SQL prefix=%.80s", sql);
    Res res(PQexec(conn_, sql));
    check_res(res.r, "exec");
}

bool PostgresStore::column_exists(const std::string& table, const std::string& column)
{
    const char* params[2] = {table.c_str(), column.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT 1 FROM information_schema.columns "
                         "WHERE table_name=$1 AND column_name=$2 AND table_schema='public';",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "column_exists");
    return PQntuples(res.r) > 0;
}

void PostgresStore::add_column_if_missing(const std::string& table, const std::string& column, const char* ddl)
{
    if (!column_exists(table, column))
        exec(ddl);
}

void PostgresStore::init_schema()
{
    LOG_FUNCTION_TRACE
    exec(R"(
        CREATE TABLE IF NOT EXISTS agents (
            agent_id             TEXT PRIMARY KEY,
            hostname             TEXT NOT NULL DEFAULT '',
            platform             TEXT NOT NULL DEFAULT '',
            curve_public_key_z85 TEXT NOT NULL DEFAULT '',
            approved             BOOLEAN NOT NULL DEFAULT FALSE,
            rejected             BOOLEAN NOT NULL DEFAULT FALSE,
            connected            BOOLEAN NOT NULL DEFAULT FALSE,
            maintenance          BOOLEAN NOT NULL DEFAULT FALSE,
            maintenance_reason   TEXT NOT NULL DEFAULT '',
            maintenance_until    BIGINT NOT NULL DEFAULT 0,
            collection_interval  INTEGER NOT NULL DEFAULT 30,
            process_limit        INTEGER NOT NULL DEFAULT 25,
            first_seen           BIGINT NOT NULL DEFAULT 0,
            last_seen            BIGINT NOT NULL DEFAULT 0,
            cpu_warning_pct_of_avg      DOUBLE PRECISION NOT NULL DEFAULT 125.0,
            cpu_degraded_pct_of_avg     DOUBLE PRECISION NOT NULL DEFAULT 150.0,
            cpu_critical_pct_of_avg     DOUBLE PRECISION NOT NULL DEFAULT 200.0,
            memory_warning_pct_of_avg   DOUBLE PRECISION NOT NULL DEFAULT 125.0,
            memory_degraded_pct_of_avg  DOUBLE PRECISION NOT NULL DEFAULT 150.0,
            memory_critical_pct_of_avg  DOUBLE PRECISION NOT NULL DEFAULT 200.0,
            disk_warning_pct_of_avg     DOUBLE PRECISION NOT NULL DEFAULT 125.0,
            disk_degraded_pct_of_avg    DOUBLE PRECISION NOT NULL DEFAULT 150.0,
            disk_critical_pct_of_avg    DOUBLE PRECISION NOT NULL DEFAULT 200.0,
            network_warning_pct_of_avg  DOUBLE PRECISION NOT NULL DEFAULT 125.0,
            network_degraded_pct_of_avg DOUBLE PRECISION NOT NULL DEFAULT 150.0,
            network_critical_pct_of_avg DOUBLE PRECISION NOT NULL DEFAULT 200.0,
            collector_config_json       TEXT NOT NULL DEFAULT '',
            description                 TEXT NOT NULL DEFAULT '',
            heartbeat_interval          INTEGER NOT NULL DEFAULT 5,
            runbook_markdown            TEXT NOT NULL DEFAULT ''
        );
    )");
    add_column_if_missing("agents", "rejected",
                          "ALTER TABLE agents ADD COLUMN rejected BOOLEAN NOT NULL DEFAULT FALSE;");
    add_column_if_missing("agents", "description",
                          "ALTER TABLE agents ADD COLUMN description TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("agents", "connected",
                          "ALTER TABLE agents ADD COLUMN connected BOOLEAN NOT NULL DEFAULT FALSE;");
    add_column_if_missing("agents", "maintenance",
                          "ALTER TABLE agents ADD COLUMN maintenance BOOLEAN NOT NULL DEFAULT FALSE;");
    add_column_if_missing("agents", "maintenance_reason",
                          "ALTER TABLE agents ADD COLUMN maintenance_reason TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("agents", "maintenance_until",
                          "ALTER TABLE agents ADD COLUMN maintenance_until BIGINT NOT NULL DEFAULT 0;");
    add_column_if_missing("agents", "collection_interval",
                          "ALTER TABLE agents ADD COLUMN collection_interval INTEGER NOT NULL DEFAULT 30;");
    add_column_if_missing("agents", "process_limit",
                          "ALTER TABLE agents ADD COLUMN process_limit INTEGER NOT NULL DEFAULT 25;");
    add_column_if_missing("agents", "collector_config_json",
                          "ALTER TABLE agents ADD COLUMN collector_config_json TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("agents", "heartbeat_interval",
                          "ALTER TABLE agents ADD COLUMN heartbeat_interval INTEGER NOT NULL DEFAULT 5;");
    add_column_if_missing("agents", "runbook_markdown",
                          "ALTER TABLE agents ADD COLUMN runbook_markdown TEXT NOT NULL DEFAULT '';");
    exec(R"(
        CREATE TABLE IF NOT EXISTS metrics (
            id           BIGSERIAL PRIMARY KEY,
            agent_id     TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
            timestamp_ms BIGINT NOT NULL,
            metrics_cbor BYTEA NOT NULL
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_metrics_agent_ts ON metrics(agent_id, timestamp_ms DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS groups (
            group_id BIGSERIAL PRIMARY KEY,
            name     TEXT NOT NULL UNIQUE,
            built_in BOOLEAN NOT NULL DEFAULT FALSE
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS users (
            user_id       BIGSERIAL PRIMARY KEY,
            username      TEXT NOT NULL UNIQUE,
            password_hash TEXT NOT NULL,
            role          TEXT NOT NULL,
            built_in      BOOLEAN NOT NULL DEFAULT FALSE,
            disabled      BOOLEAN NOT NULL DEFAULT FALSE
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS user_groups (
            user_id  BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
            group_id BIGINT NOT NULL REFERENCES groups(group_id) ON DELETE CASCADE,
            PRIMARY KEY(user_id, group_id)
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS agent_groups (
            agent_id TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
            group_id BIGINT NOT NULL REFERENCES groups(group_id) ON DELETE CASCADE,
            PRIMARY KEY(agent_id, group_id)
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS sessions (
            token      TEXT PRIMARY KEY,
            user_id    BIGINT NOT NULL REFERENCES users(user_id) ON DELETE CASCADE,
            created_at BIGINT NOT NULL,
            expires_at BIGINT NOT NULL
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS status_history (
            id         BIGSERIAL PRIMARY KEY,
            agent_id   TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
            indicator  TEXT NOT NULL,
            old_status TEXT NOT NULL,
            new_status TEXT NOT NULL,
            message    TEXT NOT NULL DEFAULT '',
            created_at BIGINT NOT NULL
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_status_history_agent_indicator "
         "ON status_history(agent_id, indicator, created_at DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS pending_status (
            agent_id      TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
            indicator     TEXT NOT NULL,
            target_status TEXT NOT NULL,
            count         INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(agent_id, indicator)
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS alerts (
            alert_id        BIGSERIAL PRIMARY KEY,
            agent_id        TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
            indicator       TEXT NOT NULL,
            old_status      TEXT NOT NULL,
            new_status      TEXT NOT NULL,
            message         TEXT NOT NULL DEFAULT '',
            created_at      BIGINT NOT NULL,
            acknowledged_by TEXT NOT NULL DEFAULT '',
            acknowledged_at BIGINT NOT NULL DEFAULT 0,
            deleted_at      BIGINT NOT NULL DEFAULT 0,
            note            TEXT NOT NULL DEFAULT '',
            escalated_at    BIGINT NOT NULL DEFAULT 0,
            runbook_url     TEXT NOT NULL DEFAULT ''
        );
    )");
    add_column_if_missing("alerts", "note", "ALTER TABLE alerts ADD COLUMN note TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("alerts", "escalated_at",
                          "ALTER TABLE alerts ADD COLUMN escalated_at BIGINT NOT NULL DEFAULT 0;");
    add_column_if_missing("alerts", "runbook_url",
                          "ALTER TABLE alerts ADD COLUMN runbook_url TEXT NOT NULL DEFAULT '';");
    exec("CREATE INDEX IF NOT EXISTS idx_alerts_active "
         "ON alerts(deleted_at, acknowledged_at, created_at DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS maintenance_windows (
            window_id  BIGSERIAL PRIMARY KEY,
            agent_id   TEXT NOT NULL DEFAULT '*',
            start_ms   BIGINT NOT NULL,
            end_ms     BIGINT NOT NULL,
            reason     TEXT NOT NULL DEFAULT '',
            created_by TEXT NOT NULL DEFAULT '',
            created_at BIGINT NOT NULL
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_maint_windows_time "
         "ON maintenance_windows(start_ms, end_ms);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS server_settings (
            key   TEXT PRIMARY KEY,
            value TEXT NOT NULL
        );
    )");
    exec(R"(
        CREATE TABLE IF NOT EXISTS silences (
            silence_id BIGSERIAL PRIMARY KEY,
            agent_id   TEXT NOT NULL DEFAULT '*',
            indicator  TEXT NOT NULL DEFAULT '*',
            reason     TEXT NOT NULL DEFAULT '',
            until_ms   BIGINT NOT NULL,
            created_by TEXT NOT NULL DEFAULT '',
            created_at BIGINT NOT NULL
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_silences_until ON silences(until_ms);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS log_matches (
            match_id       BIGSERIAL PRIMARY KEY,
            agent_id       TEXT NOT NULL REFERENCES agents(agent_id) ON DELETE CASCADE,
            indicator_name TEXT NOT NULL,
            path           TEXT NOT NULL DEFAULT '',
            matched_line   TEXT NOT NULL DEFAULT '',
            severity       TEXT NOT NULL DEFAULT 'red',
            created_at     BIGINT NOT NULL
        );
    )");
    exec("CREATE INDEX IF NOT EXISTS idx_log_matches_agent_ts "
         "ON log_matches(agent_id, created_at DESC);");
    exec(R"(
        CREATE TABLE IF NOT EXISTS views (
            view_id       BIGSERIAL PRIMARY KEY,
            name          TEXT NOT NULL,
            owner_user_id BIGINT NOT NULL DEFAULT 0,
            is_public     BOOLEAN NOT NULL DEFAULT FALSE,
            group_id      BIGINT NOT NULL DEFAULT 0,
            config_json   TEXT NOT NULL DEFAULT '{}',
            created_at    BIGINT NOT NULL
        );
    )");
    add_column_if_missing("views", "group_id", "ALTER TABLE views ADD COLUMN group_id BIGINT NOT NULL DEFAULT 0;");
    exec(R"(
        CREATE TABLE IF NOT EXISTS runbooks (
            runbook_id BIGSERIAL PRIMARY KEY,
            agent_id   TEXT NOT NULL DEFAULT '',
            indicator  TEXT NOT NULL DEFAULT '*',
            status     TEXT NOT NULL,
            url        TEXT NOT NULL,
            markdown   TEXT NOT NULL DEFAULT '',
            notes      TEXT NOT NULL DEFAULT '',
            created_by TEXT NOT NULL DEFAULT '',
            created_at BIGINT NOT NULL DEFAULT 0
        );
    )");
    add_column_if_missing("runbooks", "agent_id", "ALTER TABLE runbooks ADD COLUMN agent_id TEXT NOT NULL DEFAULT '';");
    add_column_if_missing("runbooks", "markdown", "ALTER TABLE runbooks ADD COLUMN markdown TEXT NOT NULL DEFAULT '';");
}

void PostgresStore::bootstrap_defaults()
{
    LOG_FUNCTION_TRACE
    // create_group is idempotent via INSERT … ON CONFLICT DO NOTHING
    create_group("Admins");
    const char* check_sql = "SELECT user_id FROM users WHERE username='thewatcher';";
    Res res(PQexec(conn_, check_sql));
    check_res(res.r, "bootstrap check admin");
    if (PQntuples(res.r) == 0)
    {
        const auto admin_id = create_user("thewatcher", hash_default_password(), "global_admin");
        // Find admins group id
        Res gres(PQexec(conn_, "SELECT group_id FROM groups WHERE name='Admins';"));
        check_res(gres.r, "find Admins group");
        if (PQntuples(gres.r) > 0)
        {
            const auto group_id = col_int64(gres.r, 0, 0);
            set_user_groups(admin_id, {group_id});
        }
        exec("UPDATE users SET built_in=TRUE WHERE username='thewatcher';");
        LOG_INFO("Bootstrapped default admin user thewatcher");
    }
    exec("UPDATE groups SET built_in=TRUE WHERE name='Admins';");
}

// ── Agents ────────────────────────────────────────────────────────────────────

void PostgresStore::upsert_agent(const AgentRecord& rec)
{
    const auto cfg_json = nlohmann::json(rec.collector_config).dump();
    // All s() conversions must outlive the params[] array; store them first.
    const auto p_approved = s(rec.approved);
    const auto p_rejected = s(rec.rejected);
    const auto p_connected = s(rec.connected);
    const auto p_maint = s(rec.maintenance);
    const auto p_munt = s(rec.maintenance_until);
    const auto p_ci = s(rec.collection_interval);
    const auto p_hi = s(rec.heartbeat_interval);
    const auto p_pl = s(rec.process_limit);
    const auto p_fs = s(rec.first_seen);
    const auto p_ls = s(rec.last_seen);
    const auto p_cw = s(rec.cpu_warning_pct_of_avg);
    const auto p_cd = s(rec.cpu_degraded_pct_of_avg);
    const auto p_cc = s(rec.cpu_critical_pct_of_avg);
    const auto p_mw = s(rec.memory_warning_pct_of_avg);
    const auto p_md = s(rec.memory_degraded_pct_of_avg);
    const auto p_mc = s(rec.memory_critical_pct_of_avg);
    const auto p_dw = s(rec.disk_warning_pct_of_avg);
    const auto p_dd = s(rec.disk_degraded_pct_of_avg);
    const auto p_dc = s(rec.disk_critical_pct_of_avg);
    const auto p_nw = s(rec.network_warning_pct_of_avg);
    const auto p_nd = s(rec.network_degraded_pct_of_avg);
    const auto p_nc = s(rec.network_critical_pct_of_avg);
    const char* params[] = {
        rec.agent_id.c_str(),
        rec.hostname.c_str(),
        rec.platform.c_str(),
        rec.curve_public_key_z85.c_str(),
        p_approved.c_str(),
        p_rejected.c_str(),
        p_connected.c_str(),
        p_maint.c_str(),
        rec.maintenance_reason.c_str(),
        p_munt.c_str(),
        p_ci.c_str(),
        p_pl.c_str(),
        p_fs.c_str(),
        p_ls.c_str(),
        p_cw.c_str(),
        p_cd.c_str(),
        p_cc.c_str(),
        p_mw.c_str(),
        p_md.c_str(),
        p_mc.c_str(),
        p_dw.c_str(),
        p_dd.c_str(),
        p_dc.c_str(),
        p_nw.c_str(),
        p_nd.c_str(),
        p_nc.c_str(),
        cfg_json.c_str(),
        rec.description.c_str(),
        p_hi.c_str(),
        rec.runbook_markdown.c_str(),
    };
    Res res(PQexecParams(conn_, R"(
        INSERT INTO agents(agent_id,hostname,platform,curve_public_key_z85,approved,rejected,connected,
            maintenance,maintenance_reason,maintenance_until,collection_interval,process_limit,
            first_seen,last_seen,cpu_warning_pct_of_avg,cpu_degraded_pct_of_avg,cpu_critical_pct_of_avg,
            memory_warning_pct_of_avg,memory_degraded_pct_of_avg,memory_critical_pct_of_avg,
            disk_warning_pct_of_avg,disk_degraded_pct_of_avg,disk_critical_pct_of_avg,
            network_warning_pct_of_avg,network_degraded_pct_of_avg,network_critical_pct_of_avg,
            collector_config_json,description,heartbeat_interval,runbook_markdown)
        VALUES($1,$2,$3,$4,$5::boolean,$6::boolean,$7::boolean,$8::boolean,$9,$10,$11,$12,$13,$14,
               $15,$16,$17,$18,$19,$20,$21,$22,$23,$24,$25,$26,$27,$28,$29,$30)
        ON CONFLICT(agent_id) DO UPDATE SET
            hostname=EXCLUDED.hostname,
            platform=EXCLUDED.platform,
            curve_public_key_z85=EXCLUDED.curve_public_key_z85,
            connected=EXCLUDED.connected,
            maintenance=EXCLUDED.maintenance,
            maintenance_reason=EXCLUDED.maintenance_reason,
            maintenance_until=EXCLUDED.maintenance_until,
            collection_interval=EXCLUDED.collection_interval,
            process_limit=EXCLUDED.process_limit,
            collector_config_json=EXCLUDED.collector_config_json,
            heartbeat_interval=EXCLUDED.heartbeat_interval,
            runbook_markdown=EXCLUDED.runbook_markdown,
            last_seen=EXCLUDED.last_seen;
    )",
                         30, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "upsert_agent");
}

namespace
{
    constexpr const char* AGENT_SELECT =
        "SELECT agent_id,hostname,platform,curve_public_key_z85,approved,rejected,connected,maintenance,"
        "maintenance_reason,maintenance_until,collection_interval,process_limit,first_seen,last_seen,"
        "cpu_warning_pct_of_avg,cpu_degraded_pct_of_avg,cpu_critical_pct_of_avg,"
        "memory_warning_pct_of_avg,memory_degraded_pct_of_avg,memory_critical_pct_of_avg,"
        "disk_warning_pct_of_avg,disk_degraded_pct_of_avg,disk_critical_pct_of_avg,"
        "network_warning_pct_of_avg,network_degraded_pct_of_avg,network_critical_pct_of_avg,"
        "collector_config_json,description,heartbeat_interval,runbook_markdown FROM agents";
} // namespace

std::optional<AgentRecord> PostgresStore::get_agent(const std::string& agent_id)
{
    const char* params[] = {agent_id.c_str()};
    Res res(PQexecParams(conn_, (std::string(AGENT_SELECT) + " WHERE agent_id=$1;").c_str(), 1, nullptr, params,
                         nullptr, nullptr, 0));
    check_res(res.r, "get_agent");
    if (PQntuples(res.r) == 0)
        return std::nullopt;
    return row_to_agent(res.r, 0);
}

std::vector<AgentRecord> PostgresStore::list_agents()
{
    Res res(PQexec(conn_, (std::string(AGENT_SELECT) + " ORDER BY last_seen DESC;").c_str()));
    check_res(res.r, "list_agents");
    std::vector<AgentRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(row_to_agent(res.r, i));
    return out;
}

std::vector<AgentRecord> PostgresStore::list_approved_agents()
{
    Res res(PQexec(
        conn_,
        (std::string(AGENT_SELECT) + " WHERE approved=TRUE AND rejected=FALSE ORDER BY last_seen DESC;").c_str()));
    check_res(res.r, "list_approved_agents");
    std::vector<AgentRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(row_to_agent(res.r, i));
    return out;
}

std::vector<AgentRecord> PostgresStore::list_pending_agents()
{
    Res res(PQexec(
        conn_,
        (std::string(AGENT_SELECT) + " WHERE approved=FALSE AND rejected=FALSE ORDER BY first_seen DESC;").c_str()));
    check_res(res.r, "list_pending_agents");
    std::vector<AgentRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(row_to_agent(res.r, i));
    return out;
}

void PostgresStore::approve_agent(const std::string& agent_id)
{
    const char* params[] = {agent_id.c_str()};
    Res res(PQexecParams(conn_, "UPDATE agents SET approved=TRUE,rejected=FALSE WHERE agent_id=$1;", 1, nullptr, params,
                         nullptr, nullptr, 0));
    check_res(res.r, "approve_agent");
}

void PostgresStore::reject_agent(const std::string& agent_id)
{
    const char* params[] = {agent_id.c_str()};
    Res res(PQexecParams(conn_, "UPDATE agents SET approved=FALSE,rejected=TRUE WHERE agent_id=$1;", 1, nullptr, params,
                         nullptr, nullptr, 0));
    check_res(res.r, "reject_agent");
}

void PostgresStore::delete_agent(const std::string& agent_id)
{
    const char* params[] = {agent_id.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM agents WHERE agent_id=$1;", 1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "delete_agent");
}

void PostgresStore::mark_agents_offline_before(int64_t cutoff_ms)
{
    const auto p_cutoff = s(cutoff_ms);
    const char* params[] = {p_cutoff.c_str()};
    Res res(PQexecParams(conn_,
                         "UPDATE agents SET connected=FALSE WHERE approved=TRUE AND connected=TRUE "
                         "AND last_seen>0 AND last_seen<$1 AND maintenance=FALSE;",
                         1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "mark_agents_offline_before");
}

void PostgresStore::set_agent_maintenance(const std::string& agent_id, bool maintenance, const std::string& reason,
                                          int64_t until_ms)
{
    const auto p_maint = s(maintenance);
    const auto p_until = s(until_ms);
    const char* params[] = {p_maint.c_str(), reason.c_str(), p_until.c_str(), agent_id.c_str()};
    Res res(PQexecParams(
        conn_,
        "UPDATE agents SET maintenance=$1::boolean,maintenance_reason=$2,maintenance_until=$3 WHERE agent_id=$4;", 4,
        nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "set_agent_maintenance");
}

void PostgresStore::set_agent_thresholds(const AgentRecord& rec)
{
    const auto p1 = s(rec.cpu_warning_pct_of_avg);
    const auto p2 = s(rec.cpu_degraded_pct_of_avg);
    const auto p3 = s(rec.cpu_critical_pct_of_avg);
    const auto p4 = s(rec.memory_warning_pct_of_avg);
    const auto p5 = s(rec.memory_degraded_pct_of_avg);
    const auto p6 = s(rec.memory_critical_pct_of_avg);
    const auto p7 = s(rec.disk_warning_pct_of_avg);
    const auto p8 = s(rec.disk_degraded_pct_of_avg);
    const auto p9 = s(rec.disk_critical_pct_of_avg);
    const auto p10 = s(rec.network_warning_pct_of_avg);
    const auto p11 = s(rec.network_degraded_pct_of_avg);
    const auto p12 = s(rec.network_critical_pct_of_avg);
    const char* params[] = {p1.c_str(),  p2.c_str(),  p3.c_str(),          p4.c_str(), p5.c_str(),
                            p6.c_str(),  p7.c_str(),  p8.c_str(),          p9.c_str(), p10.c_str(),
                            p11.c_str(), p12.c_str(), rec.agent_id.c_str()};
    Res res(PQexecParams(conn_, R"(
        UPDATE agents SET
            cpu_warning_pct_of_avg=$1,cpu_degraded_pct_of_avg=$2,cpu_critical_pct_of_avg=$3,
            memory_warning_pct_of_avg=$4,memory_degraded_pct_of_avg=$5,memory_critical_pct_of_avg=$6,
            disk_warning_pct_of_avg=$7,disk_degraded_pct_of_avg=$8,disk_critical_pct_of_avg=$9,
            network_warning_pct_of_avg=$10,network_degraded_pct_of_avg=$11,network_critical_pct_of_avg=$12
        WHERE agent_id=$13;
    )",
                         13, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "set_agent_thresholds");
}

void PostgresStore::set_agent_collector_config(const std::string& agent_id, const CollectorConfig& config)
{
    const auto cfg_json = nlohmann::json(config).dump();
    const char* params[] = {cfg_json.c_str(), agent_id.c_str()};
    Res res(PQexecParams(conn_, "UPDATE agents SET collector_config_json=$1 WHERE agent_id=$2;", 2, nullptr, params,
                         nullptr, nullptr, 0));
    check_res(res.r, "set_agent_collector_config");
}

void PostgresStore::set_agent_description(const std::string& agent_id, const std::string& description)
{
    const char* params[] = {description.c_str(), agent_id.c_str()};
    Res res(PQexecParams(conn_, "UPDATE agents SET description=$1 WHERE agent_id=$2;", 2, nullptr, params, nullptr,
                         nullptr, 0));
    check_res(res.r, "set_agent_description");
}

// ── Groups ────────────────────────────────────────────────────────────────────

void PostgresStore::set_agent_runbook(const std::string& agent_id, const std::string& markdown)
{
    const char* params[] = {markdown.c_str(), agent_id.c_str()};
    Res res(PQexecParams(conn_, "UPDATE agents SET runbook_markdown=$1 WHERE agent_id=$2;", 2, nullptr, params, nullptr,
                         nullptr, 0));
    check_res(res.r, "set_agent_runbook");
}

std::vector<GroupRecord> PostgresStore::list_groups()
{
    Res res(PQexec(conn_, "SELECT group_id,name,built_in FROM groups ORDER BY group_id;"));
    check_res(res.r, "list_groups");
    std::vector<GroupRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back({col_int64(res.r, i, 0), col(res.r, i, 1), col_bool(res.r, i, 2)});
    return out;
}

int64_t PostgresStore::create_group(const std::string& name)
{
    const char* params[] = {name.c_str()};
    Res res(PQexecParams(
        conn_,
        "INSERT INTO groups(name) VALUES($1) ON CONFLICT(name) DO UPDATE SET name=EXCLUDED.name RETURNING group_id;", 1,
        nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "create_group");
    return col_int64(res.r, 0, 0);
}

std::vector<int64_t> PostgresStore::get_agent_groups(const std::string& agent_id)
{
    const char* params[] = {agent_id.c_str()};
    Res res(PQexecParams(conn_, "SELECT group_id FROM agent_groups WHERE agent_id=$1 ORDER BY group_id;", 1, nullptr,
                         params, nullptr, nullptr, 0));
    check_res(res.r, "get_agent_groups");
    std::vector<int64_t> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(col_int64(res.r, i, 0));
    return out;
}

void PostgresStore::set_agent_groups(const std::string& agent_id, const std::vector<int64_t>& group_ids)
{
    const char* del_params[] = {agent_id.c_str()};
    Res del(PQexecParams(conn_, "DELETE FROM agent_groups WHERE agent_id=$1;", 1, nullptr, del_params, nullptr, nullptr,
                         0));
    check_res(del.r, "set_agent_groups delete");
    for (auto gid : group_ids)
    {
        const auto p_gid = s(gid);
        const char* params[] = {agent_id.c_str(), p_gid.c_str()};
        Res ins(PQexecParams(conn_, "INSERT INTO agent_groups(agent_id,group_id) VALUES($1,$2) ON CONFLICT DO NOTHING;",
                             2, nullptr, params, nullptr, nullptr, 0));
        check_res(ins.r, "set_agent_groups insert");
    }
}

// ── Users ─────────────────────────────────────────────────────────────────────

std::optional<UserRecord> PostgresStore::get_user_by_username(const std::string& username)
{
    const char* params[] = {username.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT user_id,username,password_hash,role,built_in,disabled FROM users WHERE username=$1;",
                         1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "get_user_by_username");
    if (PQntuples(res.r) == 0)
        return std::nullopt;
    return UserRecord{col_int64(res.r, 0, 0), col(res.r, 0, 1),      col(res.r, 0, 2),
                      col(res.r, 0, 3),       col_bool(res.r, 0, 4), col_bool(res.r, 0, 5)};
}

std::vector<UserRecord> PostgresStore::list_users()
{
    Res res(PQexec(conn_, "SELECT user_id,username,password_hash,role,built_in,disabled FROM users ORDER BY user_id;"));
    check_res(res.r, "list_users");
    std::vector<UserRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back({col_int64(res.r, i, 0), col(res.r, i, 1), col(res.r, i, 2), col(res.r, i, 3),
                       col_bool(res.r, i, 4), col_bool(res.r, i, 5)});
    return out;
}

int64_t PostgresStore::create_user(const std::string& username, const std::string& password_hash,
                                   const std::string& role)
{
    const char* params[] = {username.c_str(), password_hash.c_str(), role.c_str()};
    Res res(PQexecParams(conn_, "INSERT INTO users(username,password_hash,role) VALUES($1,$2,$3) RETURNING user_id;", 3,
                         nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "create_user");
    return col_int64(res.r, 0, 0);
}

void PostgresStore::set_user_groups(int64_t user_id, const std::vector<int64_t>& group_ids)
{
    const auto p_uid = s(user_id);
    const char* del_params[] = {p_uid.c_str()};
    Res del(
        PQexecParams(conn_, "DELETE FROM user_groups WHERE user_id=$1;", 1, nullptr, del_params, nullptr, nullptr, 0));
    check_res(del.r, "set_user_groups delete");
    for (auto gid : group_ids)
    {
        const auto p_gid = s(gid);
        const char* params[] = {p_uid.c_str(), p_gid.c_str()};
        Res ins(PQexecParams(conn_, "INSERT INTO user_groups(user_id,group_id) VALUES($1,$2) ON CONFLICT DO NOTHING;",
                             2, nullptr, params, nullptr, nullptr, 0));
        check_res(ins.r, "set_user_groups insert");
    }
}

std::vector<int64_t> PostgresStore::get_user_groups(int64_t user_id)
{
    const auto p_uid = s(user_id);
    const char* params[] = {p_uid.c_str()};
    Res res(PQexecParams(conn_, "SELECT group_id FROM user_groups WHERE user_id=$1 ORDER BY group_id;", 1, nullptr,
                         params, nullptr, nullptr, 0));
    check_res(res.r, "get_user_groups");
    std::vector<int64_t> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(col_int64(res.r, i, 0));
    return out;
}

// Disables a non-built-in user. Returns the affected row count, so 0 means an
// unknown id or a built-in user. Throws on SQL error.
int PostgresStore::disable_user(int64_t user_id)
{
    const auto p = s(user_id);
    const char* params[] = {p.c_str()};
    // SEC-14: the built-in admin must not be disableable — deleting it would
    // let the next bootstrap recreate it with the public default password.
    Res res(PQexecParams(conn_, "UPDATE users SET disabled=TRUE WHERE user_id=$1 AND built_in=FALSE;", 1, nullptr,
                         params, nullptr, nullptr, 0));
    check_res(res.r, "disable_user");
    const char* affected = PQcmdTuples(res.r);
    return affected != nullptr ? std::atoi(affected) : 0;
}

void PostgresStore::enable_user(int64_t user_id)
{
    const auto p = s(user_id);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, "UPDATE users SET disabled=FALSE WHERE user_id=$1;", 1, nullptr, params, nullptr,
                         nullptr, 0));
    check_res(res.r, "enable_user");
}

// Deletes a non-built-in user. Returns the affected row count, so 0 means an
// unknown id or a built-in user. Throws on SQL error.
int PostgresStore::delete_user(int64_t user_id)
{
    const auto p = s(user_id);
    const char* params[] = {p.c_str()};
    // SEC-14: same built_in guard as SQLite — never delete the bootstrap admin.
    Res res(PQexecParams(conn_, "DELETE FROM users WHERE user_id=$1 AND built_in=FALSE;", 1, nullptr, params, nullptr,
                         nullptr, 0));
    check_res(res.r, "delete_user");
    const char* affected = PQcmdTuples(res.r);
    return affected != nullptr ? std::atoi(affected) : 0;
}

void PostgresStore::update_user_password(int64_t user_id, const std::string& password_hash)
{
    const auto p_uid = s(user_id);
    const char* params[] = {password_hash.c_str(), p_uid.c_str()};
    Res res(PQexecParams(conn_, "UPDATE users SET password_hash=$1 WHERE user_id=$2;", 2, nullptr, params, nullptr,
                         nullptr, 0));
    check_res(res.r, "update_user_password");
}

// ── Sessions ──────────────────────────────────────────────────────────────────

void PostgresStore::create_session(const SessionRecord& session)
{
    const auto p_uid = s(session.user_id);
    const auto p_cat = s(session.created_at);
    const auto p_eat = s(session.expires_at);
    const char* params[] = {session.token.c_str(), p_uid.c_str(), p_cat.c_str(), p_eat.c_str()};
    Res res(PQexecParams(
        conn_, "INSERT INTO sessions(token,user_id,created_at,expires_at) VALUES($1,$2,$3,$4) ON CONFLICT DO NOTHING;",
        4, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "create_session");
}

// Returns the session only while it is unexpired and its user is enabled;
// otherwise nullopt. Throws on SQL error.
std::optional<SessionRecord> PostgresStore::get_session(const std::string& token, int64_t now_ms)
{
    const auto p_now = s(now_ms);
    const char* params[] = {token.c_str(), p_now.c_str()};
    Res res(PQexecParams(conn_, R"(
        SELECT s.token,s.user_id,u.username,u.role,s.created_at,s.expires_at
        FROM sessions s JOIN users u ON u.user_id=s.user_id
        WHERE s.token=$1 AND s.expires_at>$2 AND u.disabled=FALSE;
    )",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "get_session");
    if (PQntuples(res.r) == 0)
        return std::nullopt;
    return SessionRecord{col(res.r, 0, 0), col_int64(res.r, 0, 1), col(res.r, 0, 2),
                         col(res.r, 0, 3), col_int64(res.r, 0, 4), col_int64(res.r, 0, 5)};
}

void PostgresStore::delete_session(const std::string& token)
{
    const char* params[] = {token.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM sessions WHERE token=$1;", 1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "delete_session");
}

// ── Metrics ───────────────────────────────────────────────────────────────────

void PostgresStore::insert_metrics(const MetricsRow& row)
{
    const auto hex = to_bytea_hex(row.metrics_cbor);
    const auto p_ts = s(row.timestamp_ms);
    const char* params[] = {row.agent_id.c_str(), p_ts.c_str(), hex.c_str()};
    Res res(PQexecParams(conn_, "INSERT INTO metrics(agent_id,timestamp_ms,metrics_cbor) VALUES($1,$2,$3);", 3, nullptr,
                         params, nullptr, nullptr, 0));
    check_res(res.r, "insert_metrics");
}

std::vector<MetricsRow> PostgresStore::get_metrics(const std::string& agent_id, int limit)
{
    const auto p_lim = s(limit);
    const char* params[] = {agent_id.c_str(), p_lim.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT agent_id,timestamp_ms,metrics_cbor FROM metrics WHERE agent_id=$1 "
                         "ORDER BY timestamp_ms DESC LIMIT $2;",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "get_metrics");
    std::vector<MetricsRow> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
    {
        MetricsRow r;
        r.agent_id = col(res.r, i, 0);
        r.timestamp_ms = col_int64(res.r, i, 1);
        r.metrics_cbor = col_bytea(res.r, i, 2);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<MetricsRow> PostgresStore::get_metrics_in_window(const std::string& agent_id, int64_t since_ms,
                                                             int64_t until_ms)
{
    const auto p_since = s(since_ms);
    const auto p_until = s(until_ms);
    const char* params[] = {agent_id.c_str(), p_since.c_str(), p_until.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT agent_id,timestamp_ms,metrics_cbor FROM metrics "
                         "WHERE agent_id=$1 AND timestamp_ms>=$2 AND timestamp_ms<=$3 ORDER BY timestamp_ms ASC;",
                         3, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "get_metrics_in_window");
    std::vector<MetricsRow> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
    {
        MetricsRow r;
        r.agent_id = col(res.r, i, 0);
        r.timestamp_ms = col_int64(res.r, i, 1);
        r.metrics_cbor = col_bytea(res.r, i, 2);
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<MetricsRow> PostgresStore::latest_metrics()
{
    Res res(PQexec(conn_, R"(
        SELECT DISTINCT ON (m.agent_id) m.agent_id, m.timestamp_ms, m.metrics_cbor
        FROM metrics m
        ORDER BY m.agent_id, m.timestamp_ms DESC;
    )"));
    check_res(res.r, "latest_metrics");
    std::vector<MetricsRow> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
    {
        MetricsRow r;
        r.agent_id = col(res.r, i, 0);
        r.timestamp_ms = col_int64(res.r, i, 1);
        r.metrics_cbor = col_bytea(res.r, i, 2);
        out.push_back(std::move(r));
    }
    return out;
}

void PostgresStore::prune_metrics_before(int64_t cutoff_ms)
{
    const auto p = s(cutoff_ms);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM metrics WHERE timestamp_ms<$1;", 1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "prune_metrics_before");
}

int64_t PostgresStore::count_metrics_in_window(const std::string& agent_id, int64_t since_ms, int64_t until_ms)
{
    const auto p_since = s(since_ms);
    const auto p_until = s(until_ms);
    const char* params[] = {agent_id.c_str(), p_since.c_str(), p_until.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT COUNT(*) FROM metrics WHERE agent_id=$1 AND timestamp_ms>=$2 AND timestamp_ms<=$3;", 3,
                         nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "count_metrics_in_window");
    return col_int64(res.r, 0, 0);
}

// ── Status history ────────────────────────────────────────────────────────────

void PostgresStore::insert_status_history(const StatusHistoryRow& row)
{
    const auto p_cat = s(row.created_at);
    const char* params[] = {row.agent_id.c_str(),   row.indicator.c_str(), row.old_status.c_str(),
                            row.new_status.c_str(), row.message.c_str(),   p_cat.c_str()};
    Res res(PQexecParams(conn_,
                         "INSERT INTO status_history(agent_id,indicator,old_status,new_status,message,created_at) "
                         "VALUES($1,$2,$3,$4,$5,$6);",
                         6, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "insert_status_history");
}

std::optional<StatusHistoryRow> PostgresStore::latest_status_for_indicator(const std::string& agent_id,
                                                                           const std::string& indicator)
{
    const char* params[] = {agent_id.c_str(), indicator.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT id,agent_id,indicator,old_status,new_status,message,created_at "
                         "FROM status_history WHERE agent_id=$1 AND indicator=$2 ORDER BY created_at DESC LIMIT 1;",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "latest_status_for_indicator");
    if (PQntuples(res.r) == 0)
        return std::nullopt;
    return StatusHistoryRow{col_int64(res.r, 0, 0), col(res.r, 0, 1), col(res.r, 0, 2),      col(res.r, 0, 3),
                            col(res.r, 0, 4),       col(res.r, 0, 5), col_int64(res.r, 0, 6)};
}

std::vector<StatusHistoryRow> PostgresStore::list_status_history(const std::string& agent_id, int limit)
{
    const auto p_lim = s(limit);
    const char* params[] = {agent_id.c_str(), p_lim.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT id,agent_id,indicator,old_status,new_status,message,created_at "
                         "FROM status_history WHERE agent_id=$1 ORDER BY created_at DESC LIMIT $2;",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "list_status_history");
    std::vector<StatusHistoryRow> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back({col_int64(res.r, i, 0), col(res.r, i, 1), col(res.r, i, 2), col(res.r, i, 3), col(res.r, i, 4),
                       col(res.r, i, 5), col_int64(res.r, i, 6)});
    return out;
}

// ── Pending status ────────────────────────────────────────────────────────────

std::optional<PendingStatusRecord> PostgresStore::get_pending_status(const std::string& agent_id,
                                                                     const std::string& indicator)
{
    const char* params[] = {agent_id.c_str(), indicator.c_str()};
    Res res(PQexecParams(
        conn_, "SELECT agent_id,indicator,target_status,count FROM pending_status WHERE agent_id=$1 AND indicator=$2;",
        2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "get_pending_status");
    if (PQntuples(res.r) == 0)
        return std::nullopt;
    return PendingStatusRecord{col(res.r, 0, 0), col(res.r, 0, 1), col(res.r, 0, 2), col_int(res.r, 0, 3)};
}

void PostgresStore::set_pending_status(const std::string& agent_id, const std::string& indicator,
                                       const std::string& target_status, int count)
{
    const auto p_count = s(count);
    const char* params[] = {agent_id.c_str(), indicator.c_str(), target_status.c_str(), p_count.c_str()};
    Res res(PQexecParams(conn_, R"(
        INSERT INTO pending_status(agent_id,indicator,target_status,count)
        VALUES($1,$2,$3,$4)
        ON CONFLICT(agent_id,indicator) DO UPDATE SET target_status=$3,count=$4;
    )",
                         4, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "set_pending_status");
}

void PostgresStore::clear_pending_status(const std::string& agent_id, const std::string& indicator)
{
    const char* params[] = {agent_id.c_str(), indicator.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM pending_status WHERE agent_id=$1 AND indicator=$2;", 2, nullptr, params,
                         nullptr, nullptr, 0));
    check_res(res.r, "clear_pending_status");
}

// ── Alerts ────────────────────────────────────────────────────────────────────

int64_t PostgresStore::insert_alert(const AlertRecord& alert)
{
    const auto p_cat = s(alert.created_at);
    const auto p_aat = s(alert.acknowledged_at);
    const auto p_dat = s(alert.deleted_at);
    const auto p_eat = s(alert.escalated_at);
    const char* params[] = {alert.agent_id.c_str(),
                            alert.indicator.c_str(),
                            alert.old_status.c_str(),
                            alert.new_status.c_str(),
                            alert.message.c_str(),
                            p_cat.c_str(),
                            alert.acknowledged_by.c_str(),
                            p_aat.c_str(),
                            p_dat.c_str(),
                            alert.note.c_str(),
                            p_eat.c_str(),
                            alert.runbook_url.c_str()};
    Res res(PQexecParams(conn_, R"(
        INSERT INTO alerts(agent_id,indicator,old_status,new_status,message,created_at,
            acknowledged_by,acknowledged_at,deleted_at,note,escalated_at,runbook_url)
        VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)
        RETURNING alert_id;
    )",
                         12, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "insert_alert");
    return col_int64(res.r, 0, 0);
}

namespace
{
    constexpr const char* ALERT_SELECT =
        "SELECT alert_id,agent_id,indicator,old_status,new_status,message,created_at,"
        "acknowledged_by,acknowledged_at,deleted_at,note,escalated_at,runbook_url FROM alerts";
} // namespace

std::vector<AlertRecord> PostgresStore::list_alerts(bool include_deleted)
{
    const char* sql = include_deleted ? "SELECT alert_id,agent_id,indicator,old_status,new_status,message,created_at,"
                                        "acknowledged_by,acknowledged_at,deleted_at,note,escalated_at,runbook_url "
                                        "FROM alerts ORDER BY created_at DESC,alert_id DESC;"
                                      : "SELECT alert_id,agent_id,indicator,old_status,new_status,message,created_at,"
                                        "acknowledged_by,acknowledged_at,deleted_at,note,escalated_at,runbook_url "
                                        "FROM alerts WHERE deleted_at=0 ORDER BY created_at DESC,alert_id DESC;";
    Res res(PQexec(conn_, sql));
    check_res(res.r, "list_alerts");
    std::vector<AlertRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(row_to_alert(res.r, i));
    return out;
}

std::vector<AlertRecord> PostgresStore::list_unacknowledged_alerts()
{
    Res res(PQexec(conn_,
                   "SELECT alert_id,agent_id,indicator,old_status,new_status,message,created_at,"
                   "acknowledged_by,acknowledged_at,deleted_at,note,escalated_at,runbook_url "
                   "FROM alerts WHERE deleted_at=0 AND acknowledged_at=0 ORDER BY created_at DESC,alert_id DESC;"));
    check_res(res.r, "list_unacknowledged_alerts");
    std::vector<AlertRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(row_to_alert(res.r, i));
    return out;
}

void PostgresStore::acknowledge_alert(int64_t alert_id, const std::string& username, int64_t acknowledged_at,
                                      const std::string& note)
{
    const auto p_id = s(alert_id);
    const auto p_aat = s(acknowledged_at);
    const char* params[] = {username.c_str(), p_aat.c_str(), note.c_str(), p_id.c_str()};
    Res res(PQexecParams(
        conn_, "UPDATE alerts SET acknowledged_by=$1,acknowledged_at=$2,note=$3 WHERE alert_id=$4 AND deleted_at=0;", 4,
        nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "acknowledge_alert");
}

void PostgresStore::bulk_acknowledge_alerts(const std::vector<int64_t>& alert_ids, const std::string& username,
                                            int64_t acknowledged_at, const std::string& note)
{
    for (const auto id : alert_ids)
        acknowledge_alert(id, username, acknowledged_at, note);
}

void PostgresStore::soft_delete_alert(int64_t alert_id, int64_t deleted_at)
{
    const auto p_id = s(alert_id);
    const auto p_dat = s(deleted_at);
    const char* params[] = {p_dat.c_str(), p_id.c_str()};
    Res res(PQexecParams(conn_, "UPDATE alerts SET deleted_at=$1 WHERE alert_id=$2;", 2, nullptr, params, nullptr,
                         nullptr, 0));
    check_res(res.r, "soft_delete_alert");
}

void PostgresStore::bulk_soft_delete_alerts(const std::vector<int64_t>& alert_ids, int64_t deleted_at)
{
    for (const auto id : alert_ids)
        soft_delete_alert(id, deleted_at);
}

void PostgresStore::clear_active_alerts_for_agent(const std::string& agent_id, int64_t cleared_at)
{
    const auto p_cat = s(cleared_at);
    const char* params[] = {p_cat.c_str(), agent_id.c_str()};
    Res res(PQexecParams(conn_,
                         "UPDATE alerts SET acknowledged_by='maintenance',acknowledged_at=$1 "
                         "WHERE agent_id=$2 AND acknowledged_at=0 AND deleted_at=0;",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "clear_active_alerts_for_agent");
}

std::vector<std::string> PostgresStore::get_offline_unalerted_agent_ids()
{
    Res res(PQexec(conn_, R"(
        SELECT a.agent_id FROM agents a WHERE a.approved=TRUE AND a.connected=FALSE
        AND a.maintenance=FALSE AND a.last_seen>0
        AND NOT EXISTS (
            SELECT 1 FROM alerts al WHERE al.agent_id=a.agent_id
            AND al.indicator='Heartbeat' AND al.deleted_at=0
        );
    )"));
    check_res(res.r, "get_offline_unalerted_agent_ids");
    std::vector<std::string> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(col(res.r, i, 0));
    return out;
}

void PostgresStore::archive_heartbeat_alerts_for_agent(const std::string& agent_id, int64_t deleted_at)
{
    const auto p_dat = s(deleted_at);
    const char* params[] = {p_dat.c_str(), agent_id.c_str()};
    Res res(PQexecParams(
        conn_, "UPDATE alerts SET deleted_at=$1 WHERE agent_id=$2 AND indicator='Heartbeat' AND deleted_at=0;", 2,
        nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "archive_heartbeat_alerts_for_agent");
}

void PostgresStore::escalate_old_alerts(int64_t cutoff_ms, int64_t now_ms)
{
    const auto p_now = s(now_ms);
    const auto p_cutoff = s(cutoff_ms);
    const char* params[] = {p_now.c_str(), p_cutoff.c_str()};
    Res res(PQexecParams(conn_,
                         "UPDATE alerts SET escalated_at=$1 WHERE deleted_at=0 AND acknowledged_at=0 "
                         "AND escalated_at=0 AND created_at<$2;",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "escalate_old_alerts");
}

// ── Maintenance windows ───────────────────────────────────────────────────────

int64_t PostgresStore::create_maintenance_window(const MaintenanceWindowRecord& rec)
{
    const auto p_start = s(rec.start_ms);
    const auto p_end = s(rec.end_ms);
    const auto p_cat = s(rec.created_at);
    const char* params[] = {rec.agent_id.c_str(), p_start.c_str(),        p_end.c_str(),
                            rec.reason.c_str(),   rec.created_by.c_str(), p_cat.c_str()};
    Res res(PQexecParams(conn_,
                         "INSERT INTO maintenance_windows(agent_id,start_ms,end_ms,reason,created_by,created_at) "
                         "VALUES($1,$2,$3,$4,$5,$6) RETURNING window_id;",
                         6, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "create_maintenance_window");
    return col_int64(res.r, 0, 0);
}

std::vector<MaintenanceWindowRecord> PostgresStore::list_maintenance_windows()
{
    Res res(PQexec(conn_, "SELECT window_id,agent_id,start_ms,end_ms,reason,created_by,created_at "
                          "FROM maintenance_windows ORDER BY start_ms DESC;"));
    check_res(res.r, "list_maintenance_windows");
    std::vector<MaintenanceWindowRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back({col_int64(res.r, i, 0), col(res.r, i, 1), col_int64(res.r, i, 2), col_int64(res.r, i, 3),
                       col(res.r, i, 4), col(res.r, i, 5), col_int64(res.r, i, 6)});
    return out;
}

void PostgresStore::delete_maintenance_window(int64_t window_id)
{
    const auto p = s(window_id);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM maintenance_windows WHERE window_id=$1;", 1, nullptr, params, nullptr,
                         nullptr, 0));
    check_res(res.r, "delete_maintenance_window");
}

std::vector<MaintenanceWindowRecord> PostgresStore::active_maintenance_windows(int64_t now_ms)
{
    const auto p = s(now_ms);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT window_id,agent_id,start_ms,end_ms,reason,created_by,created_at "
                         "FROM maintenance_windows WHERE start_ms<=$1 AND end_ms>$1;",
                         1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "active_maintenance_windows");
    std::vector<MaintenanceWindowRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back({col_int64(res.r, i, 0), col(res.r, i, 1), col_int64(res.r, i, 2), col_int64(res.r, i, 3),
                       col(res.r, i, 4), col(res.r, i, 5), col_int64(res.r, i, 6)});
    return out;
}

// ── Settings ──────────────────────────────────────────────────────────────────

std::string PostgresStore::get_setting(const std::string& key, const std::string& fallback)
{
    const char* params[] = {key.c_str()};
    Res res(PQexecParams(conn_, "SELECT value FROM server_settings WHERE key=$1;", 1, nullptr, params, nullptr, nullptr,
                         0));
    check_res(res.r, "get_setting");
    if (PQntuples(res.r) == 0)
        return fallback;
    return col(res.r, 0, 0);
}

void PostgresStore::set_setting(const std::string& key, const std::string& value)
{
    const char* params[] = {key.c_str(), value.c_str()};
    Res res(PQexecParams(conn_,
                         "INSERT INTO server_settings(key,value) VALUES($1,$2) "
                         "ON CONFLICT(key) DO UPDATE SET value=$2;",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "set_setting");
}

// ── Silences ──────────────────────────────────────────────────────────────────

int64_t PostgresStore::create_silence(const SilenceRecord& rec)
{
    const auto p_until = s(rec.until_ms);
    const auto p_cat = s(rec.created_at);
    const char* params[] = {rec.agent_id.c_str(), rec.indicator.c_str(),  rec.reason.c_str(),
                            p_until.c_str(),      rec.created_by.c_str(), p_cat.c_str()};
    Res res(PQexecParams(conn_,
                         "INSERT INTO silences(agent_id,indicator,reason,until_ms,created_by,created_at) "
                         "VALUES($1,$2,$3,$4,$5,$6) RETURNING silence_id;",
                         6, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "create_silence");
    return col_int64(res.r, 0, 0);
}

std::vector<SilenceRecord> PostgresStore::list_silences()
{
    Res res(PQexec(conn_, "SELECT silence_id,agent_id,indicator,reason,until_ms,created_by,created_at "
                          "FROM silences ORDER BY created_at DESC;"));
    check_res(res.r, "list_silences");
    std::vector<SilenceRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back({col_int64(res.r, i, 0), col(res.r, i, 1), col(res.r, i, 2), col(res.r, i, 3),
                       col_int64(res.r, i, 4), col(res.r, i, 5), col_int64(res.r, i, 6)});
    return out;
}

void PostgresStore::delete_silence(int64_t silence_id)
{
    const auto p = s(silence_id);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM silences WHERE silence_id=$1;", 1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "delete_silence");
}

bool PostgresStore::is_silenced(const std::string& agent_id, const std::string& indicator, int64_t now_ms)
{
    const auto p_now = s(now_ms);
    const char* params[] = {agent_id.c_str(), indicator.c_str(), p_now.c_str()};
    Res res(PQexecParams(conn_, R"(
        SELECT 1 FROM silences
        WHERE (agent_id=$1 OR agent_id='*')
          AND (indicator=$2 OR indicator='*')
          AND until_ms>$3
        LIMIT 1;
    )",
                         3, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "is_silenced");
    return PQntuples(res.r) > 0;
}

// ── Log matches ───────────────────────────────────────────────────────────────

void PostgresStore::insert_log_match(const LogMatchRecord& rec)
{
    const auto p_cat = s(rec.created_at);
    const char* params[] = {rec.agent_id.c_str(),     rec.indicator_name.c_str(), rec.path.c_str(),
                            rec.matched_line.c_str(), rec.severity.c_str(),       p_cat.c_str()};
    Res res(PQexecParams(conn_,
                         "INSERT INTO log_matches(agent_id,indicator_name,path,matched_line,severity,created_at) "
                         "VALUES($1,$2,$3,$4,$5,$6);",
                         6, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "insert_log_match");
}

std::vector<LogMatchRecord> PostgresStore::list_log_matches(const std::string& agent_id, int limit)
{
    const auto p_lim = s(limit);
    const char* params[] = {agent_id.c_str(), p_lim.c_str()};
    Res res(PQexecParams(conn_,
                         "SELECT match_id,agent_id,indicator_name,path,matched_line,severity,created_at "
                         "FROM log_matches WHERE agent_id=$1 ORDER BY created_at DESC LIMIT $2;",
                         2, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "list_log_matches");
    std::vector<LogMatchRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
    {
        LogMatchRecord r;
        r.match_id = col_int64(res.r, i, 0);
        r.agent_id = col(res.r, i, 1);
        r.indicator_name = col(res.r, i, 2);
        r.path = col(res.r, i, 3);
        r.matched_line = col(res.r, i, 4);
        r.severity = col(res.r, i, 5);
        r.created_at = col_int64(res.r, i, 6);
        out.push_back(std::move(r));
    }
    return out;
}

// ── Views ─────────────────────────────────────────────────────────────────────

namespace
{
    ViewRecord pg_row_to_view(PGresult* r, int row)
    {
        ViewRecord v;
        v.view_id = col_int64(r, row, 0);
        v.name = col(r, row, 1);
        v.owner_user_id = col_int64(r, row, 2);
        v.owner_username = col(r, row, 3);
        v.is_public = col_bool(r, row, 4);
        v.group_id = col_int64(r, row, 5);
        v.created_at = col_int64(r, row, 6);
        try
        {
            const auto cfg = nlohmann::json::parse(col(r, row, 7));
            if (cfg.contains("agent_ids") && cfg["agent_ids"].is_array())
                v.agent_ids = cfg["agent_ids"].get<std::vector<std::string>>();
            if (v.group_id == 0 && cfg.contains("group_id"))
                v.group_id = cfg["group_id"].get<int64_t>();
        }
        catch (...)
        {
        }
        return v;
    }
} // namespace

int64_t PostgresStore::create_view(const ViewRecord& rec)
{
    nlohmann::json cfg;
    cfg["agent_ids"] = rec.agent_ids;
    cfg["group_id"] = rec.group_id;
    const auto cfg_json = cfg.dump();
    const auto p_uid = s(rec.owner_user_id);
    const auto p_pub = s(rec.is_public);
    const auto p_group = s(rec.group_id);
    const auto p_cat = s(rec.created_at);
    const char* params[] = {rec.name.c_str(), p_uid.c_str(),    p_pub.c_str(),
                            p_group.c_str(),  cfg_json.c_str(), p_cat.c_str()};
    Res res(PQexecParams(conn_,
                         "INSERT INTO views(name,owner_user_id,is_public,group_id,config_json,created_at) "
                         "VALUES($1,$2,$3::boolean,$4,$5,$6) RETURNING view_id;",
                         6, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "create_view");
    return col_int64(res.r, 0, 0);
}

std::optional<ViewRecord> PostgresStore::get_view(int64_t view_id)
{
    const auto p = s(view_id);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, R"(
        SELECT v.view_id,v.name,v.owner_user_id,COALESCE(u.username,''),v.is_public,v.group_id,v.created_at,v.config_json
        FROM views v LEFT JOIN users u ON u.user_id=v.owner_user_id
        WHERE v.view_id=$1;
    )",
                         1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "get_view");
    if (PQntuples(res.r) == 0)
        return std::nullopt;
    return pg_row_to_view(res.r, 0);
}

std::vector<ViewRecord> PostgresStore::list_views(int64_t user_id)
{
    const auto p = s(user_id);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, R"(
        SELECT v.view_id,v.name,v.owner_user_id,COALESCE(u.username,''),v.is_public,v.group_id,v.created_at,v.config_json
        FROM views v LEFT JOIN users u ON u.user_id=v.owner_user_id
        WHERE $1::bigint <= 0 OR v.owner_user_id=$1 OR v.is_public=TRUE
           OR EXISTS (SELECT 1 FROM user_groups ug WHERE ug.user_id=$1 AND ug.group_id=v.group_id)
        ORDER BY v.created_at ASC;
    )",
                         1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "list_views");
    std::vector<ViewRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
        out.push_back(pg_row_to_view(res.r, i));
    return out;
}

void PostgresStore::update_view(const ViewRecord& rec)
{
    nlohmann::json cfg;
    cfg["agent_ids"] = rec.agent_ids;
    cfg["group_id"] = rec.group_id;
    const auto cfg_json = cfg.dump();
    const auto p_pub = s(rec.is_public);
    const auto p_group = s(rec.group_id);
    const auto p_id = s(rec.view_id);
    const char* params[] = {rec.name.c_str(), p_pub.c_str(), p_group.c_str(), cfg_json.c_str(), p_id.c_str()};
    Res res(PQexecParams(conn_,
                         "UPDATE views SET name=$1,is_public=$2::boolean,group_id=$3,config_json=$4 WHERE view_id=$5;",
                         5, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "update_view");
}

void PostgresStore::delete_view(int64_t view_id)
{
    const auto p = s(view_id);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM views WHERE view_id=$1;", 1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "delete_view");
}

// ── Runbooks ──────────────────────────────────────────────────────────────────

int64_t PostgresStore::create_runbook(const RunbookRecord& rec)
{
    const auto p_cat = s(rec.created_at);
    const char* params[] = {rec.agent_id.c_str(), rec.indicator.c_str(), rec.status.c_str(),     rec.url.c_str(),
                            rec.markdown.c_str(), rec.notes.c_str(),     rec.created_by.c_str(), p_cat.c_str()};
    Res res(PQexecParams(conn_,
                         "INSERT INTO runbooks(agent_id,indicator,status,url,markdown,notes,created_by,created_at) "
                         "VALUES($1,$2,$3,$4,$5,$6,$7,$8) RETURNING runbook_id;",
                         8, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "create_runbook");
    return col_int64(res.r, 0, 0);
}

std::vector<RunbookRecord> PostgresStore::list_runbooks()
{
    Res res(PQexec(conn_, "SELECT runbook_id,agent_id,indicator,status,url,markdown,notes,created_by,created_at "
                          "FROM runbooks ORDER BY created_at ASC;"));
    check_res(res.r, "list_runbooks");
    std::vector<RunbookRecord> out;
    for (int i = 0; i < PQntuples(res.r); ++i)
    {
        RunbookRecord r;
        r.runbook_id = col_int64(res.r, i, 0);
        r.agent_id = col(res.r, i, 1);
        r.indicator = col(res.r, i, 2);
        r.status = col(res.r, i, 3);
        r.url = col(res.r, i, 4);
        r.markdown = col(res.r, i, 5);
        r.notes = col(res.r, i, 6);
        r.created_by = col(res.r, i, 7);
        r.created_at = col_int64(res.r, i, 8);
        out.push_back(std::move(r));
    }
    return out;
}

void PostgresStore::delete_runbook(int64_t runbook_id)
{
    const auto p = s(runbook_id);
    const char* params[] = {p.c_str()};
    Res res(PQexecParams(conn_, "DELETE FROM runbooks WHERE runbook_id=$1;", 1, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "delete_runbook");
}

std::optional<RunbookRecord> PostgresStore::get_runbook(const std::string& agent_id, const std::string& indicator,
                                                       const std::string& status)
{
    // SEC-11: same selection rules as SqliteStore::get_runbook.
    const char* params[] = {agent_id.c_str(), indicator.c_str(), status.c_str()};
    Res res(PQexecParams(conn_, R"(
        SELECT runbook_id,agent_id,indicator,status,url,markdown,notes,created_by,created_at
        FROM runbooks
        WHERE (agent_id=$1 OR agent_id='') AND (indicator=$2 OR indicator='*') AND status=$3
        ORDER BY CASE WHEN indicator='*' THEN 1 ELSE 0 END,
                 CASE WHEN agent_id='' THEN 1 ELSE 0 END, runbook_id DESC
        LIMIT 1;
    )",
                         3, nullptr, params, nullptr, nullptr, 0));
    check_res(res.r, "get_runbook");
    if (PQntuples(res.r) == 0)
        return std::nullopt;
    RunbookRecord r;
    r.runbook_id = col_int64(res.r, 0, 0);
    r.agent_id = col(res.r, 0, 1);
    r.indicator = col(res.r, 0, 2);
    r.status = col(res.r, 0, 3);
    r.url = col(res.r, 0, 4);
    r.markdown = col(res.r, 0, 5);
    r.notes = col(res.r, 0, 6);
    r.created_by = col(res.r, 0, 7);
    r.created_at = col_int64(res.r, 0, 8);
    return r;
}

} // namespace thewatcher::server

#endif // HAVE_LIBPQ
