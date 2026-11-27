// The agent's collectors, owned by the collection thread alone (issue #87).
// Typed members replace the old raw log-collector pointer and dynamic_cast.
#pragma once

#include "collectors/cpu_collector.hpp"
#include "collectors/disk_collector.hpp"
#include "collectors/log_collector.hpp"
#include "collectors/memory_collector.hpp"
#include "collectors/network_collector.hpp"
#include "collectors/process_collector.hpp"
#include "common/collector_config.hpp"

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <utility>
#include <vector>

namespace thewatcher::agent
{

class CollectorSet
{
public:
    // `log_allowed_dirs` is the SEC-10 allowlist; an empty list keeps log
    // monitoring fail-closed.
    CollectorSet(int process_limit, std::vector<std::filesystem::path> log_allowed_dirs)
        : process_(process_limit)
    {
        log_.set_allowed_dirs(std::move(log_allowed_dirs));
    }

    CollectorSet(const CollectorSet&) = delete;
    CollectorSet& operator=(const CollectorSet&) = delete;

    ProcessCollector& process()
    {
        return process_;
    }
    LogCollector& log()
    {
        return log_;
    }

    // Applies the server-pushed process watches and log monitors.
    void apply(const CollectorConfig& config)
    {
        process_.set_watches(config.processes);
        log_.set_configs(config.logs);
    }

    // Visits every collector in collection order.
    template <typename Fn>
    void for_each(Fn&& fn)
    {
        for (Collector& c :
             std::initializer_list<std::reference_wrapper<Collector>>{cpu_, memory_, disk_, process_, network_, log_})
            fn(c);
    }

private:
    CpuCollector cpu_;
    MemoryCollector memory_;
    DiskCollector disk_;
    ProcessCollector process_;
    NetworkCollector network_;
    LogCollector log_;
};

} // namespace thewatcher::agent
