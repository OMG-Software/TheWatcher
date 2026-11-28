// DSN helpers for the PostgreSQL tests. Header-only and free of libpq, so the
// builders can be unit-tested on machines without a PostgreSQL client.
#pragma once

#include <cctype>
#include <string>
#include <string_view>

namespace thewatcher::server::test
{

namespace dsn_detail
{
    inline bool is_uri(const std::string& dsn)
    {
        const std::string_view view(dsn);
        return view.starts_with("postgresql://") || view.starts_with("postgres://");
    }

    inline std::string percent_encode(const std::string& value)
    {
        static constexpr char HEX[] = "0123456789ABCDEF";
        std::string out;
        for (const unsigned char c : value)
        {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            {
                out.push_back(static_cast<char>(c));
            }
            else
            {
                out.push_back('%');
                out.push_back(HEX[c >> 4]);
                out.push_back(HEX[c & 0xF]);
            }
        }
        return out;
    }

    // Single-quotes a keyword/value item when it holds whitespace, quotes or
    // backslashes, escaping the latter two.
    inline std::string quote_keyword_value(const std::string& value)
    {
        if (!value.empty() && value.find_first_of(" \t'\\") == std::string::npos)
            return value;
        std::string out = "'";
        for (const char c : value)
        {
            if (c == '\'' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }
} // namespace dsn_detail

// Returns `dsn` with `key=value` added, in the DSN's own syntax: a keyword/
// value string gets a (quoted if needed) keyword, a postgresql:// URI gets a
// URL-encoded query parameter and keeps any existing query. Mixing the two
// forms is not a valid libpq connection string.
inline std::string dsn_with_param(std::string dsn, const std::string& key, const std::string& value)
{
    while (!dsn.empty() && dsn.back() == ' ')
        dsn.pop_back();
    if (!dsn_detail::is_uri(dsn))
        return dsn + " " + key + "=" + dsn_detail::quote_keyword_value(value);
    const char separator = dsn.find('?') == std::string::npos ? '?' : '&';
    return dsn + separator + key + "=" + dsn_detail::percent_encode(value);
}

// Pins the connection to `schema` through the search_path startup option.
inline std::string dsn_with_search_path(const std::string& dsn, const std::string& schema)
{
    return dsn_with_param(dsn, "options", "-csearch_path=" + schema);
}

} // namespace thewatcher::server::test
