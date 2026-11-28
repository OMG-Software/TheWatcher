#pragma once

#include "alert_pipeline.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace thewatcher::server
{

// Number of heartbeat intervals an agent may stay silent before it counts as
// offline, however small the configured offline_after_seconds is (#46).
constexpr int kOfflineHeartbeatMultiple = 3;

// The silence allowed before an agent is marked offline:
// max(offline_after_seconds, kOfflineHeartbeatMultiple * heartbeat_interval).
int effective_offline_timeout_seconds(int offline_after_seconds, int heartbeat_interval_seconds);

// Raises the "agent has gone offline" alert through the AlertPipeline and
// remembers agents an operator disconnected on purpose (#156).
//
// An operator DISCONNECT is a deliberate state, not a failure: such an agent
// raises no Heartbeat alert, so nothing escalates. The mark is kept in the
// store (setting agents.operator_disconnected) so it survives a restart, and
// is cleared when the agent next sends a frame, i.e. connects again.
//
// It also owns the offline rule (#46): an agent is offline once silent for
// longer than max(base, 3 x its heartbeat interval), where base is the saved
// "offline_after_seconds" setting, else default_offline_after_seconds (the
// config value). A non-positive base disables offline detection.
class OfflineAlerts
{
public:
    OfflineAlerts(Store& store, AlertPipeline& pipeline, int default_offline_after_seconds = 60);

    // The effective offline timeout for an agent with this heartbeat interval,
    // or 0 when offline detection is disabled. The saved setting wins over the
    // configured default in both directions.
    int timeout_seconds_for(int heartbeat_interval_seconds) const;

    bool detection_enabled() const;

    // Records that an operator disconnected agent_id on purpose.
    void mark_operator_disconnect(const std::string& agent_id);

    // The agent sent a frame: it is connected again, so the mark is cleared.
    void agent_seen(const std::string& agent_id);

    bool is_operator_disconnected(const std::string& agent_id) const;

    // Marks approved, connected agents silent for longer than their timeout as
    // offline (agents in maintenance are skipped) and returns their ids.
    // Does nothing when detection is disabled.
    std::vector<std::string> mark_silent_agents_offline(int64_t now_ms);

    // Marks silent agents offline, then raises a Heartbeat alert for each
    // approved, disconnected agent that has none open, except
    // operator-disconnected ones. Returns the number raised. Does nothing when
    // offline detection is disabled.
    int raise_for_offline_agents(int64_t now_ms);

private:
    void persist();

    Store& store_;
    AlertPipeline& pipeline_;
    int default_offline_after_seconds_;
    std::unordered_set<std::string> operator_disconnected_;
};

} // namespace thewatcher::server
