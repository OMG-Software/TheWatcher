// Valid ranges for the agent runtime settings, shared by the agent (config
// file, CONFIG_UPDATE, commands) and the server (REST API) so both sides
// reject the same values. A zero or negative interval makes the agent loops
// spin, and a non-positive process limit makes the process collector throw on
// every pass (issue #88).
#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace thewatcher
{

struct SettingRange
{
    int min;
    int max;
};

inline constexpr SettingRange kCollectionIntervalRange{1, 3600};
inline constexpr SettingRange kHeartbeatIntervalRange{1, 60};
inline constexpr SettingRange kProcessLimitRange{1, 1000};

inline bool in_range(std::int64_t value, SettingRange range)
{
    return value >= range.min && value <= range.max;
}

// "NAME must be between MIN and MAX".
inline std::string range_message(std::string_view name, SettingRange range)
{
    return std::string(name) + " must be between " + std::to_string(range.min) + " and " + std::to_string(range.max);
}

// Returns an error message when `value` is outside `range`, nullopt when valid.
inline std::optional<std::string> check_setting(std::string_view name, std::int64_t value, SettingRange range)
{
    if (in_range(value, range))
        return std::nullopt;
    return range_message(name, range);
}

// Strictly parses a whole-string integer and range-checks it. Throws
// std::invalid_argument with a message naming `key` (never a bare "stoi").
inline int parse_int_setting(std::string_view key, std::string_view text, SettingRange range)
{
    std::int64_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (text.empty() || result.ec == std::errc::invalid_argument || result.ptr != last)
        throw std::invalid_argument(std::string(key) + " must be an integer");
    if (result.ec == std::errc::result_out_of_range || !in_range(value, range))
        throw std::invalid_argument(range_message(key, range));
    return static_cast<int>(value);
}

} // namespace thewatcher
