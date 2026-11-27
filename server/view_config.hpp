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

// Returns the config JSON with agent_id removed from its "agent_ids" list and
// every other key kept, or nullopt when nothing has to change.
//
// A config that cannot be decoded (not JSON, not an object, "agent_ids" not a
// list of strings) is never read as "no agents": when its text mentions the
// agent id this throws std::runtime_error so the caller's transaction rolls
// back and the id is not left behind; when it cannot mention the id the row is
// left alone.
inline std::optional<std::string> remove_agent_from_view_config(const std::string& config_json,
                                                                const std::string& agent_id)
{
    auto cfg = nlohmann::json::parse(config_json, nullptr, false);
    std::vector<std::string> ids;
    bool decodable = cfg.is_object();
    if (decodable && cfg.contains("agent_ids"))
    {
        try
        {
            ids = cfg.at("agent_ids").get<std::vector<std::string>>();
        }
        catch (const nlohmann::json::exception&)
        {
            decodable = false;
        }
    }

    if (!decodable)
    {
        if (config_json.find(agent_id) != std::string::npos)
            throw std::runtime_error("a view config cannot be decoded and may reference agent " + agent_id);
        return std::nullopt;
    }

    const auto kept_end = std::remove(ids.begin(), ids.end(), agent_id);
    if (kept_end == ids.end())
        return std::nullopt;
    ids.erase(kept_end, ids.end());
    cfg["agent_ids"] = ids;
    return cfg.dump();
}

} // namespace thewatcher::server
