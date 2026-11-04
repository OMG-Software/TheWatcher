#pragma once

#include "protocol.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace thewatcher
{

struct PercentThresholds
{
    double warning_percent = 80.0;
    double degraded_percent = 90.0;
    double critical_percent = 95.0;
};

struct NetworkThresholds
{
    double warning_mbps = 100.0;
    double degraded_mbps = 200.0;
    double critical_mbps = 300.0;
};

// Anomaly detection config for one indicator.
// When multiplier > 0 the server alerts if the current sample exceeds
// baseline_mean * multiplier (computed over the rolling window).
struct AnomalyConfig
{
    double multiplier = 0.0;        // 0 = disabled
    int baseline_window_hours = 24;
};

struct DiskMonitorConfig
{
    std::string mount_point;
    std::string device;
    bool enabled = true;
    PercentThresholds thresholds;
    AnomalyConfig anomaly;
};

struct NetworkInterfaceConfig
{
    std::string interface_name;
    bool enabled = true;
    NetworkThresholds thresholds;
    AnomalyConfig anomaly;
};

struct ProcessWatchConfig
{
    std::string name;
    int expected_count = 1;
    bool enabled = true;
};

struct LogMonitorConfig
{
    std::string path;
    std::string pattern;
    std::string indicator_name;
    std::string severity = "red"; // "yellow", "amber", or "red"
    bool enabled = true;
};

struct CollectorConfig
{
    PercentThresholds cpu;
    PercentThresholds memory;
    int cpu_readings = 1;
    int memory_readings = 1;
    int disk_readings = 1;
    int network_readings = 1;
    int process_readings = 3;
    std::vector<DiskMonitorConfig> disks;
    std::vector<NetworkInterfaceConfig> networks;
    std::vector<ProcessWatchConfig> processes;
    AnomalyConfig cpu_anomaly;
    AnomalyConfig memory_anomaly;
    int stale_after_seconds = 0; // 0 = disabled
    std::vector<LogMonitorConfig> logs;
};

inline CollectorConfig default_collector_config()
{
    return {};
}

// Keep JSON support for config files / human-readable config.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PercentThresholds, warning_percent, degraded_percent, critical_percent)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NetworkThresholds, warning_mbps, degraded_mbps, critical_mbps)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(AnomalyConfig, multiplier, baseline_window_hours)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(DiskMonitorConfig, mount_point, device, enabled, thresholds, anomaly)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(NetworkInterfaceConfig, interface_name, enabled, thresholds, anomaly)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(ProcessWatchConfig, name, expected_count, enabled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(LogMonitorConfig, path, pattern, indicator_name, severity, enabled)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(CollectorConfig, cpu, memory, cpu_readings, memory_readings,
                                                disk_readings, network_readings, process_readings, disks, networks,
                                                processes, cpu_anomaly, memory_anomaly, stale_after_seconds, logs)

} // namespace thewatcher

namespace thewatcher::proto::detail
{

// Names are prefixed to avoid collisions with similar helpers in metrics.hpp.

inline cbor_item_t* config_make_double(double value)
{
    return cbor_build_float8(value);
}

inline cbor_item_t* config_make_int(int value)
{
    if (value >= 0)
    {
        return cbor_build_uint64(static_cast<uint64_t>(value));
    }

    return cbor_build_negint64(static_cast<uint64_t>(-(static_cast<int64_t>(value) + 1)));
}

// Reads a CBOR float of any width (half, single or double). Simple values
// (null, undefined, true, false) share the float/ctrl major type but carry no
// float payload, so they are rejected: cbor_float_get_float8() on them, or on a
// narrower float, reads past the item's data (issue #139).
inline double config_read_double(cbor_item_t* item)
{
    if (!cbor_is_float(item))
    {
        throw std::runtime_error("Expected CBOR floating point value");
    }

    return cbor_float_get_float(item);
}

inline int config_read_int(cbor_item_t* item)
{
    const int64_t value = read_int64(item);

    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
    {
        throw std::runtime_error("CBOR integer does not fit int");
    }

    return static_cast<int>(value);
}

// config_to_cbor_vector / config_read_vector are defined further below, AFTER
// the per-struct overloads they dispatch to. This satisfies C++ qualified-lookup
// rules for dependent expressions inside the templates.

template <>
inline CborPtr to_cbor(const thewatcher::PercentThresholds& t)
{
    auto root = adopt(cbor_new_definite_array(3));

    push(root.get(), config_make_double(t.warning_percent));
    push(root.get(), config_make_double(t.degraded_percent));
    push(root.get(), config_make_double(t.critical_percent));

    return root;
}

template <>
inline thewatcher::PercentThresholds from_cbor<thewatcher::PercentThresholds>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 3)
    {
        throw std::runtime_error("Invalid PercentThresholds CBOR payload");
    }

    thewatcher::PercentThresholds t;
    t.warning_percent = config_read_double(array_get(item, 0));
    t.degraded_percent = config_read_double(array_get(item, 1));
    t.critical_percent = config_read_double(array_get(item, 2));

    return t;
}

template <>
inline CborPtr to_cbor(const thewatcher::NetworkThresholds& t)
{
    auto root = adopt(cbor_new_definite_array(3));

    push(root.get(), config_make_double(t.warning_mbps));
    push(root.get(), config_make_double(t.degraded_mbps));
    push(root.get(), config_make_double(t.critical_mbps));

    return root;
}

template <>
inline thewatcher::NetworkThresholds from_cbor<thewatcher::NetworkThresholds>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 3)
    {
        throw std::runtime_error("Invalid NetworkThresholds CBOR payload");
    }

    thewatcher::NetworkThresholds t;
    t.warning_mbps = config_read_double(array_get(item, 0));
    t.degraded_mbps = config_read_double(array_get(item, 1));
    t.critical_mbps = config_read_double(array_get(item, 2));

    return t;
}

template <>
inline CborPtr to_cbor(const thewatcher::AnomalyConfig& a)
{
    auto root = adopt(cbor_new_definite_array(2));
    push(root.get(), config_make_double(a.multiplier));
    push(root.get(), config_make_int(a.baseline_window_hours));
    return root;
}

template <>
inline thewatcher::AnomalyConfig from_cbor<thewatcher::AnomalyConfig>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) < 2)
        throw std::runtime_error("Invalid AnomalyConfig CBOR payload");
    thewatcher::AnomalyConfig a;
    a.multiplier = config_read_double(array_get(item, 0));
    a.baseline_window_hours = config_read_int(array_get(item, 1));
    return a;
}

template <>
inline CborPtr to_cbor(const thewatcher::DiskMonitorConfig& c)
{
    auto root = adopt(cbor_new_definite_array(5));

    push(root.get(), make_string(c.mount_point));
    push(root.get(), make_string(c.device));
    push(root.get(), make_bool(c.enabled));

    auto thresholds = to_cbor(c.thresholds);
    push(root.get(), thresholds.release());

    auto anomaly = to_cbor(c.anomaly);
    push(root.get(), anomaly.release());

    return root;
}

template <>
inline thewatcher::DiskMonitorConfig from_cbor<thewatcher::DiskMonitorConfig>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) < 4)
    {
        throw std::runtime_error("Invalid DiskMonitorConfig CBOR payload");
    }

    thewatcher::DiskMonitorConfig c;
    c.mount_point = read_string(array_get(item, 0));
    c.device = read_string(array_get(item, 1));
    c.enabled = read_bool(array_get(item, 2));
    c.thresholds = from_cbor<thewatcher::PercentThresholds>(array_get(item, 3));
    if (cbor_array_size(item) >= 5)
        c.anomaly = from_cbor<thewatcher::AnomalyConfig>(array_get(item, 4));

    return c;
}

template <>
inline CborPtr to_cbor(const thewatcher::NetworkInterfaceConfig& c)
{
    auto root = adopt(cbor_new_definite_array(4));

    push(root.get(), make_string(c.interface_name));
    push(root.get(), make_bool(c.enabled));

    auto thresholds = to_cbor(c.thresholds);
    push(root.get(), thresholds.release());

    auto anomaly = to_cbor(c.anomaly);
    push(root.get(), anomaly.release());

    return root;
}

template <>
inline thewatcher::NetworkInterfaceConfig from_cbor<thewatcher::NetworkInterfaceConfig>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) < 3)
    {
        throw std::runtime_error("Invalid NetworkInterfaceConfig CBOR payload");
    }

    thewatcher::NetworkInterfaceConfig c;
    c.interface_name = read_string(array_get(item, 0));
    c.enabled = read_bool(array_get(item, 1));
    c.thresholds = from_cbor<thewatcher::NetworkThresholds>(array_get(item, 2));
    if (cbor_array_size(item) >= 4)
        c.anomaly = from_cbor<thewatcher::AnomalyConfig>(array_get(item, 3));

    return c;
}

template <>
inline CborPtr to_cbor(const thewatcher::ProcessWatchConfig& c)
{
    auto root = adopt(cbor_new_definite_array(3));

    push(root.get(), make_string(c.name));
    push(root.get(), config_make_int(c.expected_count));
    push(root.get(), make_bool(c.enabled));

    return root;
}

template <>
inline thewatcher::ProcessWatchConfig from_cbor<thewatcher::ProcessWatchConfig>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 3)
    {
        throw std::runtime_error("Invalid ProcessWatchConfig CBOR payload");
    }

    thewatcher::ProcessWatchConfig c;
    c.name = read_string(array_get(item, 0));
    c.expected_count = config_read_int(array_get(item, 1));
    c.enabled = read_bool(array_get(item, 2));

    return c;
}

template <>
inline CborPtr to_cbor(const thewatcher::LogMonitorConfig& c)
{
    auto root = adopt(cbor_new_definite_array(5));
    push(root.get(), make_string(c.path));
    push(root.get(), make_string(c.pattern));
    push(root.get(), make_string(c.indicator_name));
    push(root.get(), make_string(c.severity));
    push(root.get(), make_bool(c.enabled));
    return root;
}

template <>
inline thewatcher::LogMonitorConfig from_cbor<thewatcher::LogMonitorConfig>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) < 5)
        throw std::runtime_error("Invalid LogMonitorConfig CBOR payload");
    thewatcher::LogMonitorConfig c;
    c.path = read_string(array_get(item, 0));
    c.pattern = read_string(array_get(item, 1));
    c.indicator_name = read_string(array_get(item, 2));
    c.severity = read_string(array_get(item, 3));
    c.enabled = read_bool(array_get(item, 4));
    return c;
}

// ── vector helpers ───────────────────────────────────────────────────────────
// Defined after per-struct overloads so to_cbor(value) / from_cbor<T>(item)
// resolve to the correct specialisations at template instantiation.

template <typename T>
inline CborPtr config_to_cbor_vector(const std::vector<T>& values)
{
    auto root = adopt(cbor_new_definite_array(values.size()));

    for (const auto& value : values)
    {
        auto child = to_cbor(value);
        push(root.get(), child.release());
    }

    return root;
}

template <typename T>
inline std::vector<T> config_read_vector(cbor_item_t* item)
{
    if (!cbor_isa_array(item))
    {
        throw std::runtime_error("Expected CBOR array");
    }

    std::vector<T> out;
    out.reserve(cbor_array_size(item));

    for (size_t i = 0; i < cbor_array_size(item); ++i)
    {
        out.push_back(from_cbor<T>(array_get(item, i)));
    }

    return out;
}

template <>
inline CborPtr to_cbor(const thewatcher::CollectorConfig& c)
{
    auto root = adopt(cbor_new_definite_array(14));

    auto cpu = to_cbor(c.cpu);
    auto memory = to_cbor(c.memory);
    auto disks = config_to_cbor_vector(c.disks);
    auto networks = config_to_cbor_vector(c.networks);
    auto processes = config_to_cbor_vector(c.processes);
    auto cpu_anomaly = to_cbor(c.cpu_anomaly);
    auto memory_anomaly = to_cbor(c.memory_anomaly);
    auto logs = config_to_cbor_vector(c.logs);

    push(root.get(), cpu.release());
    push(root.get(), memory.release());
    push(root.get(), config_make_int(c.cpu_readings));
    push(root.get(), config_make_int(c.memory_readings));
    push(root.get(), config_make_int(c.disk_readings));
    push(root.get(), config_make_int(c.network_readings));
    push(root.get(), config_make_int(c.process_readings));
    push(root.get(), disks.release());
    push(root.get(), networks.release());
    push(root.get(), processes.release());
    push(root.get(), cpu_anomaly.release());
    push(root.get(), memory_anomaly.release());
    push(root.get(), config_make_int(c.stale_after_seconds));
    push(root.get(), logs.release());

    return root;
}

template <>
inline thewatcher::CollectorConfig from_cbor<thewatcher::CollectorConfig>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) < 10)
    {
        throw std::runtime_error("Invalid CollectorConfig CBOR payload");
    }

    thewatcher::CollectorConfig c;
    c.cpu = from_cbor<thewatcher::PercentThresholds>(array_get(item, 0));
    c.memory = from_cbor<thewatcher::PercentThresholds>(array_get(item, 1));
    c.cpu_readings = config_read_int(array_get(item, 2));
    c.memory_readings = config_read_int(array_get(item, 3));
    c.disk_readings = config_read_int(array_get(item, 4));
    c.network_readings = config_read_int(array_get(item, 5));
    c.process_readings = config_read_int(array_get(item, 6));
    c.disks = config_read_vector<thewatcher::DiskMonitorConfig>(array_get(item, 7));
    c.networks = config_read_vector<thewatcher::NetworkInterfaceConfig>(array_get(item, 8));
    c.processes = config_read_vector<thewatcher::ProcessWatchConfig>(array_get(item, 9));
    if (cbor_array_size(item) >= 12)
    {
        c.cpu_anomaly = from_cbor<thewatcher::AnomalyConfig>(array_get(item, 10));
        c.memory_anomaly = from_cbor<thewatcher::AnomalyConfig>(array_get(item, 11));
    }
    if (cbor_array_size(item) >= 13)
        c.stale_after_seconds = config_read_int(array_get(item, 12));
    if (cbor_array_size(item) >= 14)
        c.logs = config_read_vector<thewatcher::LogMonitorConfig>(array_get(item, 13));

    return c;
}

} // namespace thewatcher::proto::detail
