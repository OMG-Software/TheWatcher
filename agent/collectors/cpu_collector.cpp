#include "cpu_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <array>
#include <thread>
#include <utility>

#ifdef __linux__
#include <fstream>
#include <sstream>
#include <string>
#elif defined(_WIN32)
#include <windows.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#if defined(__FreeBSD__)
#include <sys/resource.h> // CPUSTATES, CP_IDLE
#else
#include <sys/sched.h> // CPUSTATES, CP_IDLE
#endif
#include <cstdlib> // getloadavg
#endif

namespace thewatcher::agent
{

namespace
{

    // Two samples closer together than this give a delta of a few ticks, which is mostly noise.
    constexpr std::chrono::milliseconds min_sample_interval{100};

#ifdef __linux__

    void read_load_average(CpuMetrics& cpu)
    {
        std::ifstream loadavg("/proc/loadavg");
        loadavg >> cpu.load_avg_1m >> cpu.load_avg_5m >> cpu.load_avg_15m;
    }

#endif

} // namespace

double cpu_percent_from_delta(uint64_t previous_idle, uint64_t previous_total, uint64_t current_idle,
                              uint64_t current_total) noexcept
{
    if (current_total <= previous_total || current_idle < previous_idle)
    {
        return 0.0;
    }

    const auto total_delta = static_cast<double>(current_total - previous_total);
    const auto idle_delta = static_cast<double>(current_idle - previous_idle);
    return std::clamp(100.0 * (total_delta - idle_delta) / total_delta, 0.0, 100.0);
}

#ifdef __linux__

CpuCollector::Sample CpuCollector::read_sample()
{
    CpuCollector::Sample sample;
    sample.sampled_at = std::chrono::steady_clock::now();

    std::ifstream stat("/proc/stat");
    std::string line;
    while (std::getline(stat, line))
    {
        if (line.rfind("cpu", 0) != 0)
        {
            break;
        }

        std::istringstream row(line);
        std::string label;
        uint64_t user = 0;
        uint64_t nice = 0;
        uint64_t system = 0;
        uint64_t idle = 0;
        uint64_t iowait = 0;
        uint64_t irq = 0;
        uint64_t softirq = 0;
        uint64_t steal = 0;
        row >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

        const uint64_t idle_all = idle + iowait;
        const uint64_t total = user + nice + system + idle + iowait + irq + softirq + steal;
        if (label == "cpu")
        {
            sample.idle = idle_all;
            sample.total = total;
        }
        else
        {
            sample.core_idle.push_back(idle_all);
            sample.core_total.push_back(total);
        }
    }

    return sample;
}

#elif defined(_WIN32)

namespace
{

    uint64_t filetime_to_u64(const FILETIME& ft)
    {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

} // namespace

CpuCollector::Sample CpuCollector::read_sample()
{
    CpuCollector::Sample sample;
    sample.sampled_at = std::chrono::steady_clock::now();

    FILETIME idle_time{};
    FILETIME kernel_time{};
    FILETIME user_time{};
    if (::GetSystemTimes(&idle_time, &kernel_time, &user_time))
    {
        // Kernel time already includes idle time.
        sample.idle = filetime_to_u64(idle_time);
        sample.total = filetime_to_u64(kernel_time) + filetime_to_u64(user_time);
    }

    return sample;
}

#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)

namespace
{

    // Raw per-state tick counters: user, nice, system, interrupt, idle (OpenBSD also has spin).
    // The element type differs per BSD, so it is a template parameter.
    template <typename Tick>
    struct CpuTicks
    {
        std::array<Tick, CPUSTATES> state{};

        uint64_t idle() const
        {
            return static_cast<uint64_t>(state[CP_IDLE]);
        }
        uint64_t total() const
        {
            uint64_t sum = 0;
            for (const Tick value : state)
            {
                sum += static_cast<uint64_t>(value);
            }
            return sum;
        }
    };

    template <typename Tick>
    bool read_ticks_mib(const int* mib, u_int mib_len, CpuTicks<Tick>& out)
    {
        size_t len = sizeof(out.state);
        return ::sysctl(mib, mib_len, out.state.data(), &len, nullptr, 0) == 0 && len == sizeof(out.state);
    }

#if !defined(__FreeBSD__)
    int logical_cpu_count()
    {
        int mib[2] = {CTL_HW, HW_NCPU};
        int ncpu = 0;
        size_t len = sizeof(ncpu);
        if (::sysctl(mib, 2, &ncpu, &len, nullptr, 0) != 0 || ncpu < 1)
        {
            return 0;
        }
        return ncpu;
    }
#endif

#if defined(__FreeBSD__)

    // kern.cp_time: long[CPUSTATES] system total; kern.cp_times: long[CPUSTATES * ncpu] per CPU.
    void fill_sample(CpuCollector::Sample& sample)
    {
        CpuTicks<long> total;
        size_t total_len = sizeof(total.state);
        if (::sysctlbyname("kern.cp_time", total.state.data(), &total_len, nullptr, 0) != 0 ||
            total_len != sizeof(total.state))
        {
            return;
        }
        sample.idle = total.idle();
        sample.total = total.total();

        size_t len = 0;
        if (::sysctlbyname("kern.cp_times", nullptr, &len, nullptr, 0) != 0 || len == 0)
        {
            return;
        }
        std::vector<long> ticks(len / sizeof(long));
        if (::sysctlbyname("kern.cp_times", ticks.data(), &len, nullptr, 0) != 0)
        {
            return;
        }
        for (std::size_t cpu = 0; (cpu + 1) * CPUSTATES <= len / sizeof(long); ++cpu)
        {
            uint64_t core_total = 0;
            for (std::size_t state = 0; state < CPUSTATES; ++state)
            {
                core_total += static_cast<uint64_t>(ticks[cpu * CPUSTATES + state]);
            }
            sample.core_idle.push_back(static_cast<uint64_t>(ticks[cpu * CPUSTATES + CP_IDLE]));
            sample.core_total.push_back(core_total);
        }
    }

#elif defined(__OpenBSD__)

    // KERN_CPTIME: long[CPUSTATES] system total; KERN_CPTIME2 + cpu number: u_int64_t[CPUSTATES].
    void fill_sample(CpuCollector::Sample& sample)
    {
        CpuTicks<long> total;
        const int mib[2] = {CTL_KERN, KERN_CPTIME};
        if (!read_ticks_mib(mib, 2, total))
        {
            return;
        }
        sample.idle = total.idle();
        sample.total = total.total();

        // Per-core data is all or nothing: offline CPUs would otherwise shift the core indexes.
        const int ncpu = logical_cpu_count();
        for (int cpu = 0; cpu < ncpu; ++cpu)
        {
            CpuTicks<uint64_t> core;
            const int core_mib[3] = {CTL_KERN, KERN_CPTIME2, cpu};
            if (!read_ticks_mib(core_mib, 3, core))
            {
                sample.core_idle.clear();
                sample.core_total.clear();
                return;
            }
            sample.core_idle.push_back(core.idle());
            sample.core_total.push_back(core.total());
        }
    }

#else // __NetBSD__

    // kern.cp_time: uint64_t[CPUSTATES] summed over CPUs; appending a CPU number selects one CPU.
    void fill_sample(CpuCollector::Sample& sample)
    {
        CpuTicks<uint64_t> total;
        const int mib[2] = {CTL_KERN, KERN_CP_TIME};
        if (!read_ticks_mib(mib, 2, total))
        {
            return;
        }
        sample.idle = total.idle();
        sample.total = total.total();

        const int ncpu = logical_cpu_count();
        for (int cpu = 0; cpu < ncpu; ++cpu)
        {
            CpuTicks<uint64_t> core;
            const int core_mib[3] = {CTL_KERN, KERN_CP_TIME, cpu};
            if (!read_ticks_mib(core_mib, 3, core))
            {
                sample.core_idle.clear();
                sample.core_total.clear();
                return;
            }
            sample.core_idle.push_back(core.idle());
            sample.core_total.push_back(core.total());
        }
    }

#endif

} // namespace

CpuCollector::Sample CpuCollector::read_sample()
{
    CpuCollector::Sample sample;
    sample.sampled_at = std::chrono::steady_clock::now();
    fill_sample(sample);
    return sample;
}

#else

CpuCollector::Sample CpuCollector::read_sample()
{
    return {};
}

#endif

CpuCollector::CpuCollector()
{
    prev_ = read_sample();
    primed_ = prev_.total != 0;
}

void CpuCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    auto& cpu = metrics.cpu;
    cpu.num_logical_cores = static_cast<int>(std::thread::hardware_concurrency());

    if (!primed_)
    {
        // The constructor could not read the counters; try again now.
        prev_ = read_sample();
        primed_ = prev_.total != 0;
        if (!primed_)
        {
            LOG_DEBUG("CPU collector could not read a valid CPU sample");
            return;
        }
    }

    if (!have_result_)
    {
        // First real value: make sure the baseline is old enough for a meaningful delta.
        const auto age = std::chrono::steady_clock::now() - prev_.sampled_at;
        if (age < min_sample_interval)
        {
            std::this_thread::sleep_for(min_sample_interval - age);
        }
    }

    auto sample = read_sample();
    if (sample.total == 0)
    {
        LOG_DEBUG("CPU collector could not read a valid CPU sample");
        return;
    }

    if (sample.sampled_at - prev_.sampled_at >= min_sample_interval)
    {
        last_usage_ = cpu_percent_from_delta(prev_.idle, prev_.total, sample.idle, sample.total);

        const auto count = std::min(prev_.core_total.size(), sample.core_total.size());
        last_per_core_.clear();
        last_per_core_.reserve(count);
        for (std::size_t index = 0; index < count; ++index)
        {
            last_per_core_.push_back(cpu_percent_from_delta(prev_.core_idle[index], prev_.core_total[index],
                                                            sample.core_idle[index], sample.core_total[index]));
        }
        have_result_ = true;
        prev_ = std::move(sample);
    }
    // Otherwise the previous result is reported again: a delta of a few ticks would be noise.

    cpu.usage_percent = last_usage_;
    cpu.per_core_usage = last_per_core_;

#ifdef __linux__
    read_load_average(cpu);
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    std::array<double, 3> load{};
    if (::getloadavg(load.data(), 3) == 3)
    {
        cpu.load_avg_1m = load[0];
        cpu.load_avg_5m = load[1];
        cpu.load_avg_15m = load[2];
    }
#endif

    LOGF_TRACE("CPU collector updated usage=%.2f cores=%d per_core_samples=%zu", cpu.usage_percent,
               cpu.num_logical_cores, cpu.per_core_usage.size());
}

} // namespace thewatcher::agent
