#pragma once

#include "common/collector_config.hpp"
#include "common/metrics.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace thewatcher::server
{

enum class IndicatorStatus
{
    Green = 0,
    Grey = 1,
    Yellow = 2,
    Amber = 3,
    Red = 4,
    Blue = 5
};

std::string status_to_string(IndicatorStatus status);
IndicatorStatus status_from_string(const std::string& value);
// True when `current` is a more severe problem than `previous` and so should
// raise an alert. Grey (no data) and Blue (maintenance) carry no severity: a
// transition TO either never alerts (#55), and a transition FROM either is
// judged as if there were no previous problem, so Blue->Red alerts (#44) and
// Grey->Green does not.
bool is_worse_status(IndicatorStatus previous, IndicatorStatus current);

// Number of heartbeat intervals an agent may stay silent before it counts as
// offline, however small the configured offline_after_seconds is (#46).
constexpr int kOfflineHeartbeatMultiple = 3;

// The silence allowed before an agent is marked offline:
// max(offline_after_seconds, kOfflineHeartbeatMultiple * heartbeat_interval).
int effective_offline_timeout_seconds(int offline_after_seconds, int heartbeat_interval_seconds);

class StatusEngine
{
public:
    explicit StatusEngine(Store& store);

    void evaluate_metrics(const std::string& agent_id, const SystemMetrics& metrics, int64_t timestamp_ms);
    void enter_maintenance(const std::string& agent_id, const std::string& reason, int64_t until_ms, int64_t now_ms);
    void exit_maintenance(const std::string& agent_id);

    // Applies an agent's acknowledged PAUSE command. An agent already in
    // maintenance (set through the API) keeps its reason and expiry; otherwise
    // it enters open-ended maintenance with the reason "command".
    void pause_agent(const std::string& agent_id, int64_t now_ms);

    // The one maintenance state machine (#44, #45). Called from the server's
    // periodic maintenance tick for every approved agent:
    //  - timed manual maintenance whose until time has passed ends,
    //  - an agent covered by an active maintenance window enters maintenance
    //    (reason "mw:<window reason>"), and one whose window is over leaves it,
    //  - an agent whose maintenance flag was set elsewhere (the API) gets its
    //    indicators marked Blue.
    // Manual and scheduled maintenance both go through enter/exit_maintenance.
    void reconcile_maintenance(int64_t now_ms);

    // Marks approved, connected agents that have been silent for longer than
    // effective_offline_timeout_seconds() as offline and returns their ids.
    // The base timeout is the "offline_after_seconds" server setting, falling
    // back to default_offline_after_seconds (the config value). A non-positive
    // default disables offline detection. Agents in maintenance are skipped.
    std::vector<std::string> mark_offline_agents(int64_t now_ms, int default_offline_after_seconds);

    // The effective offline timeout for an agent with this heartbeat interval:
    // max(base, 3 x heartbeat) where base is the saved "offline_after_seconds"
    // setting, else default_offline_after_seconds. The saved setting wins over
    // the config either way. Returns 0 when offline detection is disabled (the
    // resulting base is not positive).
    int offline_timeout_seconds_for(int heartbeat_interval_seconds, int default_offline_after_seconds);

    // Number of agents with cached state (for tests and diagnostics).
    size_t tracked_agent_count() const;

private:
    struct BaselineCacheEntry
    {
        double mean = -1.0;
        int64_t computed_at = 0;
    };

    struct StalenessEntry
    {
        double last_value = 0.0;
        int64_t value_since_ms = 0; // timestamp when this value was first observed
    };

    IndicatorStatus classify_percent(double value, const PercentThresholds& thresholds) const;
    IndicatorStatus classify_network_mbps(double value, const NetworkThresholds& thresholds) const;
    IndicatorStatus confirm_numeric_status(const std::string& agent_id, const std::string& indicator,
                                           IndicatorStatus previous, IndicatorStatus raw, int required_readings);
    IndicatorStatus process_status_for_count(int missing_count, int readings_to_red) const;
    void record_transition(const std::string& agent_id, const std::string& indicator, IndicatorStatus next,
                           const std::string& message, int64_t timestamp_ms);
    double compute_metric_mean(const std::string& agent_id, const std::string& indicator, int baseline_hours,
                               int64_t now_ms);
    IndicatorStatus maybe_anomaly_status(IndicatorStatus threshold_status, const std::string& agent_id,
                                         const std::string& indicator, double current_value,
                                         const AnomalyConfig& anomaly_cfg, int64_t now_ms);
    IndicatorStatus maybe_stale_status(IndicatorStatus threshold_status, const std::string& agent_id,
                                       const std::string& indicator, double current_value, int stale_after_seconds,
                                       int64_t now_ms);

    // Records Blue for cpu, memory and every indicator the agent already has.
    void mark_indicators_blue(const std::string& agent_id, int64_t now_ms);

    Store& store_;
    // Agents whose indicators were already marked Blue for the current maintenance.
    std::unordered_set<std::string> maintenance_marked_;
    // Drops the stale-value clocks and baselines of one agent.
    void forget_agent_state(const std::string& agent_id);

    // agent_id -> indicator -> state. Nested so one agent's state can be
    // dropped cheaply when it is deleted or leaves maintenance.
    std::unordered_map<std::string, std::unordered_map<std::string, BaselineCacheEntry>> baseline_cache_;
    std::unordered_map<std::string, std::unordered_map<std::string, StalenessEntry>> staleness_cache_;
};

} // namespace thewatcher::server
