#pragma once

#include "collector.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace thewatcher::agent
{

/// Busy percentage (0-100) between two idle/total tick counter readings.
/// Returns 0 when the counters did not advance or went backwards (wrap, reset).
double cpu_percent_from_delta(uint64_t previous_idle, uint64_t previous_total, uint64_t current_idle,
                              uint64_t current_total) noexcept;

class CpuCollector final : public Collector
{
public:
    /// Takes the baseline tick sample so the first update() reports a real value.
    CpuCollector();

    std::string_view name() const noexcept override
    {
        return "cpu";
    }
    void update(SystemMetrics& metrics) override;

    // Raw tick counters at one instant. Public so the per-platform readers can fill it.
    struct Sample
    {
        uint64_t idle = 0;
        uint64_t total = 0;
        std::vector<uint64_t> core_idle;
        std::vector<uint64_t> core_total;
        std::chrono::steady_clock::time_point sampled_at{};
    };

private:
    // Reads the platform tick counters; total == 0 means "unavailable".
    static Sample read_sample();

    Sample prev_{};
    bool primed_{false};
    // Result of the last delta computation, reused when update() is called again too soon.
    bool have_result_{false};
    double last_usage_{0.0};
    std::vector<double> last_per_core_;
};

} // namespace thewatcher::agent
