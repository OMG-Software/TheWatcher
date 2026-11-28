#include "runbook.hpp"

#include "indicators.hpp"

#include <algorithm>
#include <cctype>

namespace thewatcher::server
{

namespace
{
    constexpr std::size_t kMaxUrlBytes = 2048;

    bool equals_ignore_case(std::string_view a, std::string_view b)
    {
        return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                   return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
               });
    }

    bool is_hex(char c)
    {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
    }

    // RFC 3986 reg-name: unreserved characters, sub-delimiters and %XX escapes.
    bool is_valid_reg_name(std::string_view host)
    {
        if (host.empty())
            return false;
        for (std::size_t i = 0; i < host.size(); ++i)
        {
            const auto c = static_cast<unsigned char>(host[i]);
            if (std::isalnum(c) || std::string_view("-._~!$&'()*+,;=").find(host[i]) != std::string_view::npos)
                continue;
            if (host[i] == '%' && i + 2 < host.size() && is_hex(host[i + 1]) && is_hex(host[i + 2]))
            {
                i += 2;
                continue;
            }
            return false;
        }
        return true;
    }

    // The inside of "[...]": hex digits, ':' and '.', with at least one ':'.
    bool is_valid_ipv6_literal(std::string_view literal)
    {
        return !literal.empty() && literal.find(':') != std::string_view::npos &&
               std::all_of(literal.begin(), literal.end(), [](char c) {
                   return is_hex(c) || c == ':' || c == '.';
               });
    }

    // One to five digits, 1..65535.
    bool is_valid_port(std::string_view port)
    {
        if (port.empty() || port.size() > 5 || !std::all_of(port.begin(), port.end(), [](unsigned char c) {
                return std::isdigit(c) != 0;
            }))
            return false;
        const int value = std::stoi(std::string(port));
        return value >= 1 && value <= 65535;
    }

    // authority = host [ ":" port ]; userinfo is not allowed.
    bool is_valid_authority(std::string_view authority)
    {
        if (authority.empty() || authority.find('@') != std::string_view::npos)
            return false;
        if (authority.front() == '[')
        {
            const auto close = authority.find(']');
            if (close == std::string_view::npos || !is_valid_ipv6_literal(authority.substr(1, close - 1)))
                return false;
            const auto rest = authority.substr(close + 1);
            return rest.empty() || (rest.front() == ':' && is_valid_port(rest.substr(1)));
        }
        const auto colon = authority.rfind(':');
        if (colon == std::string_view::npos)
            return is_valid_reg_name(authority);
        return is_valid_reg_name(authority.substr(0, colon)) && is_valid_port(authority.substr(colon + 1));
    }
} // namespace

bool is_valid_runbook_url(std::string_view url)
{
    if (url.empty() || url.size() > kMaxUrlBytes)
        return false;
    // Whitespace, control characters and DEL never belong in a URL we render or forward.
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) {
            return c <= 0x20 || c == 0x7f;
        }))
        return false;

    // scheme ":" "//" authority [ path / query / fragment ]. Plain http is
    // allowed on purpose: runbooks often live on internal wikis, and the server
    // never fetches the URL, it only stores, renders and forwards it.
    const auto colon = url.find(':');
    if (colon == std::string_view::npos)
        return false;
    const auto scheme = url.substr(0, colon);
    if (!equals_ignore_case(scheme, "http") && !equals_ignore_case(scheme, "https"))
        return false;
    const auto rest = url.substr(colon + 1);
    if (rest.substr(0, 2) != "//")
        return false;
    const auto after_slashes = rest.substr(2);
    return is_valid_authority(after_slashes.substr(0, after_slashes.find_first_of("/?#")));
}

std::string resolve_runbook_url(Store& store, const std::string& agent_id, const std::string& indicator,
                                const std::string& status)
{
    // Candidates come best first; the first one with a valid URL wins, so an
    // invalid stored URL (an old database) never hides a usable fallback.
    for (const auto& runbook : store.get_runbook_candidates(agent_id, indicator, status))
    {
        if (is_valid_runbook_url(runbook.url))
            return runbook.url;
    }
    return {};
}

AlertRecord make_offline_alert(Store& store, const std::string& agent_id, int64_t now_ms)
{
    AlertRecord alert;
    alert.agent_id = agent_id;
    alert.indicator = std::string(indicators::kHeartbeat);
    alert.old_status = "green";
    alert.new_status = "red";
    alert.message = "Agent has gone offline (no heartbeat received)";
    alert.created_at = now_ms;
    alert.runbook_url = resolve_runbook_url(store, agent_id, alert.indicator, alert.new_status);
    return alert;
}

AlertRecord make_log_match_alert(Store& store, const std::string& agent_id, const std::string& indicator,
                                 const std::string& severity, const std::string& path, const std::string& matched_line,
                                 int64_t now_ms)
{
    AlertRecord alert;
    alert.agent_id = agent_id;
    alert.indicator = indicator;
    alert.old_status = "green";
    alert.new_status = severity;
    alert.message = "Log match in " + path + ": " + matched_line.substr(0, 200);
    alert.created_at = now_ms;
    alert.runbook_url = resolve_runbook_url(store, agent_id, indicator, severity);
    return alert;
}

} // namespace thewatcher::server
