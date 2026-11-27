#pragma once

#include "collector.hpp"

#include <cstdint>

namespace thewatcher::agent
{

/// a - b, or 0 when b > a (never wraps around).
constexpr uint64_t saturating_sub(uint64_t a, uint64_t b) noexcept
{
    return a > b ? a - b : 0;
}

struct SwapUsage
{
    uint64_t total = 0;
    uint64_t used = 0;
    uint64_t free = 0;
};

/// Swap figures from the total and the amount in use. Always used + free == total;
/// no swap (total == 0) gives 0/0/0 whatever `used` is.
constexpr SwapUsage swap_usage_from_used(uint64_t total, uint64_t used) noexcept
{
    const uint64_t clamped_used = used < total ? used : total;
    return {total, clamped_used, total - clamped_used};
}

/// Swap figures from the total and the amount still free.
constexpr SwapUsage swap_usage_from_free(uint64_t total, uint64_t free) noexcept
{
    const uint64_t clamped_free = free < total ? free : total;
    return {total, total - clamped_free, clamped_free};
}

/// Percentage (0-100) of memory in use, given what is available. 0 when total is 0.
double memory_usage_percent(uint64_t total, uint64_t available) noexcept;

class MemoryCollector final : public Collector
{
public:
    std::string_view name() const noexcept override
    {
        return "memory";
    }
    void update(SystemMetrics& metrics) override;
};

} // namespace thewatcher::agent
