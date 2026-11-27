#pragma once

#include "SingleLog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace thewatcher::logging
{

// Log file settings shared by the server and agent configs. Defaults keep the
// log readable and bounded: INFO, 10 MiB per file and 5 rotated backups.
struct LogSettings
{
    static constexpr std::uint64_t bytes_per_mib = 1024ULL * 1024ULL;
    static constexpr std::uint64_t max_size_mib_limit = 1024ULL * 1024ULL; // 1 TiB, guards the multiplication

    LogLevel level = LogLevel::L_INFO;
    std::uint64_t max_size_mib = 10; // 0 disables rotation
    unsigned max_files = 5;          // rotated backups kept next to the live file

    // Parses a case-insensitive level name (trace, debug, info, notice,
    // warning, error, critical, off). Throws std::invalid_argument otherwise.
    static LogLevel parse_level(const std::string& text)
    {
        std::string name;
        for (const char ch : text)
            name += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (name == "trace")
            return LogLevel::L_TRACE;
        if (name == "debug")
            return LogLevel::L_DEBUG;
        if (name == "info")
            return LogLevel::L_INFO;
        if (name == "notice")
            return LogLevel::L_NOTICE;
        if (name == "warning" || name == "warn")
            return LogLevel::L_WARNING;
        if (name == "error")
            return LogLevel::L_ERROR;
        if (name == "critical")
            return LogLevel::L_CRITICAL;
        if (name == "off")
            return LogLevel::L_OFF;
        throw std::invalid_argument("Unknown log level: " + text);
    }

    // Lower-case name for a level, as accepted by parse_level.
    static const char* level_name(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::L_TRACE:
            return "trace";
        case LogLevel::L_DEBUG:
            return "debug";
        case LogLevel::L_INFO:
            return "info";
        case LogLevel::L_NOTICE:
            return "notice";
        case LogLevel::L_WARNING:
            return "warning";
        case LogLevel::L_ERROR:
            return "error";
        case LogLevel::L_CRITICAL:
            return "critical";
        case LogLevel::L_OFF:
            return "off";
        }
        return "info";
    }

    // Parses a non-negative whole number. Throws std::invalid_argument on
    // anything else, including trailing characters and negative values.
    static std::uint64_t parse_count(const std::string& text, const std::string& what)
    {
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](char ch) {
                return ch >= '0' && ch <= '9';
            }))
            throw std::invalid_argument(what + " must be a non-negative whole number: " + text);
        try
        {
            return std::stoull(text);
        }
        catch (const std::out_of_range&)
        {
            throw std::invalid_argument(what + " is too large: " + text);
        }
    }

    void set_level(const std::string& text)
    {
        level = parse_level(text);
    }

    void set_max_size_mib(const std::string& text)
    {
        const auto value = parse_count(text, "LOG_MAX_SIZE_MB");
        if (value > max_size_mib_limit)
            throw std::invalid_argument("LOG_MAX_SIZE_MB is too large: " + text);
        max_size_mib = value;
    }

    void set_max_files(const std::string& text)
    {
        const auto value = parse_count(text, "LOG_MAX_FILES");
        if (value > 1000)
            throw std::invalid_argument("LOG_MAX_FILES must be at most 1000: " + text);
        max_files = static_cast<unsigned>(value);
    }

    // Applies the file level and rotation to the logger. The caller still
    // sets the log file path.
    void apply(SingleLog& logger) const
    {
        logger.SetFileLogLevel(level);
        // The console stays at INFO unless the operator asked for less.
        logger.SetConsoleLogLevel(level > LogLevel::L_INFO ? level : LogLevel::L_INFO);
        logger.SetLogRotation(max_size_mib * bytes_per_mib, max_files);
    }
};

} // namespace thewatcher::logging
