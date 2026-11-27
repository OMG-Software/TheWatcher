#pragma once

#include <cstddef>
#include <string_view>

namespace thewatcher::server
{

// The longest agent_id enrollment accepts.
inline constexpr std::size_t kMaxAgentIdLength = 64;

// The one agent_id format. agent_id is a routing identity, a database key and
// a part of the ZAP session identity, so it is limited to letters, digits and
// "._-": no ':' (the session identity separator), no control characters and
// nothing that is not valid UTF-8. Enrollment checks it before creating an
// agent and the data plane checks it on every frame, so a peer cannot get an
// unvalidated identity into storage or JSON even with CURVE disabled.
inline bool is_valid_agent_id(std::string_view agent_id)
{
    return !agent_id.empty() && agent_id.size() <= kMaxAgentIdLength &&
           agent_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
               std::string_view::npos;
}

} // namespace thewatcher::server
