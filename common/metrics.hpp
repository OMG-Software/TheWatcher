#pragma once

#include "protocol.hpp"

#include <cbor.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace thewatcher
{

struct CpuMetrics
{
    double usage_percent = 0.0;
    double user_percent = 0.0;
    double system_percent = 0.0;
    double idle_percent = 0.0;
    double iowait_percent = 0.0;
    std::vector<double> per_core_usage;
    double frequency_mhz = 0.0;
    int num_physical_cores = 0;
    int num_logical_cores = 0;
    double load_avg_1m = 0.0;
    double load_avg_5m = 0.0;
    double load_avg_15m = 0.0;
};

struct MemoryMetrics
{
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    uint64_t available_bytes = 0;
    uint64_t cached_bytes = 0;
    uint64_t buffers_bytes = 0;
    double usage_percent = 0.0;
    uint64_t swap_total_bytes = 0;
    uint64_t swap_used_bytes = 0;
    uint64_t swap_free_bytes = 0;
};

struct DiskMetrics
{
    std::string device;
    std::string mount_point;
    std::string filesystem;
    uint64_t total_bytes = 0;
    uint64_t used_bytes = 0;
    uint64_t free_bytes = 0;
    double usage_percent = 0.0;
    uint64_t read_bytes_per_sec = 0;
    uint64_t write_bytes_per_sec = 0;
};

struct TemperatureMetrics
{
    std::string sensor_name;
    std::string sensor_label;
    double temperature_celsius = 0.0;
};

struct ProcessInfo
{
    uint32_t pid = 0;
    std::string name;
    std::string status;
    double cpu_percent = 0.0;
    uint64_t memory_rss_bytes = 0;
    uint64_t memory_vms_bytes = 0;
    std::string username;
    int num_threads = 0;
};

struct NetworkMetrics
{
    std::string interface_name;
    uint64_t bytes_sent_per_sec = 0;
    uint64_t bytes_recv_per_sec = 0;
    uint64_t packets_sent_per_sec = 0;
    uint64_t packets_recv_per_sec = 0;
    uint64_t errors_in = 0;
    uint64_t errors_out = 0;
    uint64_t drops_in = 0;
    uint64_t drops_out = 0;
    std::string ip_address;
    std::string mac_address;
    bool is_up = false;
};

struct SystemMetrics
{
    CpuMetrics cpu;
    MemoryMetrics memory;
    std::vector<DiskMetrics> disks;
    std::vector<TemperatureMetrics> temperatures;
    std::vector<ProcessInfo> top_processes;
    std::vector<NetworkMetrics> networks;
    std::string os_name;
    std::string os_version;
    std::string hostname;
    std::string platform;
    double uptime_seconds = 0.0;
};

} // namespace thewatcher

namespace thewatcher::proto::detail
{

inline cbor_item_t* make_uint64(uint64_t value)
{
    return cbor_build_uint64(value);
}

inline cbor_item_t* make_int(int value)
{
    if (value >= 0)
    {
        return cbor_build_uint64(static_cast<uint64_t>(value));
    }

    return cbor_build_negint64(static_cast<uint64_t>(-(static_cast<int64_t>(value) + 1)));
}

inline cbor_item_t* make_double(double value)
{
    return cbor_build_float8(value);
}

inline uint64_t read_uint64(cbor_item_t* item)
{
    if (!cbor_isa_uint(item))
    {
        throw std::runtime_error("Expected CBOR unsigned integer");
    }

    // Use the width-aware helper from protocol.hpp; cbor_get_uint64 alone reads
    // 8 raw bytes which is only valid for CBOR_INT_64-width items.
    return cbor_uint_value(item);
}

inline uint32_t read_uint32(cbor_item_t* item)
{
    const uint64_t value = read_uint64(item);
    if (value > std::numeric_limits<uint32_t>::max())
    {
        throw std::runtime_error("CBOR integer does not fit uint32_t");
    }

    return static_cast<uint32_t>(value);
}

inline int read_int(cbor_item_t* item)
{
    const int64_t value = read_int64(item);
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max())
    {
        throw std::runtime_error("CBOR integer does not fit int");
    }

    return static_cast<int>(value);
}

// Reads a CBOR float of any width (half, single or double). Simple values
// (null, undefined, true, false) share the float/ctrl major type but carry no
// float payload, so they are rejected: cbor_float_get_float8() on them, or on a
// narrower float, reads past the item's data (issue #139).
inline double read_double(cbor_item_t* item)
{
    if (!cbor_is_float(item))
    {
        throw std::runtime_error("Expected CBOR floating point value");
    }

    return cbor_float_get_float(item);
}

template <>
inline CborPtr to_cbor(const std::vector<double>& values)
{
    auto root = adopt(cbor_new_definite_array(values.size()));

    for (double value : values)
    {
        push(root.get(), make_double(value));
    }

    return root;
}

inline std::vector<double> read_double_vector(cbor_item_t* item)
{
    if (!cbor_isa_array(item))
    {
        throw std::runtime_error("Expected CBOR array of doubles");
    }

    std::vector<double> out;
    out.reserve(cbor_array_size(item));

    for (size_t i = 0; i < cbor_array_size(item); ++i)
    {
        out.push_back(read_double(array_get(item, i)));
    }

    return out;
}

// to_cbor_vector and read_vector are defined AFTER all per-struct overloads
// (search for "── vector helpers ──" lower in this file). They use the per-struct
// to_cbor/from_cbor and must see them at template-definition time per the
// C++ qualified-lookup rule for dependent expressions.

template <>
inline CborPtr to_cbor(const thewatcher::CpuMetrics& m)
{
    auto root = adopt(cbor_new_definite_array(12));

    push(root.get(), make_double(m.usage_percent));
    push(root.get(), make_double(m.user_percent));
    push(root.get(), make_double(m.system_percent));
    push(root.get(), make_double(m.idle_percent));
    push(root.get(), make_double(m.iowait_percent));

    auto per_core = to_cbor(m.per_core_usage);
    push(root.get(), per_core.release());

    push(root.get(), make_double(m.frequency_mhz));
    push(root.get(), make_int(m.num_physical_cores));
    push(root.get(), make_int(m.num_logical_cores));
    push(root.get(), make_double(m.load_avg_1m));
    push(root.get(), make_double(m.load_avg_5m));
    push(root.get(), make_double(m.load_avg_15m));

    return root;
}

template <>
inline thewatcher::CpuMetrics from_cbor<thewatcher::CpuMetrics>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 12)
    {
        throw std::runtime_error("Invalid CpuMetrics CBOR payload");
    }

    thewatcher::CpuMetrics m;
    m.usage_percent = read_double(array_get(item, 0));
    m.user_percent = read_double(array_get(item, 1));
    m.system_percent = read_double(array_get(item, 2));
    m.idle_percent = read_double(array_get(item, 3));
    m.iowait_percent = read_double(array_get(item, 4));
    m.per_core_usage = read_double_vector(array_get(item, 5));
    m.frequency_mhz = read_double(array_get(item, 6));
    m.num_physical_cores = read_int(array_get(item, 7));
    m.num_logical_cores = read_int(array_get(item, 8));
    m.load_avg_1m = read_double(array_get(item, 9));
    m.load_avg_5m = read_double(array_get(item, 10));
    m.load_avg_15m = read_double(array_get(item, 11));

    return m;
}

template <>
inline CborPtr to_cbor(const thewatcher::MemoryMetrics& m)
{
    auto root = adopt(cbor_new_definite_array(10));

    push(root.get(), make_uint64(m.total_bytes));
    push(root.get(), make_uint64(m.used_bytes));
    push(root.get(), make_uint64(m.free_bytes));
    push(root.get(), make_uint64(m.available_bytes));
    push(root.get(), make_uint64(m.cached_bytes));
    push(root.get(), make_uint64(m.buffers_bytes));
    push(root.get(), make_double(m.usage_percent));
    push(root.get(), make_uint64(m.swap_total_bytes));
    push(root.get(), make_uint64(m.swap_used_bytes));
    push(root.get(), make_uint64(m.swap_free_bytes));

    return root;
}

template <>
inline thewatcher::MemoryMetrics from_cbor<thewatcher::MemoryMetrics>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 10)
    {
        throw std::runtime_error("Invalid MemoryMetrics CBOR payload");
    }

    thewatcher::MemoryMetrics m;
    m.total_bytes = read_uint64(array_get(item, 0));
    m.used_bytes = read_uint64(array_get(item, 1));
    m.free_bytes = read_uint64(array_get(item, 2));
    m.available_bytes = read_uint64(array_get(item, 3));
    m.cached_bytes = read_uint64(array_get(item, 4));
    m.buffers_bytes = read_uint64(array_get(item, 5));
    m.usage_percent = read_double(array_get(item, 6));
    m.swap_total_bytes = read_uint64(array_get(item, 7));
    m.swap_used_bytes = read_uint64(array_get(item, 8));
    m.swap_free_bytes = read_uint64(array_get(item, 9));

    return m;
}

template <>
inline CborPtr to_cbor(const thewatcher::DiskMetrics& m)
{
    auto root = adopt(cbor_new_definite_array(9));

    push(root.get(), make_string(m.device));
    push(root.get(), make_string(m.mount_point));
    push(root.get(), make_string(m.filesystem));
    push(root.get(), make_uint64(m.total_bytes));
    push(root.get(), make_uint64(m.used_bytes));
    push(root.get(), make_uint64(m.free_bytes));
    push(root.get(), make_double(m.usage_percent));
    push(root.get(), make_uint64(m.read_bytes_per_sec));
    push(root.get(), make_uint64(m.write_bytes_per_sec));

    return root;
}

template <>
inline thewatcher::DiskMetrics from_cbor<thewatcher::DiskMetrics>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 9)
    {
        throw std::runtime_error("Invalid DiskMetrics CBOR payload");
    }

    thewatcher::DiskMetrics m;
    m.device = read_string(array_get(item, 0));
    m.mount_point = read_string(array_get(item, 1));
    m.filesystem = read_string(array_get(item, 2));
    m.total_bytes = read_uint64(array_get(item, 3));
    m.used_bytes = read_uint64(array_get(item, 4));
    m.free_bytes = read_uint64(array_get(item, 5));
    m.usage_percent = read_double(array_get(item, 6));
    m.read_bytes_per_sec = read_uint64(array_get(item, 7));
    m.write_bytes_per_sec = read_uint64(array_get(item, 8));

    return m;
}

template <>
inline CborPtr to_cbor(const thewatcher::TemperatureMetrics& m)
{
    auto root = adopt(cbor_new_definite_array(3));

    push(root.get(), make_string(m.sensor_name));
    push(root.get(), make_string(m.sensor_label));
    push(root.get(), make_double(m.temperature_celsius));

    return root;
}

template <>
inline thewatcher::TemperatureMetrics from_cbor<thewatcher::TemperatureMetrics>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 3)
    {
        throw std::runtime_error("Invalid TemperatureMetrics CBOR payload");
    }

    thewatcher::TemperatureMetrics m;
    m.sensor_name = read_string(array_get(item, 0));
    m.sensor_label = read_string(array_get(item, 1));
    m.temperature_celsius = read_double(array_get(item, 2));

    return m;
}

template <>
inline CborPtr to_cbor(const thewatcher::ProcessInfo& p)
{
    auto root = adopt(cbor_new_definite_array(8));

    push(root.get(), make_uint64(p.pid));
    push(root.get(), make_string(p.name));
    push(root.get(), make_string(p.status));
    push(root.get(), make_double(p.cpu_percent));
    push(root.get(), make_uint64(p.memory_rss_bytes));
    push(root.get(), make_uint64(p.memory_vms_bytes));
    push(root.get(), make_string(p.username));
    push(root.get(), make_int(p.num_threads));

    return root;
}

template <>
inline thewatcher::ProcessInfo from_cbor<thewatcher::ProcessInfo>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 8)
    {
        throw std::runtime_error("Invalid ProcessInfo CBOR payload");
    }

    thewatcher::ProcessInfo p;
    p.pid = read_uint32(array_get(item, 0));
    p.name = read_string(array_get(item, 1));
    p.status = read_string(array_get(item, 2));
    p.cpu_percent = read_double(array_get(item, 3));
    p.memory_rss_bytes = read_uint64(array_get(item, 4));
    p.memory_vms_bytes = read_uint64(array_get(item, 5));
    p.username = read_string(array_get(item, 6));
    p.num_threads = read_int(array_get(item, 7));

    return p;
}

template <>
inline CborPtr to_cbor(const thewatcher::NetworkMetrics& m)
{
    auto root = adopt(cbor_new_definite_array(12));

    push(root.get(), make_string(m.interface_name));
    push(root.get(), make_uint64(m.bytes_sent_per_sec));
    push(root.get(), make_uint64(m.bytes_recv_per_sec));
    push(root.get(), make_uint64(m.packets_sent_per_sec));
    push(root.get(), make_uint64(m.packets_recv_per_sec));
    push(root.get(), make_uint64(m.errors_in));
    push(root.get(), make_uint64(m.errors_out));
    push(root.get(), make_uint64(m.drops_in));
    push(root.get(), make_uint64(m.drops_out));
    push(root.get(), make_string(m.ip_address));
    push(root.get(), make_string(m.mac_address));
    push(root.get(), make_bool(m.is_up));

    return root;
}

template <>
inline thewatcher::NetworkMetrics from_cbor<thewatcher::NetworkMetrics>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 12)
    {
        throw std::runtime_error("Invalid NetworkMetrics CBOR payload");
    }

    thewatcher::NetworkMetrics m;
    m.interface_name = read_string(array_get(item, 0));
    m.bytes_sent_per_sec = read_uint64(array_get(item, 1));
    m.bytes_recv_per_sec = read_uint64(array_get(item, 2));
    m.packets_sent_per_sec = read_uint64(array_get(item, 3));
    m.packets_recv_per_sec = read_uint64(array_get(item, 4));
    m.errors_in = read_uint64(array_get(item, 5));
    m.errors_out = read_uint64(array_get(item, 6));
    m.drops_in = read_uint64(array_get(item, 7));
    m.drops_out = read_uint64(array_get(item, 8));
    m.ip_address = read_string(array_get(item, 9));
    m.mac_address = read_string(array_get(item, 10));
    m.is_up = read_bool(array_get(item, 11));

    return m;
}

// ── vector helpers ───────────────────────────────────────────────────────────
// Defined after per-struct overloads so to_cbor(value) / from_cbor<T>(item)
// resolve to the right specialisation when these templates are instantiated.

template <typename T>
inline CborPtr to_cbor_vector(const std::vector<T>& values)
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
inline std::vector<T> read_vector(cbor_item_t* item)
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
inline CborPtr to_cbor(const thewatcher::SystemMetrics& m)
{
    auto root = adopt(cbor_new_definite_array(11));

    auto cpu = to_cbor(m.cpu);
    auto memory = to_cbor(m.memory);
    auto disks = to_cbor_vector(m.disks);
    auto temperatures = to_cbor_vector(m.temperatures);
    auto top_processes = to_cbor_vector(m.top_processes);
    auto networks = to_cbor_vector(m.networks);

    push(root.get(), cpu.release());
    push(root.get(), memory.release());
    push(root.get(), disks.release());
    push(root.get(), temperatures.release());
    push(root.get(), top_processes.release());
    push(root.get(), networks.release());
    push(root.get(), make_string(m.os_name));
    push(root.get(), make_string(m.os_version));
    push(root.get(), make_string(m.hostname));
    push(root.get(), make_string(m.platform));
    push(root.get(), make_double(m.uptime_seconds));

    return root;
}

template <>
inline thewatcher::SystemMetrics from_cbor<thewatcher::SystemMetrics>(cbor_item_t* item)
{
    if (!cbor_isa_array(item) || cbor_array_size(item) != 11)
    {
        throw std::runtime_error("Invalid SystemMetrics CBOR payload");
    }

    thewatcher::SystemMetrics m;
    m.cpu = from_cbor<thewatcher::CpuMetrics>(array_get(item, 0));
    m.memory = from_cbor<thewatcher::MemoryMetrics>(array_get(item, 1));
    m.disks = read_vector<thewatcher::DiskMetrics>(array_get(item, 2));
    m.temperatures = read_vector<thewatcher::TemperatureMetrics>(array_get(item, 3));
    m.top_processes = read_vector<thewatcher::ProcessInfo>(array_get(item, 4));
    m.networks = read_vector<thewatcher::NetworkMetrics>(array_get(item, 5));
    m.os_name = read_string(array_get(item, 6));
    m.os_version = read_string(array_get(item, 7));
    m.hostname = read_string(array_get(item, 8));
    m.platform = read_string(array_get(item, 9));
    m.uptime_seconds = read_double(array_get(item, 10));

    return m;
}

} // namespace thewatcher::proto::detail