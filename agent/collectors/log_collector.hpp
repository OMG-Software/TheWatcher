#pragma once

#include "collector.hpp"
#include "common/collector_config.hpp"
#include "common/protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace thewatcher::agent
{

// Parses LOG_ALLOWED_DIRS (path-list separator: ';' on Windows, ':'
// elsewhere) into resolved allowlist directories. Returns an empty list
// when unset — which disables log monitoring entirely (fail closed).
std::vector<std::filesystem::path> parse_log_allowed_dirs_from_env();

class LogCollector final : public Collector
{
public:
    std::string_view name() const noexcept override
    {
        return "log";
    }

    // update() satisfies the Collector interface but does nothing here;
    // log tailing happens in tick() which is called explicitly by the agent
    // after collect_metrics() so matches can be sent as separate LOG_MATCH frames.
    void update(SystemMetrics& /*metrics*/) override
    {
    }

    void set_configs(std::vector<LogMonitorConfig> configs);

    // SEC-10: agent-local allowlist of directories the log monitor may read.
    // Wired from the LOG_ALLOWED_DIRS environment variable by the Agent —
    // the server has no way to change it. While empty (the default) every
    // configured path is denied: fail closed.
    void set_allowed_dirs(std::vector<std::filesystem::path> dirs);

    // Poll each configured file for new lines, apply patterns, accumulate matches.
    void tick();

    // Drain and return all accumulated matches since the last call.
    std::vector<proto::LogMatch> take_matches();

private:
    struct FileState
    {
        int64_t inode = -1; // -1 = file not yet seen
        int64_t offset = 0;
    };

    std::vector<LogMonitorConfig> configs_;
    std::vector<std::filesystem::path> allowed_dirs_;
    std::unordered_map<std::string, FileState> file_states_;
    std::vector<proto::LogMatch> pending_;
    std::mutex mutex_;

    void tail_file(const LogMonitorConfig& cfg);
};

} // namespace thewatcher::agent