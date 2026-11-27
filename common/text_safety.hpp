#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Helpers for text that comes from the network (agent-reported hostnames,
// mount points, log lines, ...). Peers are untrusted: their strings may carry
// control characters (log forging, issue #157) or invalid UTF-8 (which makes
// nlohmann::json::dump() throw and PostgreSQL reject the row, issue #56).
namespace thewatcher::text
{

inline constexpr char32_t kReplacementCharacter = 0xFFFD;

namespace detail
{

    // Appends code_point to out as UTF-8.
    inline void append_utf8(std::string& out, char32_t code_point)
    {
        if (code_point < 0x80)
        {
            out.push_back(static_cast<char>(code_point));
        }
        else if (code_point < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        else if (code_point < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
        }
    }

    inline bool is_continuation(std::string_view s, std::size_t index, unsigned char low = 0x80,
                                unsigned char high = 0xBF)
    {
        if (index >= s.size())
            return false;
        const auto byte = static_cast<unsigned char>(s[index]);
        return byte >= low && byte <= high;
    }

} // namespace detail

// Length in bytes (1-4) of the well-formed UTF-8 sequence starting at s[pos],
// or 0 when the bytes there are not valid UTF-8. Follows RFC 3629: overlong
// forms, UTF-16 surrogates and code points above U+10FFFF are invalid, which
// is the same rule nlohmann::json applies.
inline std::size_t utf8_sequence_length(std::string_view s, std::size_t pos)
{
    if (pos >= s.size())
        return 0;
    const auto lead = static_cast<unsigned char>(s[pos]);
    if (lead < 0x80)
        return 1;
    if (lead >= 0xC2 && lead <= 0xDF)
        return detail::is_continuation(s, pos + 1) ? 2 : 0;
    if (lead >= 0xE0 && lead <= 0xEF)
    {
        // E0 excludes overlongs (second byte A0-BF), ED excludes surrogates (80-9F).
        const unsigned char low = lead == 0xE0 ? 0xA0 : 0x80;
        const unsigned char high = lead == 0xED ? 0x9F : 0xBF;
        return detail::is_continuation(s, pos + 1, low, high) && detail::is_continuation(s, pos + 2) ? 3 : 0;
    }
    if (lead >= 0xF0 && lead <= 0xF4)
    {
        // F0 excludes overlongs (90-BF), F4 excludes code points above U+10FFFF (80-8F).
        const unsigned char low = lead == 0xF0 ? 0x90 : 0x80;
        const unsigned char high = lead == 0xF4 ? 0x8F : 0xBF;
        return detail::is_continuation(s, pos + 1, low, high) && detail::is_continuation(s, pos + 2) &&
                       detail::is_continuation(s, pos + 3)
                   ? 4
                   : 0;
    }
    return 0;
}

// True when every byte of s belongs to a well-formed UTF-8 sequence.
inline bool is_valid_utf8(std::string_view s)
{
    for (std::size_t pos = 0; pos < s.size();)
    {
        const auto length = utf8_sequence_length(s, pos);
        if (length == 0)
            return false;
        pos += length;
    }
    return true;
}

// Returns s with every invalid byte replaced by U+FFFD, so the result is
// always valid UTF-8 and safe to put in JSON or a database text column.
inline std::string replace_invalid_utf8(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (std::size_t pos = 0; pos < s.size();)
    {
        const auto length = utf8_sequence_length(s, pos);
        if (length == 0)
        {
            detail::append_utf8(out, kReplacementCharacter);
            ++pos;
            continue;
        }
        out.append(s.substr(pos, length));
        pos += length;
    }
    return out;
}

// Replaces invalid UTF-8 in place. Returns true when something was changed,
// so callers can tell whether a stored copy of the original needs rewriting.
inline bool sanitize_utf8_in_place(std::string& s)
{
    if (is_valid_utf8(s))
        return false;
    s = replace_invalid_utf8(s);
    return true;
}

// Shortens s to at most max_bytes without splitting a UTF-8 sequence. s is
// assumed to be valid UTF-8 (sanitise first); an invalid tail is cut as well.
inline std::string truncate_utf8(std::string_view s, std::size_t max_bytes)
{
    if (s.size() <= max_bytes)
        return std::string(s);
    std::size_t end = max_bytes;
    // Step back over continuation bytes (10xxxxxx) to a character start.
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
        --end;
    return std::string(s.substr(0, end));
}

// The one place peer-supplied strings are made safe for the log (issue #157).
// Control characters (CR, LF, ESC, DEL, ...), the Unicode line separators
// U+0085, U+2028 and U+2029, and invalid UTF-8 bytes become '?', so a peer
// cannot start a fake log line or inject terminal escapes. The result is
// cut to at most max_bytes bytes, never in the middle of a character.
inline std::string sanitize_log_text(std::string_view s, std::size_t max_bytes = 64)
{
    std::string out;
    out.reserve(std::min(s.size(), max_bytes));
    for (std::size_t pos = 0; pos < s.size();)
    {
        const auto length = utf8_sequence_length(s, pos);
        const auto sequence = s.substr(pos, length == 0 ? 1 : length);
        std::string_view piece = sequence;
        const std::string_view line_separators[] = {"\xC2\x85", "\xE2\x80\xA8", "\xE2\x80\xA9"};
        bool replace = length == 0;
        if (length == 1)
        {
            const auto byte = static_cast<unsigned char>(sequence[0]);
            replace = byte < 0x20 || byte == 0x7F;
        }
        else
        {
            for (const auto separator : line_separators)
                replace = replace || sequence == separator;
        }
        if (replace)
            piece = "?";
        if (out.size() + piece.size() > max_bytes)
            break;
        out.append(piece);
        pos += sequence.size();
    }
    return out;
}

} // namespace thewatcher::text
