// Applies the agent state that a successful command ACK confirms.
#pragma once

#include "common/commands.hpp"
#include "offline_alerts.hpp"
#include "status_engine.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>

namespace thewatcher::server
{

// Updates the stored agent after it acknowledged a command of `type`.
//
// Only three command types confirm a state change:
// - DISCONNECT: the agent is disconnected, and the offline scan is told this
//   was deliberate so it raises no Heartbeat alert (#156).
// - PAUSE: the agent enters maintenance. When the API already stored a
//   maintenance reason and expiry before sending the command, those are kept;
//   otherwise it is open-ended maintenance with the reason "command"
//   (StatusEngine::pause_agent).
// - RESUME: the agent leaves maintenance.
// Every other type (SET_INTERVAL, SET_PROCESS_LIMIT, RESTART_COLLECTORS,
// GET_STATUS) is left alone here, in particular an operator's maintenance
// window, its reason and its expiry. (Before this runs, the frame handler has
// already recorded the ACK frame as traffic, like any inbound frame: last_seen
// and connected=true.)
//
// Maintenance changes go through `status_engine`, the server's one engine, so
// its own maintenance bookkeeping and per-agent caches stay in step with the
// stored flag (#43, #44); a temporary engine would leave the server's copy
// stale. The store updates are narrow (touch_agent, and set_agent_maintenance
// inside the engine) instead of a whole-row rewrite from a snapshot, which
// would revert an operator save made concurrently. Nothing happens for an
// agent deleted since the command was sent.
inline void apply_command_ack(Store& store, OfflineAlerts& offline_alerts, StatusEngine& status_engine,
                              const std::string& agent_id, CommandType type, int64_t ack_time)
{
    if (type != CommandType::DISCONNECT && type != CommandType::PAUSE && type != CommandType::RESUME)
        return;

    const auto agent = store.get_agent(agent_id);
    if (!agent)
        return;

    switch (type)
    {
    case CommandType::DISCONNECT:
        store.touch_agent(agent_id, ack_time, false);
        offline_alerts.mark_operator_disconnect(agent_id);
        break;
    case CommandType::PAUSE:
        store.touch_agent(agent_id, ack_time, agent->connected);
        status_engine.pause_agent(agent_id, ack_time);
        break;
    case CommandType::RESUME:
        store.touch_agent(agent_id, ack_time, agent->connected);
        // Clears the stored flag, reason and expiry as well as the engine's state.
        status_engine.exit_maintenance(agent_id);
        break;
    default:
        // Filtered out above.
        break;
    }
}

} // namespace thewatcher::server
