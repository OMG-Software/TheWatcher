#pragma once

#include "common/collector_config.hpp"
#include "common/metrics.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

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
bool is_worse_status(IndicatorStatus previous, IndicatorStatus current);

// True when a silence rule covers an alert on `indicator`: a rule for the exact
// name, for its kind ("disk" covers "disk:/") or for '*'. See indicators.hpp.
bool is_alert_silenced(Store& store, const std::string& agent_id, const std::string& indicator, int64_t now_ms);

// The JSON body POSTed to the webhook for a new alert. It carries the runbook
// link attached to the alert and the agent's own markdown runbook (#53).
nlohmann::json build_webhook_payload(Store& store, const AlertRecord& alert, int64_t alert_id);

class StatusEngine
{
public:
    explicit StatusEngine(Store& store);

    void evaluate_metrics(const std::string& agent_id, const SystemMetrics& metrics, int64_t timestamp_ms);
    void enter_maintenance(const std::string& agent_id, const std::string& reason, int64_t until_ms, int64_t now_ms);
    void exit_maintenance(const std::string& agent_id);

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

    Store& store_;
    int64_t last_prune_ms_ = 0;
    std::unordered_map<std::string, BaselineCacheEntry> baseline_cache_;
    std::unordered_map<std::string, StalenessEntry> staleness_cache_;
};

} // namespace thewatcher::server
