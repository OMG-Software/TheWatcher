#include "runbook.hpp"

#include <algorithm>
#include <cctype>

namespace thewatcher::server
{

bool is_valid_runbook_url(std::string_view url)
{
    constexpr std::size_t kMaxUrlBytes = 2048;
    if (url.empty() || url.size() > kMaxUrlBytes)
        return false;
    // Whitespace, control characters and DEL never belong in a URL we render or forward.
    if (std::any_of(url.begin(), url.end(), [](unsigned char c) {
            return c <= 0x20 || c == 0x7f;
        }))
        return false;

    auto starts_with_ignore_case = [&](std::string_view prefix) {
        return url.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), url.begin(), [](char a, char b) {
                   return a == std::tolower(static_cast<unsigned char>(b));
               });
    };
    std::size_t host_start = 0;
    if (starts_with_ignore_case("https://"))
        host_start = 8;
    else if (starts_with_ignore_case("http://"))
        host_start = 7;
    else
        return false;

    // A host must follow the scheme ("https:///x" and "https://" have none).
    return host_start < url.size() && url[host_start] != '/' && url[host_start] != '?' && url[host_start] != '#';
}

std::string resolve_runbook_url(Store& store, const std::string& agent_id, const std::string& indicator,
                                const std::string& status)
{
    // The store applies the precedence documented in runbook.hpp.
    const auto runbook = store.get_runbook(agent_id, indicator, status);
    if (runbook && is_valid_runbook_url(runbook->url))
        return runbook->url;
    return {};
}

} // namespace thewatcher::server
