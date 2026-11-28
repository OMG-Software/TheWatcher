#pragma once

#include "store.hpp"

#include <memory>
#include <mutex>

namespace thewatcher::server
{

// Makes any Store safe to share between threads by holding one mutex for the
// whole of every call. A multi-statement operation (a transaction inside the
// inner store) therefore runs without any other call interleaving, and the
// single database connection is only ever used by one thread at a time.
//
// It implements the full Store interface, so adding a virtual method to Store
// without forwarding it here fails to compile: a new method cannot forget the
// lock. Inner stores must never call back into this wrapper.
class SynchronizedStore final : public Store
{
public:
    explicit SynchronizedStore(std::unique_ptr<Store> inner)
        : inner_(std::move(inner))
    {
    }

    void upsert_agent(const AgentRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->upsert_agent(rec);
    }

    std::optional<AgentRecord> get_agent(const std::string& agent_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_agent(agent_id);
    }

    std::vector<AgentRecord> list_agents() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_agents();
    }

    std::vector<AgentRecord> list_approved_agents() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_approved_agents();
    }

    std::vector<AgentRecord> list_pending_agents() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_pending_agents();
    }

    void approve_agent(const std::string& agent_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->approve_agent(agent_id);
    }

    void reject_agent(const std::string& agent_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->reject_agent(agent_id);
    }

    void delete_agent(const std::string& agent_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_agent(agent_id);
    }

    void mark_agents_offline_before(int64_t cutoff_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->mark_agents_offline_before(cutoff_ms);
    }

    void set_agent_maintenance(const std::string& agent_id, bool maintenance, const std::string& reason,
                               int64_t until_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_maintenance(agent_id, maintenance, reason, until_ms);
    }

    void set_agent_thresholds(const AgentRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_thresholds(rec);
    }

    void set_agent_collector_config(const std::string& agent_id, const CollectorConfig& config) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_collector_config(agent_id, config);
    }

    void touch_agent(const std::string& agent_id, int64_t last_seen_ms, bool connected) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->touch_agent(agent_id, last_seen_ms, connected);
    }

    void set_agent_collection_interval(const std::string& agent_id, int seconds) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_collection_interval(agent_id, seconds);
    }

    void set_agent_heartbeat_interval(const std::string& agent_id, int seconds) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_heartbeat_interval(agent_id, seconds);
    }

    void set_agent_process_limit(const std::string& agent_id, int limit) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_process_limit(agent_id, limit);
    }

    bool save_agent_configuration(const std::string& agent_id, const AgentConfigUpdate& update) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->save_agent_configuration(agent_id, update);
    }

    std::vector<GroupRecord> list_groups() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_groups();
    }

    int64_t create_group(const std::string& name) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_group(name);
    }

    std::vector<int64_t> get_agent_groups(const std::string& agent_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_agent_groups(agent_id);
    }

    void set_agent_groups(const std::string& agent_id, const std::vector<int64_t>& group_ids) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_groups(agent_id, group_ids);
    }

    std::optional<UserRecord> get_user_by_username(const std::string& username) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_user_by_username(username);
    }

    std::vector<UserRecord> list_users() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_users();
    }

    int64_t create_user(const std::string& username, const std::string& password_hash, const std::string& role) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_user(username, password_hash, role);
    }

    void set_user_groups(int64_t user_id, const std::vector<int64_t>& group_ids) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_user_groups(user_id, group_ids);
    }

    std::vector<int64_t> get_user_groups(int64_t user_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_user_groups(user_id);
    }

    int disable_user(int64_t user_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->disable_user(user_id);
    }

    int enable_user(int64_t user_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->enable_user(user_id);
    }

    int delete_user(int64_t user_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_user(user_id);
    }

    void update_user_password(int64_t user_id, const std::string& password_hash) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->update_user_password(user_id, password_hash);
    }

    void create_session(const SessionRecord& session) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_session(session);
    }

    std::optional<SessionRecord> get_session(const std::string& token, int64_t now_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_session(token, now_ms);
    }

    void delete_session(const std::string& token) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_session(token);
    }

    int delete_sessions_for_user(int64_t user_id, const std::string& keep_token = "") override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_sessions_for_user(user_id, keep_token);
    }

    int delete_expired_sessions(int64_t now_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_expired_sessions(now_ms);
    }

    bool create_session_if_credentials_current(const SessionRecord& session, const std::string& password_hash) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_session_if_credentials_current(session, password_hash);
    }

    int update_user_password_and_revoke_sessions(int64_t user_id, const std::string& password_hash,
                                                 const std::string& keep_token = "") override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->update_user_password_and_revoke_sessions(user_id, password_hash, keep_token);
    }

    int disable_user_and_revoke_sessions(int64_t user_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->disable_user_and_revoke_sessions(user_id);
    }

    void insert_metrics(const MetricsRow& row) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->insert_metrics(row);
    }

    std::vector<MetricsRow> get_metrics(const std::string& agent_id, int limit = 100) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_metrics(agent_id, limit);
    }

    std::vector<MetricsRow> get_metrics_in_window(const std::string& agent_id, int64_t since_ms,
                                                  int64_t until_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_metrics_in_window(agent_id, since_ms, until_ms);
    }

    std::vector<MetricsRow> latest_metrics() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->latest_metrics();
    }

    void insert_status_history(const StatusHistoryRow& row) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->insert_status_history(row);
    }

    std::optional<StatusHistoryRow> latest_status_for_indicator(const std::string& agent_id,
                                                                const std::string& indicator) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->latest_status_for_indicator(agent_id, indicator);
    }

    std::vector<StatusHistoryRow> list_status_history(const std::string& agent_id, int limit = 100) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_status_history(agent_id, limit);
    }

    std::optional<PendingStatusRecord> get_pending_status(const std::string& agent_id,
                                                          const std::string& indicator) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_pending_status(agent_id, indicator);
    }

    void set_pending_status(const std::string& agent_id, const std::string& indicator, const std::string& target_status,
                            int count) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_pending_status(agent_id, indicator, target_status, count);
    }

    void clear_pending_status(const std::string& agent_id, const std::string& indicator) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->clear_pending_status(agent_id, indicator);
    }

    int64_t insert_alert(const AlertRecord& alert) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->insert_alert(alert);
    }

    bool insert_heartbeat_alert_if_absent(const AlertRecord& alert) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->insert_heartbeat_alert_if_absent(alert);
    }

    std::vector<AlertRecord> list_alerts(bool include_deleted = false) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_alerts(include_deleted);
    }

    std::vector<AlertRecord> list_unacknowledged_alerts() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_unacknowledged_alerts();
    }

    void acknowledge_alert(int64_t alert_id, const std::string& username, int64_t acknowledged_at,
                           const std::string& note = "") override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->acknowledge_alert(alert_id, username, acknowledged_at, note);
    }

    void bulk_acknowledge_alerts(const std::vector<int64_t>& alert_ids, const std::string& username,
                                 int64_t acknowledged_at, const std::string& note = "") override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->bulk_acknowledge_alerts(alert_ids, username, acknowledged_at, note);
    }

    void soft_delete_alert(int64_t alert_id, int64_t deleted_at) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->soft_delete_alert(alert_id, deleted_at);
    }

    void bulk_soft_delete_alerts(const std::vector<int64_t>& alert_ids, int64_t deleted_at) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->bulk_soft_delete_alerts(alert_ids, deleted_at);
    }

    void clear_active_alerts_for_agent(const std::string& agent_id, int64_t cleared_at) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->clear_active_alerts_for_agent(agent_id, cleared_at);
    }

    void set_agent_description(const std::string& agent_id, const std::string& description) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_description(agent_id, description);
    }

    void set_agent_runbook(const std::string& agent_id, const std::string& markdown) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_agent_runbook(agent_id, markdown);
    }

    std::vector<std::string> get_offline_unalerted_agent_ids() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_offline_unalerted_agent_ids();
    }

    void archive_heartbeat_alerts_for_agent(const std::string& agent_id, int64_t deleted_at) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->archive_heartbeat_alerts_for_agent(agent_id, deleted_at);
    }

    void escalate_old_alerts(int64_t cutoff_ms, int64_t now_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->escalate_old_alerts(cutoff_ms, now_ms);
    }

    int64_t count_metrics_in_window(const std::string& agent_id, int64_t since_ms, int64_t until_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->count_metrics_in_window(agent_id, since_ms, until_ms);
    }

    int64_t create_maintenance_window(const MaintenanceWindowRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_maintenance_window(rec);
    }

    std::vector<MaintenanceWindowRecord> list_maintenance_windows() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_maintenance_windows();
    }

    void delete_maintenance_window(int64_t window_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_maintenance_window(window_id);
    }

    std::vector<MaintenanceWindowRecord> active_maintenance_windows(int64_t now_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->active_maintenance_windows(now_ms);
    }

    std::string get_setting(const std::string& key, const std::string& fallback = "") override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_setting(key, fallback);
    }

    void set_setting(const std::string& key, const std::string& value) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->set_setting(key, value);
    }

    int64_t create_silence(const SilenceRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_silence(rec);
    }

    std::vector<SilenceRecord> list_silences() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_silences();
    }

    void delete_silence(int64_t silence_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_silence(silence_id);
    }

    bool is_silenced(const std::string& agent_id, const std::string& indicator, int64_t now_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->is_silenced(agent_id, indicator, now_ms);
    }

    void prune_metrics_before(int64_t cutoff_ms) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->prune_metrics_before(cutoff_ms);
    }

    void insert_log_match(const LogMatchRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->insert_log_match(rec);
    }

    std::vector<LogMatchRecord> list_log_matches(const std::string& agent_id, int limit = 200) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_log_matches(agent_id, limit);
    }

    int64_t create_view(const ViewRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_view(rec);
    }

    std::optional<ViewRecord> get_view(int64_t view_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_view(view_id);
    }

    std::vector<ViewRecord> list_views(int64_t user_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_views(user_id);
    }

    void update_view(const ViewRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->update_view(rec);
    }

    void delete_view(int64_t view_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_view(view_id);
    }

    int64_t create_runbook(const RunbookRecord& rec) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->create_runbook(rec);
    }

    std::vector<RunbookRecord> list_runbooks() override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->list_runbooks();
    }

    void delete_runbook(int64_t runbook_id) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->delete_runbook(runbook_id);
    }

    std::optional<RunbookRecord> get_runbook(const std::string& agent_id, const std::string& indicator,
                                             const std::string& status) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return inner_->get_runbook(agent_id, indicator, status);
    }

private:
    std::unique_ptr<Store> inner_;
    std::mutex mutex_;
};

} // namespace thewatcher::server
