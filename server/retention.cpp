#include "retention.hpp"

#include "common/SingleLog.hpp"

#include <exception>
#include <string>

namespace thewatcher::server
{

namespace
{
    constexpr int64_t kDayMs = 86'400'000LL;

    // Reads a retention setting in days; defaults on missing or invalid values.
    int retention_days(Store& store, const char* key, int default_days)
    {
        const auto value = store.get_setting(key, "");
        if (value.empty())
            return default_days;
        try
        {
            const int days = std::stoi(value);
            return days >= 1 ? days : default_days;
        }
        catch (const std::exception&)
        {
            LOGF_WARNING("Ignoring invalid retention setting %s='%s'", key, value.c_str());
            return default_days;
        }
    }

    int64_t cutoff_for(int64_t now_ms, int days)
    {
        return now_ms - static_cast<int64_t>(days) * kDayMs;
    }
} // namespace

void run_retention(Store& store, int64_t now_ms)
{
    const auto metrics_days = retention_days(store, "metrics_retention_days", 30);
    const auto history_days = retention_days(store, "status_history_retention_days", 90);
    const auto alerts_days = retention_days(store, "alerts_retention_days", 90);
    const auto log_days = retention_days(store, "log_matches_retention_days", 30);

    store.prune_metrics_before(cutoff_for(now_ms, metrics_days));
    store.prune_status_history_before(cutoff_for(now_ms, history_days));
    store.prune_alerts_before(cutoff_for(now_ms, alerts_days));
    store.prune_log_matches_before(cutoff_for(now_ms, log_days));
    store.prune_silences_before(cutoff_for(now_ms, alerts_days));
    store.prune_maintenance_windows_before(cutoff_for(now_ms, alerts_days));
    LOGF_DEBUG("Retention run complete metrics=%dd history=%dd alerts=%dd log_matches=%dd", metrics_days, history_days,
               alerts_days, log_days);
}

} // namespace thewatcher::server
