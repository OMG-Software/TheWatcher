#pragma once

#include "collector.hpp"
#include "common/collector_config.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace thewatcher::agent
{

// Per-process record shared by every platform's reader.
struct ProcRecord
{
    uint32_t pid = 0;
    std::string name;     // short name: comm on Linux, image name elsewhere
    std::string exe_name; // executable basename when it differs from name (Linux comm is cut to 15 chars)
    std::string status;
    uint64_t cpu_ticks = 0; // user + system time in the platform's tick unit
    double cpu_percent = 0.0;
    uint64_t rss_bytes = 0;
    uint64_t vms_bytes = 0;
    std::string username;
    int num_threads = 0;
};

// Basename of an executable path (either separator). A " (deleted)" suffix that
// Linux appends to /proc/<pid>/exe is removed.
std::string executable_basename(std::string_view path);

// Basename of the first argument of a NUL separated /proc/<pid>/cmdline.
std::string argv0_basename(std::string_view cmdline);

// True when a watch names this process: it equals the short name or the
// executable basename. Windows image names are matched case-insensitively.
bool process_matches_watch(std::string_view watch_name, std::string_view name, std::string_view exe_name,
                           bool case_insensitive);

// Process CPU on every OS is percent of ONE core (100 = one core fully busy,
// 200 = two cores), so it may exceed 100 on multi-core hosts. ticks_per_second
// is the platform's tick unit (clock ticks, microseconds or 100-ns units).
double cpu_percent_of_one_core(uint64_t delta_ticks, double elapsed_seconds, double ticks_per_second);

// Top `limit` processes by cpu_percent, plus every process matching an enabled
// watch. A process matching a watch is reported under the watch's name so the
// dashboard's exact-name count sees it.
std::vector<ProcRecord> select_reported_processes(std::vector<ProcRecord> all, int limit,
                                                  const std::vector<ProcessWatchConfig>& watches,
                                                  bool case_insensitive);

class ProcessCollector final : public Collector
{
public:
    explicit ProcessCollector(int limit = 25)
        : limit_{limit}
    {
    }

    std::string_view name() const noexcept override
    {
        return "process";
    }
    void update(SystemMetrics& metrics) override;
    void set_limit(int limit)
    {
        limit_ = limit;
    }
    void set_watches(std::vector<ProcessWatchConfig> watches)
    {
        watches_ = std::move(watches);
    }

private:
    int limit_;
    std::vector<ProcessWatchConfig> watches_;

    struct CpuSample
    {
        uint64_t cpu_ticks = 0;
        std::chrono::steady_clock::time_point sampled_at{};
    };

    std::unordered_map<uint32_t, CpuSample> prev_cpu_;
};

} // namespace thewatcher::agent
