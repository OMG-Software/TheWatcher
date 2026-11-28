#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

// The one place that defines the indicator names the server raises alerts on.
// StatusEngine, the offline scan and the API all build and read names through
// these helpers, so a silence or runbook can only drift from a real alert if
// this file changes (a test checks the engine against kinds()).
namespace thewatcher::server::indicators
{

inline constexpr std::string_view kCpu = "cpu";
inline constexpr std::string_view kMemory = "memory";
inline constexpr std::string_view kDisk = "disk";       // "disk:<mount point>"
inline constexpr std::string_view kNetwork = "network"; // "network:<interface>"
inline constexpr std::string_view kProcess = "process"; // "process:<name>"
inline constexpr std::string_view kHeartbeat = "Heartbeat";

// One selectable indicator kind. `qualifier` names what follows the colon in a
// per-item indicator ("mount point", "interface"); it is empty for kinds that
// are a single indicator.
struct Kind
{
    std::string_view name;
    std::string_view label;
    std::string_view qualifier;
};

inline const std::array<Kind, 6>& kinds()
{
    static const std::array<Kind, 6> all{
        {
         {kCpu, "CPU", ""},
         {kMemory, "Memory", ""},
         {kDisk, "Disk (every mount point)", "mount point"},
         {kNetwork, "Network (every interface)", "interface"},
         {kProcess, "Watched processes", "process"},
         {kHeartbeat, "Agent offline (heartbeat)", ""},
         }
    };
    return all;
}

// Builds "<kind>:<qualifier>", for example make(kDisk, "/") == "disk:/".
inline std::string make(std::string_view kind, std::string_view qualifier)
{
    std::string out(kind);
    out += ':';
    out += qualifier;
    return out;
}

// The part before the first colon: "disk:/" -> "disk", "cpu" -> "cpu".
inline std::string kind_of(std::string_view indicator)
{
    return std::string(indicator.substr(0, indicator.find(':')));
}

inline bool is_known_kind(std::string_view kind)
{
    return std::any_of(kinds().begin(), kinds().end(), [&](const Kind& k) {
        return k.name == kind;
    });
}

namespace detail
{
    inline std::string lower(std::string_view text)
    {
        std::string out(text);
        std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return out;
    }

    // The spelling the engine uses for a kind written in any case or in the
    // plural form the old dashboard offered ("processes"); empty if not a kind.
    inline std::string_view canonical_kind(std::string_view kind)
    {
        const auto folded = lower(kind);
        if (folded == "processes")
            return kProcess;
        for (const auto& k : kinds())
        {
            if (lower(k.name) == folded)
                return k.name;
        }
        return {};
    }
} // namespace detail

// Normalises a name typed or stored by a user: a known kind (any case, or the
// old "processes") gets the engine's spelling, the qualifier is kept as is.
// Anything else (for example a log-match indicator) is returned unchanged.
inline std::string canonical(std::string_view name)
{
    const auto colon = name.find(':');
    const auto kind = detail::canonical_kind(name.substr(0, colon));
    if (kind.empty())
        return std::string(name);
    std::string out(kind);
    if (colon != std::string_view::npos)
        out += name.substr(colon);
    return out;
}

// Names a silence rule may be stored under to suppress an alert on `indicator`:
// the exact name, its kind (so "disk" silences "disk:/"), and the spellings
// older dashboards stored ("heartbeat", "processes"). "*" is handled by the store.
inline std::vector<std::string> silence_candidates(const std::string& indicator)
{
    std::vector<std::string> out{indicator};
    const auto kind = kind_of(indicator);
    if (kind != indicator)
        out.push_back(kind);
    if (kind == kProcess)
        out.emplace_back("processes");
    else if (kind == kHeartbeat)
        out.emplace_back("heartbeat");
    return out;
}

} // namespace thewatcher::server::indicators
