#pragma once

#include "store.hpp"

#include <string>
#include <string_view>

namespace thewatcher::server
{

// True for an absolute http:// or https:// URL with a host and no whitespace or
// control characters, at most 2048 bytes. Rejects javascript:, data:, file: and
// every other scheme, because runbook URLs are rendered as links and sent to
// webhooks.
bool is_valid_runbook_url(std::string_view url);

// The runbook link to attach to an alert for `agent_id` / `indicator` / `status`,
// or an empty string. Precedence, first match wins:
//   1. this agent's runbook for the exact indicator ("disk:/")
//   2. this agent's runbook for the indicator kind ("disk")
//   3. this agent's wildcard runbook ("*")
//   4. a global runbook for the exact indicator, 5. for the kind, 6. the global wildcard
// A stored URL that is not a valid http(s) URL is ignored.
std::string resolve_runbook_url(Store& store, const std::string& agent_id, const std::string& indicator,
                                const std::string& status);

} // namespace thewatcher::server
