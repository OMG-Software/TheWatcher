#include "disk_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace thewatcher::agent
{

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

namespace
{

#ifdef __linux__

} // namespace

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

namespace
{

    std::vector<DiskMetrics> read_mounts()
    {
        std::vector<DiskMetrics> disks;
        std::ifstream mounts("/proc/mounts");
        std::string device;
        std::string mount_point;
        std::string filesystem;

        while (mounts >> device >> mount_point >> filesystem)
        {
            mounts.ignore(1024, '\n');
            if (device.rfind("/dev/", 0) != 0)
            {
                continue;
            }

            std::error_code ec;
            auto space = std::filesystem::space(mount_point, ec);
            if (ec)
            {
                continue;
            }

            DiskMetrics disk;
            disk.device = device;
            disk.mount_point = mount_point;
            disk.filesystem = filesystem;
            apply_space_info(disk, space);
            disks.push_back(std::move(disk));
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

    std::vector<DiskMetrics> read_mounts()
    {
        std::vector<DiskMetrics> disks;
        std::error_code ec;
        const auto root = std::filesystem::current_path(ec).root_path();
        if (ec || root.empty())
        {
            return disks;
        }

        auto space = std::filesystem::space(root, ec);
        if (ec)
        {
            return disks;
        }

        DiskMetrics disk;
        disk.device = root.string();
        disk.mount_point = root.string();
        disk.filesystem = "unknown";
        apply_space_info(disk, space);
        disks.push_back(std::move(disk));
        return disks;
    }

#endif

#ifdef __linux__

    std::string device_basename(const std::string& path)
    {
        const auto pos = path.find_last_of("/\\");
        return pos == std::string::npos ? path : path.substr(pos + 1);
    }

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

void DiskCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    auto disks = read_mounts();

#ifdef __linux__
    auto current_io = read_io_samples();
    for (auto& disk : disks)
    {
        const auto key = device_basename(disk.device);
        auto current = current_io.find(key);
        auto previous = prev_io_.find(key);
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
#endif

    LOGF_TRACE("Disk collector updated mounts=%zu", disks.size());
    metrics.disks = std::move(disks);
}

} // namespace thewatcher::agent
