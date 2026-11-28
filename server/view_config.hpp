#pragma once

// Helper shared by the SQLite and PostgreSQL stores: a view keeps its agent ids
// inside the JSON text of views.config_json, so deleting an agent has to edit
// that text.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace thewatcher::server
{

namespace detail
{
    // True when `value` is, or contains, a string equal to `text` once decoded.
    inline bool json_mentions(const nlohmann::json& value, const std::string& text)
    {
        if (value.is_string())
            return value.get<std::string>() == text;
        if (value.is_array() || value.is_object())
        {
            for (const auto& child : value)
            {
                if (json_mentions(child, text))
                    return true;
            }
        }
        return false;
    }
} // namespace detail

// Returns the config JSON with agent_id removed from its "agent_ids" list and
// every other key kept, or nullopt when nothing has to change.
//
// Ids are compared as decoded strings, so an id written with a JSON unicode escape
// matches and a longer id ("node-ab") does not. A config that cannot be read as a list
// of strings is never taken as "no agents":
// - JSON that parses but is malformed (not an object, "agent_ids" not a list, or
//   a list with non-string entries) is walked, ignoring non-string entries for
//   matching; when it still holds the id this throws std::runtime_error so the
//   caller's transaction rolls back and the id is not left behind, otherwise the
//   row is left alone.
// - Text that is not JSON cannot be walked, so it is refused rather than guessed
//   at when it mentions the id or contains a backslash (an escape could hide it).
inline std::optional<std::string> remove_agent_from_view_config(const std::string& config_json,
                                                                const std::string& agent_id)
{
    const auto refuse = [&] {
        return std::runtime_error("a view config cannot be decoded and may reference agent " + agent_id);
    };

    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    if (cfg.is_discarded())
    {
        if (config_json.find(agent_id) != std::string::npos || config_json.find('\\') != std::string::npos)
            throw refuse();
        return std::nullopt;
    }

    const bool has_list = cfg.is_object() && cfg.contains("agent_ids") && cfg["agent_ids"].is_array();
    if (!has_list)
    {
        if (detail::json_mentions(cfg, agent_id))
            throw refuse();
        return std::nullopt;
    }

    bool all_strings = true;
    bool present = false;
    for (const auto& entry : cfg["agent_ids"])
    {
        if (!entry.is_string())
            all_strings = false;
        else if (entry.get<std::string>() == agent_id)
            present = true;
    }
    if (!present)
        return std::nullopt;
    if (!all_strings)
        throw refuse();

    auto ids = cfg["agent_ids"].get<std::vector<std::string>>();
    ids.erase(std::remove(ids.begin(), ids.end(), agent_id), ids.end());
    cfg["agent_ids"] = ids;
    return cfg.dump();
}

} // namespace thewatcher::server
