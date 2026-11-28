#pragma once

#include "store.hpp"

#include <string>
#include <string_view>

namespace thewatcher::server
{

// True for an absolute http or https URL (scheme, "//", then a host with an
// optional port and no userinfo) with no whitespace or control characters, at
// most 2048 bytes. Rejects javascript:, data:, file: and
// every other scheme, because runbook URLs are rendered as links and sent to
// webhooks.
bool is_valid_runbook_url(std::string_view url);

// The runbook link to attach to an alert for `agent_id` / `indicator` / `status`,
// or an empty string. Precedence, first match wins:
//   1. this agent's runbook for the exact indicator ("disk:/")
//   2. this agent's runbook for the indicator kind ("disk")
//   3. this agent's wildcard runbook ("*")
//   4. a global runbook for the exact indicator, 5. for the kind, 6. the global wildcard
// A stored URL that is not valid is skipped and the next match is used.
std::string resolve_runbook_url(Store& store, const std::string& agent_id, const std::string& indicator,
                                const std::string& status);

// Alerts the server raises outside the status engine, built with their runbook
// link resolved the same way (the caller inserts them).
// The offline scan: a red "Heartbeat" alert.
AlertRecord make_offline_alert(Store& store, const std::string& agent_id, int64_t now_ms);
// A log match on `indicator` with `severity` ("yellow", "amber" or "red"); the
// message quotes at most the first 200 characters of the matched line.
AlertRecord make_log_match_alert(Store& store, const std::string& agent_id, const std::string& indicator,
                                 const std::string& severity, const std::string& path, const std::string& matched_line,
                                 int64_t now_ms);

} // namespace thewatcher::server
