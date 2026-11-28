#pragma once

#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/text_safety.hpp"

namespace thewatcher::server
{

// Issue #56: agent-reported strings may hold invalid UTF-8 (a Latin-1
// hostname, a mount point in a legacy code page, a log line with binary
// bytes). nlohmann::json throws when it dumps them and PostgreSQL rejects
// them, so every string is cleaned once, on ingest, before the status engine,
// the store or a webhook sees it. Invalid bytes become U+FFFD.
//
// Returns true when any string changed, so the caller knows a stored copy of
// the original payload no longer matches.
inline bool sanitize_agent_text(SystemMetrics& metrics)
{
    bool changed = false;
    const auto clean = [&changed](std::string& value) {
        changed = text::sanitize_utf8_in_place(value) || changed;
    };
    clean(metrics.os_name);
    clean(metrics.os_version);
    clean(metrics.hostname);
    clean(metrics.platform);
    for (auto& disk : metrics.disks)
    {
        clean(disk.device);
        clean(disk.mount_point);
        clean(disk.filesystem);
    }
    for (auto& temperature : metrics.temperatures)
    {
        clean(temperature.sensor_name);
        clean(temperature.sensor_label);
    }
    for (auto& process : metrics.top_processes)
    {
        clean(process.name);
        clean(process.status);
        clean(process.username);
    }
    for (auto& network : metrics.networks)
    {
        clean(network.interface_name);
        clean(network.ip_address);
        clean(network.mac_address);
    }
    return changed;
}

inline bool sanitize_agent_text(proto::LogMatch& match)
{
    bool changed = false;
    for (auto* value : {&match.indicator_name, &match.path, &match.matched_line, &match.severity})
        changed = text::sanitize_utf8_in_place(*value) || changed;
    return changed;
}

} // namespace thewatcher::server
