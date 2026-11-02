#include "log_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>

#include <sys/stat.h>

namespace thewatcher::agent
{

namespace
{

    // SEC-10 hard bounds. The server supplies path + pattern; without these
    // caps a hostile or careless configuration could make the agent read
    // arbitrary amounts of data (or burn CPU in catastrophic backtracking)
    // inside its collection thread.
    constexpr std::size_t max_pattern_len = 256;
    constexpr std::size_t max_line_len = 64 * 1024;
    constexpr std::size_t max_matches_per_tick = 100;
    constexpr int64_t max_bytes_per_tick = 1024 * 1024;

    // True when path (already resolved) lies under dir, comparing whole
    // path components so "/allowed2" does not count as under "/allowed".
    bool is_under(const std::filesystem::path& path, const std::filesystem::path& dir)
    {
        auto p = path.begin();
        for (auto d = dir.begin(); d != dir.end(); ++d, ++p)
        {
            if (p == path.end() || *p != *d)
                return false;
        }
        return true;
    }

} // namespace

// Parses LOG_ALLOWED_DIRS (platform path-list separator: ';' on Windows,
// ':' elsewhere) into the allowlist handed to LogCollector.
std::vector<std::filesystem::path> parse_log_allowed_dirs_from_env()
{
#ifdef _WIN32
    constexpr char separator = ';';
#else
    constexpr char separator = ':';
#endif
    std::vector<std::filesystem::path> dirs;

    std::string value;
#ifdef _WIN32
    {
        // MSVC deprecates getenv (C4996, fatal under werror); _dupenv_s is
        // the documented replacement. size includes the NUL terminator.
        char* buffer = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&buffer, &size, "LOG_ALLOWED_DIRS") == 0 && buffer != nullptr)
        {
            value.assign(buffer, size > 0 ? size - 1 : 0);
            std::free(buffer);
        }
    }
#else
    if (const char* raw = std::getenv("LOG_ALLOWED_DIRS"); raw != nullptr)
        value = raw;
#endif
    if (value.empty())
        return dirs;

    std::size_t start = 0;
    while (start <= value.size())
    {
        const auto end = value.find(separator, start);
        const auto entry = value.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!entry.empty())
        {
            std::error_code ec;
            auto resolved = std::filesystem::weakly_canonical(entry, ec);
            if (!ec)
                dirs.push_back(std::move(resolved));
        }
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return dirs;
}

void LogCollector::set_configs(std::vector<LogMonitorConfig> configs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    configs_ = std::move(configs);
    // Remove file states for paths that are no longer configured.
    for (auto it = file_states_.begin(); it != file_states_.end();)
    {
        bool found = false;
        for (const auto& cfg : configs_)
        {
            if (cfg.path == it->first)
            {
                found = true;
                break;
            }
        }
        it = found ? std::next(it) : file_states_.erase(it);
    }
}

void LogCollector::set_allowed_dirs(std::vector<std::filesystem::path> dirs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    allowed_dirs_ = std::move(dirs);
    LOGF_DEBUG("LogCollector allowlist updated entry_count=%zu", allowed_dirs_.size());
}

void LogCollector::tick()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // SEC-10: with no allowlist configured the log monitor is disabled
    // outright — the monitored contents land on the dashboard, so an empty
    // allowlist must not fall back to "read anything".
    if (allowed_dirs_.empty() && !configs_.empty())
    {
        LOG_WARNING("Log monitoring disabled: LOG_ALLOWED_DIRS is not configured");
        return;
    }
    for (const auto& cfg : configs_)
    {
        if (!cfg.enabled || cfg.path.empty() || cfg.pattern.empty() || cfg.indicator_name.empty())
            continue;
        if (cfg.pattern.size() > max_pattern_len)
        {
            LOGF_WARNING("LogCollector pattern too long path=%s pattern_len=%zu", cfg.path.c_str(), cfg.pattern.size());
            continue;
        }
        try
        {
            tail_file(cfg);
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("LogCollector tail_file path=%s error=%s", cfg.path.c_str(), e.what());
        }
    }
}

std::vector<proto::LogMatch> LogCollector::take_matches()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(pending_, {});
}

void LogCollector::tail_file(const LogMonitorConfig& cfg)
{
    // SEC-10: resolve symlinks and require the real location to sit inside
    // a locally allowlisted directory. The server cannot extend this list.
    std::error_code resolve_ec;
    const auto resolved = std::filesystem::weakly_canonical(cfg.path, resolve_ec);
    if (resolve_ec)
    {
        file_states_.erase(cfg.path);
        return;
    }
    bool allowed = false;
    for (const auto& dir : allowed_dirs_)
    {
        if (is_under(resolved, dir))
        {
            allowed = true;
            break;
        }
    }
    if (!allowed)
    {
        LOGF_WARNING("LogCollector path outside LOG_ALLOWED_DIRS path=%s", cfg.path.c_str());
        file_states_.erase(cfg.path);
        return;
    }

    struct stat st{};
    if (::stat(cfg.path.c_str(), &st) != 0)
    {
        if (errno != ENOENT)
        {
            const auto stat_error = errno;
            const auto message = std::system_category().message(stat_error);
            LOGF_WARNING("LogCollector stat path=%s error=%s", cfg.path.c_str(), message.c_str());
        }
        // File doesn't exist yet; reset state so we start from byte 0 when it appears.
        file_states_.erase(cfg.path);
        return;
    }

    // SEC-10: only regular files — directories, devices and fifos are
    // rejected outright (symlinks were resolved above, so symlink_status
    // here sees the original link and refuses it).
    std::error_code status_ec;
    const auto link_status = std::filesystem::symlink_status(cfg.path, status_ec);
    if (status_ec || !std::filesystem::is_regular_file(link_status))
    {
        LOGF_WARNING("LogCollector path is not a regular file path=%s", cfg.path.c_str());
        file_states_.erase(cfg.path);
        return;
    }

    const auto inode = static_cast<int64_t>(st.st_ino);
    auto& state = file_states_[cfg.path];

    // Detect rotation: inode changed or file is shorter than our saved offset.
    if (state.inode != inode || static_cast<int64_t>(st.st_size) < state.offset)
    {
        LOGF_DEBUG("LogCollector rotation detected path=%s", cfg.path.c_str());
        state.inode = inode;
        state.offset = 0;
    }

    if (state.offset >= static_cast<int64_t>(st.st_size))
        return; // nothing new

    std::ifstream file(cfg.path, std::ios::binary);
    if (!file.is_open())
    {
        LOGF_WARNING("LogCollector open failed path=%s", cfg.path.c_str());
        return;
    }

    file.seekg(state.offset);

    std::regex pattern;
    try
    {
        pattern = std::regex(cfg.pattern, std::regex::ECMAScript | std::regex::optimize);
    }
    catch (const std::regex_error& e)
    {
        LOGF_WARNING("LogCollector invalid regex pattern=%s error=%s", cfg.pattern.c_str(), e.what());
        return;
    }

    // SEC-10: bounded, byte-at-a-time reading with hard caps on bytes and
    // matches per tick. std::getline is unbounded — a single huge "line"
    // could previously consume unbounded memory and time. Lines longer than
    // max_line_len are truncated; the skip loop below keeps the next tick
    // aligned to a line boundary.
    int64_t bytes_read = 0;
    std::size_t matches = 0;
    std::string line;
    while (bytes_read < max_bytes_per_tick && matches < max_matches_per_tick)
    {
        const int c = file.get();
        if (c == std::char_traits<char>::eof())
            break;
        ++bytes_read;

        if (c == '\n')
        {
            if (!line.empty() && std::regex_search(line, pattern))
            {
                proto::LogMatch m;
                m.indicator_name = cfg.indicator_name;
                m.path = cfg.path;
                m.matched_line = line;
                m.severity = cfg.severity;
                LOGF_DEBUG("LogCollector match indicator=%s path=%s line=%.80s", cfg.indicator_name.c_str(),
                           cfg.path.c_str(), line.c_str());
                pending_.push_back(std::move(m));
                ++matches;
            }
            line.clear();
        }
        else if (line.size() < max_line_len)
        {
            line.push_back(static_cast<char>(c));
        }
        // Characters beyond max_line_len are dropped until the next newline.
    }

    // If a cap stopped us mid-line, consume to the end of that line (bounded)
    // so the next tick starts on a clean boundary; monster lines are dropped.
    // An empty buffer means we are already at a line boundary — skipping
    // then would silently discard the next line.
    if (!line.empty() && !file.eof())
    {
        int64_t skipped = 0;
        while (skipped < max_bytes_per_tick)
        {
            const int c = file.get();
            if (c == std::char_traits<char>::eof())
                break;
            ++skipped;
            if (c == '\n')
                break;
        }
    }

    // After reading reaches EOF the stream sets eofbit, causing tellg() to
    // return -1. Clear error state so we get the real file position; fall
    // back to st_size if needed.
    file.clear();
    const auto pos = file.tellg();
    state.offset = (pos >= 0) ? static_cast<int64_t>(pos) : static_cast<int64_t>(st.st_size);
}

} // namespace thewatcher::agent