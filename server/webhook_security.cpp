#include "webhook_security.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>

#ifdef _WIN32
#include <WS2tcpip.h>
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

namespace thewatcher::server
{
namespace
{
    constexpr std::size_t max_webhook_url_bytes = 2048;

    std::string lowercase_ascii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value;
    }

    bool has_forbidden_authority_char(const std::string& value)
    {
        return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) || ch == '/' || ch == '\\' || ch == '@';
        });
    }

    bool parse_port(std::string_view text, int& port)
    {
        if (text.empty() || !std::all_of(text.begin(), text.end(), [](unsigned char ch) {
                return std::isdigit(ch);
            }))
        {
            return false;
        }

        int parsed = 0;
        const auto* first = text.data();
        const auto* last = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(first, last, parsed);
        if (ec != std::errc{} || ptr != last || parsed < 1 || parsed > 65535)
            return false;

        port = parsed;
        return true;
    }

    std::optional<std::array<int, 4>> parse_ipv4_literal(const std::string& host)
    {
        std::array<int, 4> octets{};
        std::size_t start = 0;
        for (std::size_t index = 0; index < octets.size(); ++index)
        {
            const auto end = index + 1 == octets.size() ? host.size() : host.find('.', start);
            if (end == std::string::npos || end == start)
                return std::nullopt;
            if (!std::all_of(host.begin() + static_cast<std::ptrdiff_t>(start),
                             host.begin() + static_cast<std::ptrdiff_t>(end), [](unsigned char ch) {
                                 return std::isdigit(ch);
                             }))
            {
                return std::nullopt;
            }

            // Issue #142: getaddrinfo() reads a leading-zero part as octal
            // ("012.0.0.1" is 10.0.0.1), so such a host is not a canonical
            // dotted quad. Returning nullopt sends it to the numeric-host
            // check, which rejects it.
            if (end - start > 1 && host[start] == '0')
                return std::nullopt;

            int octet = 0;
            const auto* first = host.data() + start;
            const auto* last = host.data() + end;
            const auto [ptr, ec] = std::from_chars(first, last, octet);
            if (ec != std::errc{} || ptr != last || octet < 0 || octet > 255)
                return std::nullopt;
            octets[index] = octet;
            start = end + 1;
        }

        if (start != host.size() + 1)
            return std::nullopt;
        return octets;
    }

    bool is_restricted_ipv4_literal(const std::array<int, 4>& ip)
    {
        if (ip[0] == 0 || ip[0] == 10 || ip[0] == 127)
            return true;
        if (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127)
            return true;
        if (ip[0] == 169 && ip[1] == 254)
            return true;
        if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31)
            return true;
        if (ip[0] == 192 && ip[1] == 168)
            return true;
        if (ip[0] == 198 && (ip[1] == 18 || ip[1] == 19))
            return true;
        if (ip[0] >= 224)
            return true;
        if (ip[0] == 192 && ip[1] == 0 && ip[2] == 2)
            return true;
        if (ip[0] == 198 && ip[1] == 51 && ip[2] == 100)
            return true;
        if (ip[0] == 203 && ip[1] == 0 && ip[2] == 113)
            return true;
        return ip[0] == 255 && ip[1] == 255 && ip[2] == 255 && ip[3] == 255;
    }

    std::optional<std::array<std::uint8_t, 16>> parse_ipv6_literal(const std::string& host)
    {
        std::array<std::uint8_t, 16> bytes{};
#ifdef _WIN32
        if (InetPtonA(AF_INET6, host.c_str(), bytes.data()) != 1)
            return std::nullopt;
#else
        if (inet_pton(AF_INET6, host.c_str(), bytes.data()) != 1)
            return std::nullopt;
#endif
        return bytes;
    }

    // Issue #142: true when the resolver would read host as an IPv4 address
    // in a legacy form - a single number ("2130706433", "0"), fewer than four
    // parts ("127.1"), hex ("0x7f000001") or octal - instead of as a DNS name.
    // Mirrors the WHATWG URL "ends in a number" rule: the last label (ignoring
    // one trailing dot) is all digits, or "0x" followed by hex digits. No real
    // top-level domain looks like that.
    bool looks_like_numeric_ipv4_host(const std::string& host)
    {
        std::string_view view = host;
        if (!view.empty() && view.back() == '.')
            view.remove_suffix(1);
        const auto dot = view.rfind('.');
        const auto last = dot == std::string_view::npos ? view : view.substr(dot + 1);
        if (last.empty())
            return false;
        const auto is_digit = [](unsigned char ch) {
            return std::isdigit(ch) != 0;
        };
        if (std::all_of(last.begin(), last.end(), is_digit))
            return true;
        return last.size() >= 2 && last[0] == '0' && (last[1] == 'x' || last[1] == 'X') &&
               std::all_of(last.begin() + 2, last.end(), [](unsigned char ch) {
                   return std::isxdigit(ch) != 0;
               });
    }

    // Returns the four bytes of an IPv6 address starting at offset as an IPv4
    // address, for prefixes that embed one (NAT64, 6to4).
    std::array<int, 4> ipv4_at(const std::array<std::uint8_t, 16>& bytes, std::size_t offset)
    {
        return {static_cast<int>(bytes[offset]), static_cast<int>(bytes[offset + 1]),
                static_cast<int>(bytes[offset + 2]), static_cast<int>(bytes[offset + 3])};
    }

    bool is_restricted_ipv6_literal(const std::string& host)
    {
        const auto bytes = parse_ipv6_literal(host);
        if (!bytes)
            return true;

        const auto all_zero = std::all_of(bytes->begin(), bytes->end(), [](std::uint8_t value) {
            return value == 0;
        });
        if (all_zero)
            return true;

        const auto first_15_zero = std::all_of(bytes->begin(), bytes->begin() + 15, [](std::uint8_t value) {
            return value == 0;
        });
        if (first_15_zero && bytes->at(15) == 1)
            return true;

        if ((bytes->at(0) & 0xfe) == 0xfc)
            return true;
        if (bytes->at(0) == 0xfe && (bytes->at(1) & 0xc0) == 0x80)
            return true;
        // Issue #142: fec0::/10 (deprecated site-local) is internal too.
        if (bytes->at(0) == 0xfe && (bytes->at(1) & 0xc0) == 0xc0)
            return true;
        if (bytes->at(0) == 0xff)
            return true;
        if (bytes->at(0) == 0x20 && bytes->at(1) == 0x01 && bytes->at(2) == 0x0d && bytes->at(3) == 0xb8)
            return true;
        if (bytes->at(0) == 0x20 && bytes->at(1) == 0x01 && bytes->at(2) == 0x00 && bytes->at(3) == 0x02 &&
            bytes->at(4) == 0x00 && bytes->at(5) == 0x00)
        {
            return true;
        }

        const auto zero_prefix = [&](std::size_t count) {
            return std::all_of(bytes->begin(), bytes->begin() + static_cast<std::ptrdiff_t>(count),
                               [](std::uint8_t value) {
                                   return value == 0;
                               });
        };
        // Issue #142: forms that embed or translate to an IPv4 address.
        // ::a.b.c.d (deprecated IPv4-compatible, ::/96) - never a public target.
        if (zero_prefix(12))
            return true;
        // ::ffff:0:a.b.c.d (IPv4-translated, ::ffff:0:0/96).
        if (zero_prefix(8) && bytes->at(8) == 0xff && bytes->at(9) == 0xff && bytes->at(10) == 0 && bytes->at(11) == 0)
        {
            return true;
        }
        // 100::/64 (discard-only).
        if (bytes->at(0) == 0x01 && bytes->at(1) == 0x00 &&
            std::all_of(bytes->begin() + 2, bytes->begin() + 8, [](std::uint8_t value) {
                return value == 0;
            }))
        {
            return true;
        }
        // 64:ff9b::/96 (NAT64 well-known prefix): check the embedded IPv4.
        if (bytes->at(0) == 0x00 && bytes->at(1) == 0x64 && bytes->at(2) == 0xff && bytes->at(3) == 0x9b)
        {
            const bool well_known = std::all_of(bytes->begin() + 4, bytes->begin() + 12, [](std::uint8_t value) {
                return value == 0;
            });
            // 64:ff9b:1::/48 is the local-use NAT64 prefix: always internal.
            if (!well_known)
                return bytes->at(4) == 0x00 && bytes->at(5) == 0x01;
            return is_restricted_ipv4_literal(ipv4_at(*bytes, 12));
        }
        // 2002::/16 (6to4): the IPv4 address sits in bytes 2-5.
        if (bytes->at(0) == 0x20 && bytes->at(1) == 0x02)
            return is_restricted_ipv4_literal(ipv4_at(*bytes, 2));

        const auto ipv4_mapped = std::all_of(bytes->begin(), bytes->begin() + 10,
                                             [](std::uint8_t value) {
                                                 return value == 0;
                                             }) &&
                                 bytes->at(10) == 0xff && bytes->at(11) == 0xff;
        if (ipv4_mapped)
        {
            const std::array<int, 4> ipv4_tail{
                static_cast<int>(bytes->at(12)),
                static_cast<int>(bytes->at(13)),
                static_cast<int>(bytes->at(14)),
                static_cast<int>(bytes->at(15)),
            };
            return is_restricted_ipv4_literal(ipv4_tail);
        }

        return false;
    }

    bool ends_with(std::string_view value, std::string_view suffix)
    {
        return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
    }

    bool is_restricted_hostname(const std::string& host)
    {
        return host == "localhost" || host == "localhost." || ends_with(host, ".localhost") ||
               ends_with(host, ".localhost.");
    }

    bool is_restricted_host(const std::string& host)
    {
        if (const auto ipv4 = parse_ipv4_literal(host))
            return is_restricted_ipv4_literal(*ipv4);
        if (host.find(':') != std::string::npos)
            return is_restricted_ipv6_literal(host);
        // Issue #142: any other numeric form is not a canonical dotted quad
        // and getaddrinfo() would still read it as IPv4; reject it.
        if (looks_like_numeric_ipv4_host(host))
            return true;
        return is_restricted_hostname(host);
    }

    bool valid_hostname_shape(const std::string& host)
    {
        if (host.empty() || host.size() > 253 || has_forbidden_authority_char(host))
            return false;
        return std::all_of(host.begin(), host.end(), [](unsigned char ch) {
            return std::isalnum(ch) || ch == '-' || ch == '.' || ch == ':';
        });
    }

    std::optional<WebhookTarget> parse_authority(const std::string& authority, std::string path)
    {
        if (authority.empty() || has_forbidden_authority_char(authority))
            return std::nullopt;

        WebhookTarget target;
        target.path = std::move(path);

        if (authority.front() == '[')
        {
            const auto close = authority.find(']');
            if (close == std::string::npos)
                return std::nullopt;
            target.host = lowercase_ascii(authority.substr(1, close - 1));
            if (close + 1 < authority.size())
            {
                if (authority[close + 1] != ':' ||
                    !parse_port(std::string_view(authority).substr(close + 2), target.port))
                {
                    return std::nullopt;
                }
            }
        }
        else
        {
            const auto first_colon = authority.find(':');
            const auto last_colon = authority.rfind(':');
            if (first_colon != last_colon)
                return std::nullopt;
            if (last_colon != std::string::npos)
            {
                target.host = lowercase_ascii(authority.substr(0, last_colon));
                if (!parse_port(std::string_view(authority).substr(last_colon + 1), target.port))
                    return std::nullopt;
            }
            else
            {
                target.host = lowercase_ascii(authority);
            }
        }

        if (!valid_hostname_shape(target.host) || is_restricted_host(target.host))
            return std::nullopt;
        return target;
    }
} // namespace

std::optional<WebhookTarget> parse_webhook_url(const std::string& url)
{
    constexpr std::string_view prefix = "http://";
    if (url.empty() || url.size() > max_webhook_url_bytes || url.rfind(prefix, 0) != 0)
        return std::nullopt;

    const auto rest = url.substr(prefix.size());
    const auto path_start = rest.find_first_of("/?#");
    const auto authority = rest.substr(0, path_start);
    std::string path = "/";
    if (path_start != std::string::npos)
        path = rest[path_start] == '/' ? rest.substr(path_start) : "/" + rest.substr(path_start);

    return parse_authority(authority, std::move(path));
}

} // namespace thewatcher::server
