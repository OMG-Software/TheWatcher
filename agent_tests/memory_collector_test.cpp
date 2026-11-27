#include "agent/collectors/memory_collector.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <limits>

using namespace thewatcher;
using namespace thewatcher::agent;
using Catch::Matchers::WithinAbs;

SCENARIO("Memory subtraction never underflows")
{
    GIVEN("a minuend larger than the subtrahend")
    {
        THEN("the difference is exact")
        {
            REQUIRE(saturating_sub(10, 3) == 7);
        }
    }
    GIVEN("a subtrahend larger than the minuend")
    {
        THEN("the result is 0, not about 1.8e19")
        {
            REQUIRE(saturating_sub(3, 10) == 0);
            REQUIRE(saturating_sub(0, std::numeric_limits<uint64_t>::max()) == 0);
        }
    }
}

SCENARIO("Swap figures are derived consistently")
{
    GIVEN("a host with 8 GiB of swap of which 2 GiB is in use")
    {
        constexpr uint64_t gib = 1024ULL * 1024 * 1024;

        WHEN("built from total and used")
        {
            const auto swap = swap_usage_from_used(8 * gib, 2 * gib);
            THEN("free is the remainder")
            {
                REQUIRE(swap.total == 8 * gib);
                REQUIRE(swap.used == 2 * gib);
                REQUIRE(swap.free == 6 * gib);
            }
        }
        WHEN("built from total and free")
        {
            const auto swap = swap_usage_from_free(8 * gib, 6 * gib);
            THEN("used is the remainder")
            {
                REQUIRE(swap.used == 2 * gib);
                REQUIRE(swap.free == 6 * gib);
            }
        }
    }

    GIVEN("a host with no swap configured")
    {
        WHEN("the kernel still reports a non-zero reservation or free value")
        {
            const auto from_used = swap_usage_from_used(0, 5000);
            const auto from_free = swap_usage_from_free(0, 5000);
            THEN("swap is reported as 0/0/0")
            {
                REQUIRE(from_used.total == 0);
                REQUIRE(from_used.used == 0);
                REQUIRE(from_used.free == 0);
                REQUIRE(from_free.total == 0);
                REQUIRE(from_free.used == 0);
                REQUIRE(from_free.free == 0);
            }
        }
    }

    GIVEN("a used value larger than the total")
    {
        THEN("used is clamped to the total and free is 0")
        {
            const auto swap = swap_usage_from_used(100, 400);
            REQUIRE(swap.used == 100);
            REQUIRE(swap.free == 0);
        }
    }

    GIVEN("a free value larger than the total (Windows commit-limit style)")
    {
        THEN("free is clamped to the total and used is 0")
        {
            const auto swap = swap_usage_from_free(100, 400);
            REQUIRE(swap.free == 100);
            REQUIRE(swap.used == 0);
        }
    }
}

SCENARIO("Memory usage percentage is bounded")
{
    GIVEN("a zero total")
    {
        THEN("usage is 0 percent")
        {
            REQUIRE_THAT(memory_usage_percent(0, 0), WithinAbs(0.0, 1e-9));
        }
    }
    GIVEN("half of the memory available")
    {
        THEN("usage is 50 percent")
        {
            REQUIRE_THAT(memory_usage_percent(1000, 500), WithinAbs(50.0, 1e-9));
        }
    }
    GIVEN("available larger than total")
    {
        THEN("usage is 0 percent rather than negative")
        {
            REQUIRE_THAT(memory_usage_percent(1000, 2000), WithinAbs(0.0, 1e-9));
        }
    }
}

SCENARIO("MemoryCollector reports plausible host memory")
{
    GIVEN("a fresh MemoryCollector")
    {
        MemoryCollector collector;
        SystemMetrics metrics;

        WHEN("the collector runs")
        {
            collector.update(metrics);
            const auto& m = metrics.memory;

            THEN("physical memory is non-zero and consistent")
            {
                REQUIRE(m.total_bytes > 0);
                REQUIRE(m.used_bytes <= m.total_bytes);
                REQUIRE(m.free_bytes <= m.total_bytes);
                REQUIRE(m.available_bytes <= m.total_bytes);
                REQUIRE(m.usage_percent >= 0.0);
                REQUIRE(m.usage_percent <= 100.0);
            }
            AND_THEN("swap never underflows")
            {
                REQUIRE(m.swap_used_bytes <= m.swap_total_bytes);
                REQUIRE(m.swap_free_bytes <= m.swap_total_bytes);
                REQUIRE(m.swap_used_bytes + m.swap_free_bytes == m.swap_total_bytes);
            }
        }
    }
}
