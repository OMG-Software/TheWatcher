#pragma once

#include "common/collector_config.hpp"
#include "common/metrics.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace thewatcher::server
{

struct AgentRecord
{
    std::string agent_id;
    std::string hostname;
    std::string platform;
    std::string curve_public_key_z85;
    bool approved = false;
    bool rejected = false;
    bool connected = false;
    bool maintenance = false;
    std::string maintenance_reason;
    int64_t maintenance_until = 0;
    int collection_interval = 30;
    int heartbeat_interval = 5;
    int process_limit = 25;
    int64_t first_seen = 0;
    int64_t last_seen = 0;
    double cpu_warning_pct_of_avg = 125.0;
    double cpu_degraded_pct_of_avg = 150.0;
    double cpu_critical_pct_of_avg = 200.0;
    double memory_warning_pct_of_avg = 125.0;
    double memory_degraded_pct_of_avg = 150.0;
    double memory_critical_pct_of_avg = 200.0;
    double disk_warning_pct_of_avg = 125.0;
    double disk_degraded_pct_of_avg = 150.0;
    double disk_critical_pct_of_avg = 200.0;
    double network_warning_pct_of_avg = 125.0;
    double network_degraded_pct_of_avg = 150.0;
    double network_critical_pct_of_avg = 200.0;
    CollectorConfig collector_config;
    std::string description;
    std::string runbook_markdown;
};

struct MetricsRow
{
    std::string agent_id;
    int64_t timestamp_ms = 0;
    std::vector<uint8_t> metrics_cbor; // CBOR-encoded SystemMetrics payload (matches the wire format).
};

struct GroupRecord
{
    int64_t group_id = 0;
    std::string name;
    bool built_in = false;
};

struct UserRecord
{
    int64_t user_id = 0;
    std::string username;
    std::string password_hash;
    std::string role;
    bool built_in = false;
    bool disabled = false;
};

struct SessionRecord
{
    std::string token;
    int64_t user_id = 0;
    std::string username;
    std::string role;
    int64_t created_at = 0;
    int64_t expires_at = 0;
};

struct StatusHistoryRow
{
    int64_t id = 0;
    std::string agent_id;
    std::string indicator;
    std::string old_status;
    std::string new_status;
    std::string message;
    int64_t created_at = 0;
};

struct AlertRecord
{
    int64_t alert_id = 0;
    std::string agent_id;
    std::string indicator;
    std::string old_status;
    std::string new_status;
    std::string message;
    int64_t created_at = 0;
    std::string acknowledged_by;
    int64_t acknowledged_at = 0;
    int64_t deleted_at = 0;
    std::string note;
    int64_t escalated_at = 0;
    std::string runbook_url; // populated from runbooks table when alert is created
    std::string runbook_markdown;
};

struct MaintenanceWindowRecord
{
    int64_t window_id = 0;
    std::string agent_id; // '*' for all agents
    int64_t start_ms = 0;
    int64_t end_ms = 0;
    std::string reason;
    std::string created_by;
    int64_t created_at = 0;
};

struct SilenceRecord
{
    int64_t silence_id = 0;
    std::string agent_id;  // '*' for all agents
    std::string indicator; // '*' for all indicators
    std::string reason;
    int64_t until_ms = 0;
    std::string created_by;
    int64_t created_at = 0;
};

struct PendingStatusRecord
{
    std::string agent_id;
    std::string indicator;
    std::string target_status;
    int count = 0;
};

struct LogMatchRecord
{
    int64_t match_id = 0;
    std::string agent_id;
    std::string indicator_name;
    std::string path;
    std::string matched_line;
    std::string severity;
    int64_t created_at = 0;
};

struct RunbookRecord
{
    int64_t runbook_id = 0;
    std::string agent_id;
    std::string indicator; // exact name like "cpu", or "*" to match any indicator
    std::string status;    // "yellow", "amber", "red"
    std::string url;
    std::string markdown;
    std::string notes;
    std::string created_by;
    int64_t created_at = 0;
};

struct ViewRecord
{
    int64_t view_id = 0;
    std::string name;
    int64_t owner_user_id = 0;
    std::string owner_username; // populated on read
    bool is_public = false;
    int64_t group_id = 0;
    std::vector<std::string> agent_ids; // stored as JSON in config_json
    int64_t created_at = 0;
};

class Store
{
public:
    virtual ~Store() = default;

    virtual void upsert_agent(const AgentRecord& rec) = 0;
    virtual std::optional<AgentRecord> get_agent(const std::string& agent_id) = 0;
    virtual std::vector<AgentRecord> list_agents() = 0;
    virtual std::vector<AgentRecord> list_approved_agents() = 0;
    virtual std::vector<AgentRecord> list_pending_agents() = 0;
    virtual void approve_agent(const std::string& agent_id) = 0;
    virtual void reject_agent(const std::string& agent_id) = 0;
    virtual void delete_agent(const std::string& agent_id) = 0;
    virtual void mark_agents_offline_before(int64_t cutoff_ms) = 0;
    virtual void set_agent_maintenance(const std::string& agent_id, bool maintenance, const std::string& reason,
                                       int64_t until_ms) = 0;
    virtual void set_agent_thresholds(const AgentRecord& rec) = 0;
    virtual void set_agent_collector_config(const std::string& agent_id, const CollectorConfig& config) = 0;

    virtual std::vector<GroupRecord> list_groups() = 0;
    virtual int64_t create_group(const std::string& name) = 0;
    virtual std::vector<int64_t> get_agent_groups(const std::string& agent_id) = 0;
    virtual void set_agent_groups(const std::string& agent_id, const std::vector<int64_t>& group_ids) = 0;
    virtual std::optional<UserRecord> get_user_by_username(const std::string& username) = 0;
    virtual std::vector<UserRecord> list_users() = 0;
    virtual int64_t create_user(const std::string& username, const std::string& password_hash,
                                const std::string& role) = 0;
    virtual void set_user_groups(int64_t user_id, const std::vector<int64_t>& group_ids) = 0;
    virtual std::vector<int64_t> get_user_groups(int64_t user_id) = 0;
    // disable_user/delete_user return the number of affected rows: 0 means
    // the id was unknown or the built_in guard refused the change, which
    // the API layer maps to 404.
    virtual int disable_user(int64_t user_id) = 0;
    virtual void enable_user(int64_t user_id) = 0;
    virtual int delete_user(int64_t user_id) = 0;
    virtual void update_user_password(int64_t user_id, const std::string& password_hash) = 0;
    virtual void create_session(const SessionRecord& session) = 0;
    // Returns the session for token, or nullopt when the token is unknown,
    // expired at now_ms, or belongs to a disabled user.
    virtual std::optional<SessionRecord> get_session(const std::string& token, int64_t now_ms) = 0;
    virtual void delete_session(const std::string& token) = 0;

    virtual void insert_metrics(const MetricsRow& row) = 0;
    virtual std::vector<MetricsRow> get_metrics(const std::string& agent_id, int limit = 100) = 0;
    virtual std::vector<MetricsRow> get_metrics_in_window(const std::string& agent_id, int64_t since_ms,
                                                          int64_t until_ms) = 0;
    virtual std::vector<MetricsRow> latest_metrics() = 0;

    virtual void insert_status_history(const StatusHistoryRow& row) = 0;
    virtual std::optional<StatusHistoryRow> latest_status_for_indicator(const std::string& agent_id,
                                                                        const std::string& indicator) = 0;
    virtual std::vector<StatusHistoryRow> list_status_history(const std::string& agent_id, int limit = 100) = 0;
    virtual std::optional<PendingStatusRecord> get_pending_status(const std::string& agent_id,
                                                                  const std::string& indicator) = 0;
    virtual void set_pending_status(const std::string& agent_id, const std::string& indicator,
                                    const std::string& target_status, int count) = 0;
    virtual void clear_pending_status(const std::string& agent_id, const std::string& indicator) = 0;
    virtual int64_t insert_alert(const AlertRecord& alert) = 0;
    virtual std::vector<AlertRecord> list_alerts(bool include_deleted = false) = 0;
    virtual std::vector<AlertRecord> list_unacknowledged_alerts() = 0;
    virtual void acknowledge_alert(int64_t alert_id, const std::string& username, int64_t acknowledged_at,
                                   const std::string& note = "") = 0;
    virtual void bulk_acknowledge_alerts(const std::vector<int64_t>& alert_ids, const std::string& username,
                                         int64_t acknowledged_at, const std::string& note = "") = 0;
    virtual void soft_delete_alert(int64_t alert_id, int64_t deleted_at) = 0;
    virtual void bulk_soft_delete_alerts(const std::vector<int64_t>& alert_ids, int64_t deleted_at) = 0;
    virtual void clear_active_alerts_for_agent(const std::string& agent_id, int64_t cleared_at) = 0;
    virtual void set_agent_description(const std::string& agent_id, const std::string& description) = 0;
    virtual void set_agent_runbook(const std::string& agent_id, const std::string& markdown) = 0;
    virtual std::vector<std::string> get_offline_unalerted_agent_ids() = 0;
    virtual void archive_heartbeat_alerts_for_agent(const std::string& agent_id, int64_t deleted_at) = 0;
    virtual void escalate_old_alerts(int64_t cutoff_ms, int64_t now_ms) = 0;
    virtual int64_t count_metrics_in_window(const std::string& agent_id, int64_t since_ms, int64_t until_ms) = 0;
    virtual int64_t create_maintenance_window(const MaintenanceWindowRecord& rec) = 0;
    virtual std::vector<MaintenanceWindowRecord> list_maintenance_windows() = 0;
    virtual void delete_maintenance_window(int64_t window_id) = 0;
    virtual std::vector<MaintenanceWindowRecord> active_maintenance_windows(int64_t now_ms) = 0;
    virtual std::string get_setting(const std::string& key, const std::string& fallback = "") = 0;
    virtual void set_setting(const std::string& key, const std::string& value) = 0;
    virtual int64_t create_silence(const SilenceRecord& rec) = 0;
    virtual std::vector<SilenceRecord> list_silences() = 0;
    virtual void delete_silence(int64_t silence_id) = 0;
    virtual bool is_silenced(const std::string& agent_id, const std::string& indicator, int64_t now_ms) = 0;
    virtual void prune_metrics_before(int64_t cutoff_ms) = 0;
    virtual void insert_log_match(const LogMatchRecord& rec) = 0;
    virtual std::vector<LogMatchRecord> list_log_matches(const std::string& agent_id, int limit = 200) = 0;
    virtual int64_t create_view(const ViewRecord& rec) = 0;
    virtual std::optional<ViewRecord> get_view(int64_t view_id) = 0;
    virtual std::vector<ViewRecord> list_views(
        int64_t user_id) = 0; // all when user_id <= 0; otherwise own/public/group
    virtual void update_view(const ViewRecord& rec) = 0;
    virtual void delete_view(int64_t view_id) = 0;

    // Runbook links — attached to alerts by indicator+status on creation.
    virtual int64_t create_runbook(const RunbookRecord& rec) = 0;
    virtual std::vector<RunbookRecord> list_runbooks() = 0;
    virtual void delete_runbook(int64_t runbook_id) = 0;
    // Returns the best runbook for an alert on `agent_id`: only that agent's
    // runbooks or generic ones (empty agent_id) match (SEC-11). Order: exact
    // indicator > '*', then agent-specific > generic, then newest.
    virtual std::optional<RunbookRecord> get_runbook(const std::string& agent_id, const std::string& indicator,
                                                     const std::string& status) = 0;
};

std::unique_ptr<Store> make_store(const std::string& db_type, const std::string& db_path_or_dsn);

} // namespace thewatcher::server
