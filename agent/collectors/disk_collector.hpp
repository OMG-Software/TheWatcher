#pragma once

#include "collector.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace thewatcher::agent
{

// Fills disk's total/free/used bytes and usage_percent from space, as df does
// (issue #158). free_bytes is space.available: the space that unprivileged
// writers can still use. used_bytes is capacity - free, so blocks reserved for
// root (5% by default on ext4) count as neither used nor free.
// usage_percent = used / (used + available), so usage reaches 100% when a
// normal process gets ENOSPC.
void apply_space_info(DiskMetrics& disk, const std::filesystem::space_info& space);

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
