#include "agent/collectors/cpu_collector.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <thread>

using namespace thewatcher;
using namespace thewatcher::agent;
using Catch::Matchers::WithinAbs;

SCENARIO("CPU usage is derived from tick counter deltas")
{
    GIVEN("two samples where half of the elapsed ticks were idle")
    {
        WHEN("the percentage is calculated")
        {
            const double percent = cpu_percent_from_delta(100, 200, 150, 300);

            THEN("usage is 50 percent")
            {
                REQUIRE_THAT(percent, WithinAbs(50.0, 1e-9));
            }
        }
    }

    GIVEN("two samples with no idle ticks")
    {
        THEN("usage is 100 percent")
        {
            REQUIRE_THAT(cpu_percent_from_delta(10, 100, 10, 200), WithinAbs(100.0, 1e-9));
        }
    }

    GIVEN("two samples that are all idle")
    {
        THEN("usage is 0 percent")
        {
            REQUIRE_THAT(cpu_percent_from_delta(0, 0, 100, 100), WithinAbs(0.0, 1e-9));
        }
    }

    GIVEN("identical samples (no ticks elapsed)")
    {
        THEN("usage is 0 percent and there is no division by zero")
        {
            REQUIRE_THAT(cpu_percent_from_delta(5, 10, 5, 10), WithinAbs(0.0, 1e-9));
        }
    }

    GIVEN("counters that went backwards (wrap or reset)")
    {
        THEN("usage is 0 percent instead of a huge unsigned delta")
        {
            REQUIRE_THAT(cpu_percent_from_delta(500, 1000, 100, 200), WithinAbs(0.0, 1e-9));
            REQUIRE_THAT(cpu_percent_from_delta(100, 1000, 50, 2000), WithinAbs(0.0, 1e-9));
        }
    }

    GIVEN("an idle delta larger than the total delta")
    {
        THEN("usage is clamped to the 0 to 100 range")
        {
            const double percent = cpu_percent_from_delta(0, 0, 200, 100);
            REQUIRE(percent >= 0.0);
            REQUIRE(percent <= 100.0);
        }
    }

    GIVEN("counters near the 64-bit limit")
    {
        constexpr auto big = std::numeric_limits<uint64_t>::max() - 10;
        THEN("the delta is still computed without overflow")
        {
            REQUIRE_THAT(cpu_percent_from_delta(big - 100, big - 50, big - 75, big), WithinAbs(50.0, 1e-9));
        }
    }
}

#if defined(__linux__) || defined(_WIN32) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

namespace
{

// Keeps one core busy until destroyed so CPU usage is measurably non-zero.
class BusyLoad
{
public:
    BusyLoad()
        : worker_([this] {
            volatile uint64_t sink = 0;
            while (running_.load())
            {
                sink = sink + 1;
            }
        })
    {
    }
    ~BusyLoad()
    {
        running_ = false;
        worker_.join();
    }
    BusyLoad(const BusyLoad&) = delete;
    BusyLoad& operator=(const BusyLoad&) = delete;

private:
    std::atomic<bool> running_{true};
    std::thread worker_;
};

} // namespace

SCENARIO("A freshly constructed CpuCollector reports a real CPU value")
{
    GIVEN("a busy host and a brand new collector")
    {
        BusyLoad load;
        CpuCollector collector;
        SystemMetrics metrics;

        WHEN("update is called for the first time")
        {
            collector.update(metrics);

            THEN("usage is non-zero and within 0 to 100")
            {
                REQUIRE(metrics.cpu.usage_percent > 0.0);
                REQUIRE(metrics.cpu.usage_percent <= 100.0);
            }
            AND_THEN("the logical core count is reported")
            {
                REQUIRE(metrics.cpu.num_logical_cores > 0);
            }
        }
    }

    GIVEN("a collector that was replaced by a new one (restart)")
    {
        BusyLoad load;
        SystemMetrics metrics;
        {
            CpuCollector old_collector;
            old_collector.update(metrics);
        }

        WHEN("the replacement collector updates once")
        {
            CpuCollector collector;
            SystemMetrics fresh;
            collector.update(fresh);

            THEN("it does not report 0 percent")
            {
                REQUIRE(fresh.cpu.usage_percent > 0.0);
            }
        }
    }
}

SCENARIO("CpuCollector output stays plausible across repeated updates")
{
    GIVEN("a collector updated back to back")
    {
        CpuCollector collector;
        SystemMetrics metrics;

        WHEN("update is called several times in quick succession")
        {
            for (int i = 0; i < 3; ++i)
            {
                collector.update(metrics);
            }

            THEN("usage and per-core values are within 0 to 100")
            {
                REQUIRE(metrics.cpu.usage_percent >= 0.0);
                REQUIRE(metrics.cpu.usage_percent <= 100.0);
                for (const double core : metrics.cpu.per_core_usage)
                {
                    REQUIRE(core >= 0.0);
                    REQUIRE(core <= 100.0);
                }
            }
            AND_THEN("load averages are not negative")
            {
                REQUIRE(metrics.cpu.load_avg_1m >= 0.0);
                REQUIRE(metrics.cpu.load_avg_5m >= 0.0);
                REQUIRE(metrics.cpu.load_avg_15m >= 0.0);
            }
        }
    }
}

#endif
