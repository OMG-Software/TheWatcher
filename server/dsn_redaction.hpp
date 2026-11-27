#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace thewatcher::server
{

// Returns `dsn` with every password value replaced by "***", so the result is
// safe to write to a log. Handles libpq keyword/value strings (plain, quoted
// with backslash escapes, optional spaces around '=') and URIs (user info and
// the "password" query parameter). Header-only so the SQLite-only build can
// use it too.
inline std::string redact_dsn(const std::string& dsn)
{
    constexpr const char* kMask = "***";
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };

    const bool is_uri = std::string_view(dsn).starts_with("postgresql://") || std::string_view(dsn).starts_with("postgres://");
    if (is_uri)
    {
        std::string out = dsn;
        const auto authority_start = out.find("://") + 3;
        const auto authority_end = out.find_first_of("/?", authority_start);
        const auto at = out.rfind('@', authority_end == std::string::npos ? std::string::npos : authority_end);
        if (at != std::string::npos && at >= authority_start)
        {
            const auto colon = out.find(':', authority_start);
            if (colon != std::string::npos && colon < at)
                out.replace(colon + 1, at - colon - 1, kMask);
        }
        // "?password=..." or "&password=..." query parameters.
        for (std::size_t pos = out.find('?'); pos != std::string::npos;)
        {
            const auto key = pos + 1;
            if (out.compare(key, 9, "password=") == 0)
            {
                const auto end = out.find('&', key + 9);
                out.replace(key + 9, end == std::string::npos ? std::string::npos : end - key - 9, kMask);
            }
            pos = out.find('&', key);
        }
        return out;
    }

    std::string out;
    std::size_t i = 0;
    while (i < dsn.size())
    {
        // A keyword starts the string or follows whitespace.
        const bool token_start = i == 0 || is_space(dsn[i - 1]);
        if (token_start && dsn.compare(i, 8, "password") == 0)
        {
            std::size_t j = i + 8;
            while (j < dsn.size() && is_space(dsn[j]))
                ++j;
            if (j < dsn.size() && dsn[j] == '=')
            {
                ++j;
                while (j < dsn.size() && is_space(dsn[j]))
                    ++j;
                if (j < dsn.size() && dsn[j] == '\'')
                {
                    ++j; // skip to the closing quote, honouring backslash escapes
                    while (j < dsn.size() && dsn[j] != '\'')
                        j += dsn[j] == '\\' ? 2 : 1;
                    j = std::min(j + 1, dsn.size());
                }
                else
                {
                    while (j < dsn.size() && !is_space(dsn[j]))
                        ++j;
                }
                out += "password=";
                out += kMask;
                i = j;
                continue;
            }
        }
        out += dsn[i++];
    }
    return out;
}

} // namespace thewatcher::server
