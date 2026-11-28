#pragma once

#include "common/SingleLog.hpp"

#include <cstddef>
#include <string>

namespace thewatcher::server
{

// Parses a stored integer setting. A value that is not a whole number (or has
// trailing text, or does not fit an int) logs a warning and yields `fallback`,
// so one bad row in the settings table cannot stop the monitoring loop.
inline int parse_int_setting(const std::string& value, int fallback, const char* name)
{
    try
    {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed == value.size())
            return parsed;
    }
    catch (const std::exception&)
    {
        // Fall through to the warning below.
    }
    LOGF_WARNING("Ignoring invalid integer setting %s='%s'; using %d", name, value.c_str(), fallback);
    return fallback;
}

} // namespace thewatcher::server
