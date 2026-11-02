#pragma once

#ifdef HAVE_LIBPQ

#include "store.hpp"

#include <string>

#include <libpq-fe.h>

namespace thewatcher::server
{

class PostgresStore final : public Store
{
public:
    explicit PostgresStore(const std::string& dsn);
    ~PostgresStore() override;

    void upsert_agent(const AgentRecord& rec) override;
    std::optional<AgentRecord> get_agent(const std::string& agent_id) override;
    std::vector<AgentRecord> list_agents() override;
    std::vector<AgentRecord> list_approved_agents() override;
    std::vector<AgentRecord> list_pending_agents() override;
    void approve_agent(const std::string& agent_id) override;
    void reject_agent(const std::string& agent_id) override;
    void delete_agent(const std::string& agent_id) override;
    void mark_agents_offline_before(int64_t cutoff_ms) override;
    void set_agent_maintenance(const std::string& agent_id, bool maintenance, const std::string& reason,
                               int64_t until_ms) override;
    void set_agent_thresholds(const AgentRecord& rec) override;
    void set_agent_collector_config(const std::string& agent_id, const CollectorConfig& config) override;

    std::vector<GroupRecord> list_groups() override;
    int64_t create_group(const std::string& name) override;
    std::vector<int64_t> get_agent_groups(const std::string& agent_id) override;
    void set_agent_groups(const std::string& agent_id, const std::vector<int64_t>& group_ids) override;
    std::optional<UserRecord> get_user_by_username(const std::string& username) override;
    std::vector<UserRecord> list_users() override;
    int64_t create_user(const std::string& username, const std::string& password_hash,
                        const std::string& role) override;
    void set_user_groups(int64_t user_id, const std::vector<int64_t>& group_ids) override;
    std::vector<int64_t> get_user_groups(int64_t user_id) override;
    int disable_user(int64_t user_id) override;
    void enable_user(int64_t user_id) override;
    int delete_user(int64_t user_id) override;
    void update_user_password(int64_t user_id, const std::string& password_hash) override;
    void create_session(const SessionRecord& session) override;
    std::optional<SessionRecord> get_session(const std::string& token, int64_t now_ms) override;
    void delete_session(const std::string& token) override;

    void insert_metrics(const MetricsRow& row) override;
    std::vector<MetricsRow> get_metrics(const std::string& agent_id, int limit) override;
    std::vector<MetricsRow> get_metrics_in_window(const std::string& agent_id, int64_t since_ms,
                                                  int64_t until_ms) override;
    std::vector<MetricsRow> latest_metrics() override;

    void insert_status_history(const StatusHistoryRow& row) override;
    std::optional<StatusHistoryRow> latest_status_for_indicator(const std::string& agent_id,
                                                                const std::string& indicator) override;
    std::vector<StatusHistoryRow> list_status_history(const std::string& agent_id, int limit) override;
    std::optional<PendingStatusRecord> get_pending_status(const std::string& agent_id,
                                                          const std::string& indicator) override;
    void set_pending_status(const std::string& agent_id, const std::string& indicator, const std::string& target_status,
                            int count) override;
    void clear_pending_status(const std::string& agent_id, const std::string& indicator) override;
    int64_t insert_alert(const AlertRecord& alert) override;
    std::vector<AlertRecord> list_alerts(bool include_deleted) override;
    std::vector<AlertRecord> list_unacknowledged_alerts() override;
    void acknowledge_alert(int64_t alert_id, const std::string& username, int64_t acknowledged_at,
                           const std::string& note = "") override;
    void bulk_acknowledge_alerts(const std::vector<int64_t>& alert_ids, const std::string& username,
                                 int64_t acknowledged_at, const std::string& note = "") override;
    void soft_delete_alert(int64_t alert_id, int64_t deleted_at) override;
    void bulk_soft_delete_alerts(const std::vector<int64_t>& alert_ids, int64_t deleted_at) override;
    void clear_active_alerts_for_agent(const std::string& agent_id, int64_t cleared_at) override;
    void set_agent_description(const std::string& agent_id, const std::string& description) override;
    void set_agent_runbook(const std::string& agent_id, const std::string& markdown) override;
    std::vector<std::string> get_offline_unalerted_agent_ids() override;
    void archive_heartbeat_alerts_for_agent(const std::string& agent_id, int64_t deleted_at) override;
    void escalate_old_alerts(int64_t cutoff_ms, int64_t now_ms) override;
    int64_t count_metrics_in_window(const std::string& agent_id, int64_t since_ms, int64_t until_ms) override;
    int64_t create_maintenance_window(const MaintenanceWindowRecord& rec) override;
    std::vector<MaintenanceWindowRecord> list_maintenance_windows() override;
    void delete_maintenance_window(int64_t window_id) override;
    std::vector<MaintenanceWindowRecord> active_maintenance_windows(int64_t now_ms) override;
    std::string get_setting(const std::string& key, const std::string& fallback) override;
    void set_setting(const std::string& key, const std::string& value) override;
    int64_t create_silence(const SilenceRecord& rec) override;
    std::vector<SilenceRecord> list_silences() override;
    void delete_silence(int64_t silence_id) override;
    bool is_silenced(const std::string& agent_id, const std::string& indicator, int64_t now_ms) override;
    void prune_metrics_before(int64_t cutoff_ms) override;
    void insert_log_match(const LogMatchRecord& rec) override;
    std::vector<LogMatchRecord> list_log_matches(const std::string& agent_id, int limit) override;
    int64_t create_view(const ViewRecord& rec) override;
    std::optional<ViewRecord> get_view(int64_t view_id) override;
    std::vector<ViewRecord> list_views(int64_t user_id) override;
    void update_view(const ViewRecord& rec) override;
    void delete_view(int64_t view_id) override;

    int64_t create_runbook(const RunbookRecord& rec) override;
    std::vector<RunbookRecord> list_runbooks() override;
    void delete_runbook(int64_t runbook_id) override;
    std::optional<RunbookRecord> get_runbook(const std::string& indicator, const std::string& status) override;

private:
    void exec(const char* sql);
    bool column_exists(const std::string& table, const std::string& column);
    void add_column_if_missing(const std::string& table, const std::string& column, const char* ddl);
    void init_schema();
    void bootstrap_defaults();

    PGconn* conn_ = nullptr;
};

} // namespace thewatcher::server

#endif // HAVE_LIBPQ
