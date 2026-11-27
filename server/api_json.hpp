#pragma once

// nlohmann/json adapters for the wire-side metric structs.
// Lives in server/ so common/ stays free of JSON dependencies. Used only by api.cpp
// to render the same JSON shape the React dashboard previously consumed.

#include "common/metrics.hpp"

#include <nlohmann/json.hpp>

namespace thewatcher
{

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CpuMetrics, usage_percent, user_percent, system_percent, idle_percent,
                                                iowait_percent, per_core_usage, frequency_mhz, num_physical_cores,
                                                num_logical_cores, load_avg_1m, load_avg_5m, load_avg_15m)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(MemoryMetrics, total_bytes, used_bytes, free_bytes, available_bytes,
                                                cached_bytes, buffers_bytes, usage_percent, swap_total_bytes,
                                                swap_used_bytes, swap_free_bytes)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DiskMetrics, device, mount_point, filesystem, total_bytes, used_bytes,
                                                free_bytes, usage_percent, read_bytes_per_sec, write_bytes_per_sec)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(TemperatureMetrics, sensor_name, sensor_label, temperature_celsius)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProcessInfo, pid, name, status, cpu_percent, memory_rss_bytes,
                                                memory_vms_bytes, username, num_threads)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NetworkMetrics, interface_name, bytes_sent_per_sec, bytes_recv_per_sec,
                                                packets_sent_per_sec, packets_recv_per_sec, errors_in, errors_out,
                                                drops_in, drops_out, ip_address, mac_address, is_up, is_loopback)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SystemMetrics, cpu, memory, disks, temperatures, top_processes,
                                                networks, os_name, os_version, hostname, platform, uptime_seconds)

} // namespace thewatcher
