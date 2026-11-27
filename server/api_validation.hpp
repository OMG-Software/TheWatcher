#pragma once

// Input validation shared by the REST API handlers: the size limits the API
// enforces and strict parsers for ids and numeric query values. Header-only
// and free of httplib/JSON so it can be unit-tested on its own.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace thewatcher::server::validation
{

// Thrown for input the client got wrong (answered 400 with its message), as
// opposed to a storage or runtime fault (answered 500 with a generic message).
class ValidationError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// Maximum sizes, in bytes for text. Every handler uses these so the same kind
// of value is limited the same way on every endpoint.
namespace limits
{
    inline constexpr std::size_t agent_id_bytes = 128;
    inline constexpr std::size_t name_bytes = 64; // group, user and view names, indicator names
    inline constexpr std::size_t username_bytes = 64;
    inline constexpr std::size_t password_bytes = 256;
    inline constexpr std::size_t reason_bytes = 1024; // maintenance, silence reasons
    inline constexpr std::size_t note_bytes = 4096;   // alert notes, runbook notes, agent description
    inline constexpr std::size_t url_bytes = 2048;
    inline constexpr std::size_t markdown_bytes = 65536; // agent runbook text

    inline constexpr std::size_t max_ids_per_request = 1000; // bulk alert ids, view agents, group ids
    inline constexpr std::size_t max_group_ids = 256;
    inline constexpr std::size_t max_collector_entries = 64; // disks, networks, processes, logs

    inline constexpr int min_interval_seconds = 1; // collection interval
    inline constexpr int max_interval_seconds = 86400;
    inline constexpr int min_heartbeat_seconds = 1;
    inline constexpr int max_heartbeat_seconds = 60;
    inline constexpr int min_process_limit = 1;
    inline constexpr int max_process_limit = 1000;
    inline constexpr int max_readings = 1000;            // *_readings in a collector config
    inline constexpr int max_expected_processes = 10000; // expected_count of a watched process

    // Unix epoch milliseconds are accepted up to the year 2100: this rejects
    // values that are obviously seconds-by-mistake overflows or garbage.
    inline constexpr int64_t max_timestamp_ms = 4'102'444'800'000;

    // Paging for list endpoints.
    inline constexpr int default_page_size = 1000;
    inline constexpr int max_page_size = 1000;
    inline constexpr int default_history_limit = 100;
    inline constexpr int max_metrics_limit = 1000;
    inline constexpr int max_history_limit = 200;
    inline constexpr int max_log_match_limit = 1000;
    inline constexpr int max_uptime_days = 90;
} // namespace limits

// Parses a database id taken from a URL path or a JSON body: plain decimal
// digits only (no sign, spaces, "0x", or trailing text), greater than zero and
// within int64. Returns nullopt for anything else, so callers answer 400
// instead of letting std::stoll throw.
inline std::optional<int64_t> parse_id(std::string_view text)
{
    if (text.empty() || text.size() > 19)
        return std::nullopt;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
    }
    int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value <= 0)
        return std::nullopt;
    return value;
}

// Parses an optionally signed decimal integer in [min, max], for query
// parameters such as ?limit=. Returns nullopt when it is not a number or is
// out of range (callers answer 400 rather than silently clamping bad input).
inline std::optional<int> parse_int(std::string_view text, int min, int max)
{
    if (text.empty())
        return std::nullopt;
    int value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || value < min || value > max)
        return std::nullopt;
    return value;
}

// Returns an error message when value is longer than max_bytes (or empty while
// required), otherwise nullopt. field names the JSON field in the message.
inline std::optional<std::string> check_text(std::string_view field, std::string_view value, std::size_t max_bytes,
                                             bool required = false)
{
    if (required && value.empty())
        return std::string(field) + " is required";
    if (value.size() > max_bytes)
        return std::string(field) + " must be " + std::to_string(max_bytes) + " bytes or fewer";
    return std::nullopt;
}

// Returns an error message when value is outside [min, max], otherwise nullopt.
inline std::optional<std::string> check_range(std::string_view field, int64_t value, int64_t min, int64_t max)
{
    if (value < min || value > max)
        return std::string(field) + " must be between " + std::to_string(min) + " and " + std::to_string(max);
    return std::nullopt;
}

// Returns an error message when count is above max_count, otherwise nullopt.
inline std::optional<std::string> check_count(std::string_view field, std::size_t count, std::size_t max_count)
{
    if (count > max_count)
        return std::string(field) + " must have " + std::to_string(max_count) + " entries or fewer";
    return std::nullopt;
}

} // namespace thewatcher::server::validation
