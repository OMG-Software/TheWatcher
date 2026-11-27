#include "disk_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif !defined(__linux__)
#include <sys/param.h>
// clang-format off
#include <sys/mount.h>
// clang-format on
#if defined(__NetBSD__)
#include <sys/statvfs.h>
#endif
#endif

namespace thewatcher::agent
{

// Fills disk's size fields from space as df does: free_bytes is the space
// available to unprivileged writers, and usage is used / (used + available).
// Clamps inconsistent filesystem values so used_bytes cannot underflow.
void apply_space_info(DiskMetrics& disk, const std::filesystem::space_info& space)
{
    const auto capacity = space.capacity;
    // available <= free <= capacity on every platform; clamp in case a
    // filesystem reports inconsistent values.
    const auto free = std::min(space.free, capacity);
    const auto available = std::min(space.available, free);
    disk.total_bytes = capacity;
    disk.free_bytes = available;
    disk.used_bytes = capacity - free;
    const auto usable = disk.used_bytes + available;
    disk.usage_percent = usable > 0 ? 100.0 * static_cast<double>(disk.used_bytes) / static_cast<double>(usable) : 0.0;
}

std::string unescape_mount_field(std::string_view field)
{
    std::string out;
    out.reserve(field.size());
    const auto is_octal = [](char c) {
        return c >= '0' && c <= '7';
    };
    for (std::size_t i = 0; i < field.size(); ++i)
    {
        if (field[i] == '\\' && i + 3 < field.size() && is_octal(field[i + 1]) && is_octal(field[i + 2]) &&
            is_octal(field[i + 3]))
        {
            out.push_back(
                static_cast<char>(((field[i + 1] - '0') << 6) | ((field[i + 2] - '0') << 3) | (field[i + 3] - '0')));
            i += 3;
        }
        else
        {
            out.push_back(field[i]);
        }
    }
    return out;
}

std::vector<MountEntry> parse_mount_table(std::string_view text)
{
    std::vector<MountEntry> entries;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line))
    {
        std::istringstream row(line);
        std::string device;
        std::string mount_point;
        std::string filesystem;
        if (!(row >> device >> mount_point >> filesystem))
            continue;
        entries.push_back({unescape_mount_field(device), unescape_mount_field(mount_point), filesystem, {}});
    }
    return entries;
}

bool is_pseudo_filesystem(std::string_view filesystem)
{
    static constexpr std::string_view pseudo[] = {
        "proc",     "procfs",    "linprocfs",  "linsysfs", "sysfs",      "devtmpfs",    "devpts",
        "devfs",    "fdescfs",   "tmpfs",      "ramfs",    "mfs",        "cgroup",      "cgroup2",
        "pstore",   "bpf",       "debugfs",    "tracefs",  "securityfs", "configfs",    "fusectl",
        "mqueue",   "hugetlbfs", "autofs",     "overlay",  "squashfs",   "binfmt_misc", "nsfs",
        "efivarfs", "selinuxfs", "rpc_pipefs", "nullfs",   "unionfs",    "kernfs",      "ptyfs"};
    return std::find(std::begin(pseudo), std::end(pseudo), filesystem) != std::end(pseudo);
}

std::vector<MountEntry> filter_mounts(const std::vector<MountEntry>& entries, bool require_block_device,
                                      const DeviceResolver& resolve)
{
    std::vector<MountEntry> kept;
    for (const auto& entry : entries)
    {
        if (is_pseudo_filesystem(entry.filesystem))
            continue;
        if (require_block_device && entry.device.rfind("/dev/", 0) != 0)
            continue;
        // Resolve once: aliases such as /dev/disk/by-id/... may point at a loop device or
        // at a device already reported.
        MountEntry resolved = entry;
        resolved.canonical_device = resolve ? resolve(entry.device) : entry.device;
        if (resolved.canonical_device.rfind("/dev/loop", 0) == 0)
            continue;
        // Bind mounts and btrfs subvolumes repeat the device: report it once.
        const bool seen = std::any_of(kept.begin(), kept.end(), [&](const MountEntry& k) {
            return k.canonical_device == resolved.canonical_device;
        });
        if (!seen)
            kept.push_back(std::move(resolved));
    }
    return kept;
}

std::string diskstats_name_for_device(std::string_view device,
                                      const std::unordered_map<std::string, std::string>& dm_names)
{
    const auto pos = device.find_last_of("/\\");
    const std::string base(pos == std::string_view::npos ? device : device.substr(pos + 1));
    const auto dm = dm_names.find(base);
    return dm != dm_names.end() ? dm->second : base;
}

namespace
{

#ifdef __linux__

    std::vector<DiskMetrics> read_mounts(std::vector<std::string>& io_names)
    {
        std::vector<DiskMetrics> disks;
        std::ifstream mounts("/proc/self/mounts");
        const std::string text((std::istreambuf_iterator<char>(mounts)), std::istreambuf_iterator<char>());

        // dm-N names are what /proc/diskstats uses; /dev/mapper/<name> is an alias.
        std::unordered_map<std::string, std::string> dm_names;
        std::error_code ec;
        for (const auto& dir : std::filesystem::directory_iterator("/sys/block", ec))
        {
            const auto node = dir.path().filename().string();
            if (node.rfind("dm-", 0) != 0)
                continue;
            std::ifstream name_file(dir.path() / "dm" / "name");
            std::string dm_name;
            if (std::getline(name_file, dm_name) && !dm_name.empty())
                dm_names[dm_name] = node;
        }

        // Follow symlinks (/dev/disk/by-uuid/..., /dev/mapper/...) to the real node, once:
        // the result drives loop filtering, de-duplication and the diskstats lookup.
        const DeviceResolver resolve = [](const std::string& device) {
            std::error_code resolve_ec;
            const auto real = std::filesystem::canonical(device, resolve_ec);
            return resolve_ec ? device : real.string();
        };
        for (const auto& entry : filter_mounts(parse_mount_table(text), true, resolve))
        {
            auto space = std::filesystem::space(entry.mount_point, ec);
            if (ec)
                continue;

            DiskMetrics disk;
            disk.device = entry.device;
            disk.mount_point = entry.mount_point;
            disk.filesystem = entry.filesystem;
            apply_space_info(disk, space);
            disks.push_back(std::move(disk));

            io_names.push_back(diskstats_name_for_device(entry.canonical_device, dm_names));
        }
        return disks;
    }

#elif defined(_WIN32)

    std::vector<DiskMetrics> read_mounts()
    {
        std::vector<DiskMetrics> disks;
        const DWORD mask = ::GetLogicalDrives();
        if (mask == 0)
        {
            return disks;
        }

        for (int i = 0; i < 26; ++i)
        {
            if ((mask & (1u << i)) == 0)
            {
                continue;
            }

            char root[4] = {static_cast<char>('A' + i), ':', '\\', '\0'};
            if (::GetDriveTypeA(root) != DRIVE_FIXED)
            {
                continue;
            }

            std::error_code ec;
            auto space = std::filesystem::space(root, ec);
            if (ec)
            {
                continue;
            }

            char fs_name[MAX_PATH + 1] = {};
            if (!::GetVolumeInformationA(root, nullptr, 0, nullptr, nullptr, nullptr, fs_name, sizeof(fs_name)))
            {
                fs_name[0] = '\0';
            }

            DiskMetrics disk;
            disk.device = root;
            disk.mount_point = root;
            disk.filesystem = fs_name[0] != '\0' ? std::string(fs_name) : std::string("unknown");
            apply_space_info(disk, space);
            disks.push_back(std::move(disk));
        }

        return disks;
    }

#else

// BSD: getmntinfo returns a pointer into libc-owned static storage, so there is
// nothing to free. NetBSD uses statvfs; the other BSDs use statfs. Both have
// f_fstypename, f_mntfromname and f_mntonname.
#if defined(__NetBSD__)
    using MountStat = struct statvfs;
#else
    using MountStat = struct statfs;
#endif

    std::vector<DiskMetrics> read_mounts()
    {
        std::vector<DiskMetrics> disks;
        MountStat* table = nullptr;
        const int count = ::getmntinfo(&table, MNT_NOWAIT);
        if (count <= 0 || table == nullptr)
        {
            return disks;
        }

        std::vector<MountEntry> entries;
        for (int i = 0; i < count; ++i)
        {
            entries.push_back({table[i].f_mntfromname, table[i].f_mntonname, table[i].f_fstypename, {}});
        }

        for (const auto& entry : filter_mounts(entries, false))
        {
            std::error_code ec;
            auto space = std::filesystem::space(entry.mount_point, ec);
            if (ec)
            {
                continue;
            }

            DiskMetrics disk;
            disk.device = entry.device;
            disk.mount_point = entry.mount_point;
            disk.filesystem = entry.filesystem;
            apply_space_info(disk, space);
            disks.push_back(std::move(disk));
        }
        return disks;
    }

#endif

#ifdef __linux__

    uint64_t sectors_per_second(uint64_t previous, uint64_t current, double elapsed_seconds)
    {
        if (elapsed_seconds <= 0.0 || current < previous)
        {
            return 0;
        }

        constexpr uint64_t sector_size = 512;
        return static_cast<uint64_t>(static_cast<double>((current - previous) * sector_size) / elapsed_seconds);
    }

#endif

} // namespace

#ifdef __linux__

std::unordered_map<std::string, DiskCollector::IoSample> DiskCollector::read_io_samples() const
{
    std::unordered_map<std::string, DiskCollector::IoSample> samples;
    std::ifstream diskstats("/proc/diskstats");
    std::string line;
    const auto now = std::chrono::steady_clock::now();

    while (std::getline(diskstats, line))
    {
        std::istringstream row(line);
        int major = 0;
        int minor = 0;
        std::string device;
        uint64_t reads_completed = 0;
        uint64_t reads_merged = 0;
        uint64_t read_sectors = 0;
        uint64_t read_time_ms = 0;
        uint64_t writes_completed = 0;
        uint64_t writes_merged = 0;
        uint64_t write_sectors = 0;

        row >> major >> minor >> device >> reads_completed >> reads_merged >> read_sectors >> read_time_ms >>
            writes_completed >> writes_merged >> write_sectors;

        if (!device.empty())
        {
            samples[device] = {read_sectors, write_sectors, now};
        }
    }

    return samples;
}

#endif

void DiskCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
#ifdef __linux__
    std::vector<std::string> io_names; // parallel to disks: each mount's /proc/diskstats name
    auto disks = read_mounts(io_names);

    auto current_io = read_io_samples();
    for (std::size_t i = 0; i < disks.size(); ++i)
    {
        auto& disk = disks[i];
        auto current = current_io.find(io_names[i]);
        auto previous = prev_io_.find(io_names[i]);
        if (current != current_io.end() && previous != prev_io_.end())
        {
            const auto elapsed =
                std::chrono::duration<double>(current->second.sampled_at - previous->second.sampled_at).count();
            disk.read_bytes_per_sec =
                sectors_per_second(previous->second.read_sectors, current->second.read_sectors, elapsed);
            disk.write_bytes_per_sec =
                sectors_per_second(previous->second.write_sectors, current->second.write_sectors, elapsed);
        }
    }
    prev_io_ = std::move(current_io);
#else
    auto disks = read_mounts();
#endif

    LOGF_TRACE("Disk collector updated mounts=%zu", disks.size());
    metrics.disks = std::move(disks);
}

} // namespace thewatcher::agent
