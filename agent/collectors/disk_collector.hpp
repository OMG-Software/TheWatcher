#pragma once

#include "collector.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace thewatcher::agent
{

// Fills disk's total/free/used bytes and usage_percent from space, as df does
// (issue #158). free_bytes is space.available: the space that unprivileged
// writers can still use. used_bytes is capacity - free, so blocks reserved for
// root (5% by default on ext4) count as neither used nor free.
// usage_percent = used / (used + available), so usage reaches 100% when a
// normal process gets ENOSPC.
void apply_space_info(DiskMetrics& disk, const std::filesystem::space_info& space);

// One row of a mount table (/proc/self/mounts or getmntinfo).
struct MountEntry
{
    std::string device;
    std::string mount_point;
    std::string filesystem;
};

// Decodes the octal escapes (\040 for a space, \011 tab, \012 newline, \134
// backslash) the Linux kernel writes into /proc/self/mounts fields. Anything
// that is not a complete three-digit octal escape is left as is (issue #96).
std::string unescape_mount_field(std::string_view field);

// Parses /proc/self/mounts text. Lines with fewer than three fields are
// skipped; device and mount point are unescaped.
std::vector<MountEntry> parse_mount_table(std::string_view text);

// True for filesystems that have no storage worth monitoring: kernel pseudo
// filesystems, stacked filesystems, and squashfs (snap packages), which is
// always 100% used.
bool is_pseudo_filesystem(std::string_view filesystem);

// Keeps the mounts worth reporting: drops pseudo filesystems and loop devices,
// and keeps only the first mount of each device so bind mounts and btrfs
// subvolumes are not reported twice. With require_block_device only /dev/*
// devices are kept (Linux); BSD passes false so ZFS datasets survive.
std::vector<MountEntry> filter_mounts(const std::vector<MountEntry>& entries, bool require_block_device);

// Name under which a mounted device appears in /proc/diskstats. dm_names maps
// a device-mapper name (the /dev/mapper/<name> alias) to its dm-N node, as
// read from /sys/block/dm-N/dm/name; other devices use their basename.
std::string diskstats_name_for_device(std::string_view device,
                                      const std::unordered_map<std::string, std::string>& dm_names);

class DiskCollector final : public Collector
{
public:
    std::string_view name() const noexcept override
    {
        return "disk";
    }
    void update(SystemMetrics& metrics) override;

private:
    struct IoSample
    {
        uint64_t read_sectors = 0;
        uint64_t write_sectors = 0;
        std::chrono::steady_clock::time_point sampled_at{};
    };

    std::unordered_map<std::string, IoSample> prev_io_;

#ifdef __linux__
    std::unordered_map<std::string, IoSample> read_io_samples() const;
#endif
};

} // namespace thewatcher::agent
