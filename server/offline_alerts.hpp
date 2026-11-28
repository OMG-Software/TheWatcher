#pragma once

#include "alert_pipeline.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>

namespace thewatcher::server
{

// Raises the "agent has gone offline" alert through the AlertPipeline and
// remembers agents an operator disconnected on purpose (#156).
//
// An operator DISCONNECT is a deliberate state, not a failure: such an agent
// raises no Heartbeat alert, so nothing escalates. The mark is kept in the
// store (setting agents.operator_disconnected) so it survives a restart, and
// is cleared when the agent next sends a frame, i.e. connects again.
class OfflineAlerts
{
public:
    OfflineAlerts(Store& store, AlertPipeline& pipeline);

    // Records that an operator disconnected agent_id on purpose.
    void mark_operator_disconnect(const std::string& agent_id);

    // The agent sent a frame: it is connected again, so the mark is cleared.
    void agent_seen(const std::string& agent_id);

    bool is_operator_disconnected(const std::string& agent_id) const;

    // Raises a Heartbeat alert for each approved, disconnected agent that has
    // none open, except operator-disconnected ones. Returns the number raised.
    int raise_for_offline_agents(int64_t now_ms);

private:
    void persist();

    Store& store_;
    AlertPipeline& pipeline_;
    std::unordered_set<std::string> operator_disconnected_;
};

} // namespace thewatcher::server
