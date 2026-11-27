#pragma once

#include "collector.hpp"
#include "common/collector_config.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace thewatcher::agent
{

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
