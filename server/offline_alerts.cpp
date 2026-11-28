#include "offline_alerts.hpp"

#include "common/SingleLog.hpp"
#include "indicators.hpp"

#include <nlohmann/json.hpp>

#include <vector>

namespace thewatcher::server
{

namespace
{
    constexpr const char* kSettingKey = "agents.operator_disconnected";
} // namespace

OfflineAlerts::OfflineAlerts(Store& store, AlertPipeline& pipeline)
    : store_(store)
    , pipeline_(pipeline)
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

int OfflineAlerts::raise_for_offline_agents(int64_t now_ms)
{
    int raised = 0;
    for (const auto& agent_id : store_.get_offline_unalerted_agent_ids())
    {
        if (is_operator_disconnected(agent_id))
            continue;
        const auto outcome = pipeline_.raise({agent_id, std::string(indicators::kHeartbeat), "green", "red",
                                              "Agent has gone offline (no heartbeat received)", now_ms, true});
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
