#include "offline_alerts.hpp"

#include "common/SingleLog.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <vector>

namespace thewatcher::server
{

namespace
{
    constexpr const char* kSettingKey = "agents.operator_disconnected";
} // namespace

int effective_offline_timeout_seconds(int offline_after_seconds, int heartbeat_interval_seconds)
{
    return std::max(offline_after_seconds, kOfflineHeartbeatMultiple * std::max(0, heartbeat_interval_seconds));
}

OfflineAlerts::OfflineAlerts(Store& store, AlertPipeline& pipeline, int default_offline_after_seconds)
    : store_(store)
    , pipeline_(pipeline)
    , default_offline_after_seconds_(default_offline_after_seconds)
{
    // A malformed setting is treated as empty: the worst case is an alert for
    // an agent that was disconnected on purpose before the restart.
    try
    {
        const auto stored = nlohmann::json::parse(store_.get_setting(kSettingKey, "[]"));
        for (const auto& id : stored)
            operator_disconnected_.insert(id.get<std::string>());
    }
    catch (const std::exception& e)
    {
        LOGF_WARNING("Ignoring malformed %s setting: %s", kSettingKey, e.what());
        operator_disconnected_.clear();
    }
}

void OfflineAlerts::mark_operator_disconnect(const std::string& agent_id)
{
    if (operator_disconnected_.insert(agent_id).second)
        persist();
}

void OfflineAlerts::agent_seen(const std::string& agent_id)
{
    if (operator_disconnected_.erase(agent_id) > 0)
        persist();
}

bool OfflineAlerts::is_operator_disconnected(const std::string& agent_id) const
{
    return operator_disconnected_.count(agent_id) > 0;
}

int OfflineAlerts::timeout_seconds_for(int heartbeat_interval_seconds) const
{
    int base_seconds = default_offline_after_seconds_;
    const auto setting = store_.get_setting("offline_after_seconds", "");
    if (!setting.empty())
    {
        try
        {
            base_seconds = std::stoi(setting);
        }
        catch (const std::exception&)
        {
            LOGF_WARNING("Ignoring invalid offline_after_seconds setting '%s'", setting.c_str());
        }
    }
    // The saved setting overrides the config in both directions; detection is
    // off only when the resulting base is not positive.
    if (base_seconds <= 0)
        return 0;
    return effective_offline_timeout_seconds(base_seconds, heartbeat_interval_seconds);
}

bool OfflineAlerts::detection_enabled() const
{
    return timeout_seconds_for(0) > 0;
}

std::vector<std::string> OfflineAlerts::mark_silent_agents_offline(int64_t now_ms)
{
    std::vector<std::string> gone;
    if (!detection_enabled())
        return gone;

    for (const auto& agent : store_.list_approved_agents())
    {
        if (!agent.connected || agent.maintenance || agent.last_seen <= 0)
            continue;
        const auto timeout_s = timeout_seconds_for(agent.heartbeat_interval);
        if (agent.last_seen < now_ms - static_cast<int64_t>(timeout_s) * 1000)
        {
            store_.mark_agent_offline(agent.agent_id);
            gone.push_back(agent.agent_id);
        }
    }
    return gone;
}

int OfflineAlerts::raise_for_offline_agents(int64_t now_ms)
{
    if (!detection_enabled())
        return 0;
    mark_silent_agents_offline(now_ms);

    int raised = 0;
    for (const auto& agent_id : store_.get_offline_unalerted_agent_ids())
    {
        if (is_operator_disconnected(agent_id))
            continue;
        const auto outcome = pipeline_.raise({agent_id, "Heartbeat", "green", "red",
                                              "Agent has gone offline (no heartbeat received)", now_ms, false, true});
        if (outcome == AlertOutcome::Raised)
        {
            ++raised;
            LOGF_INFO("Created dead-agent alert for agent_id=%s", agent_id.c_str());
        }
    }
    return raised;
}

void OfflineAlerts::persist()
{
    store_.set_setting(
        kSettingKey,
        nlohmann::json(std::vector<std::string>(operator_disconnected_.begin(), operator_disconnected_.end())).dump());
}

} // namespace thewatcher::server
