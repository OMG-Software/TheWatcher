// remove_agent_from_view_config: how deleting an agent edits the agent list kept
// in a view's JSON config. Ids are compared as decoded strings, never as text.
#include "../server/view_config.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

using thewatcher::server::remove_agent_from_view_config;
using json = nlohmann::json;

SCENARIO("Deleting an agent removes exactly its id from a view's agent list")
{
    GIVEN("a valid list with the agent and other keys")
    {
        const std::string config = R"({"agent_ids":["node-a","node-b"],"keep":1})";

        THEN("the id is removed and the other keys are kept")
        {
            const auto updated = remove_agent_from_view_config(config, "node-a");
            REQUIRE(updated.has_value());
            const auto parsed = json::parse(*updated);
            REQUIRE(parsed["agent_ids"] == json::array({"node-b"}));
            REQUIRE(parsed["keep"] == 1);
        }

        THEN("a different id leaves the row alone")
        {
            REQUIRE_FALSE(remove_agent_from_view_config(config, "node-c").has_value());
        }
    }

    GIVEN("a list that holds an id written with a JSON escape")
    {
        const std::string config = R"({"agent_ids":["node\u002da","node-b"]})";

        THEN("the decoded id is found and removed")
        {
            const auto updated = remove_agent_from_view_config(config, "node-a");
            REQUIRE(updated.has_value());
            REQUIRE(json::parse(*updated)["agent_ids"] == json::array({"node-b"}));
        }
    }

    GIVEN("a list that holds a longer id that contains the agent id")
    {
        const std::string config = R"({"agent_ids":["node-ab"]})";

        THEN("it does not match: node-ab does not block or change node-a")
        {
            REQUIRE_FALSE(remove_agent_from_view_config(config, "node-a").has_value());
        }
    }
}

SCENARIO("A malformed view config is never read as 'no agents'")
{
    GIVEN("a list that mixes strings and numbers and contains the agent, escaped")
    {
        const std::string config = R"({"agent_ids":["node\u002da",42]})";

        THEN("the delete is aborted")
        {
            REQUIRE_THROWS_AS(remove_agent_from_view_config(config, "node-a"), std::runtime_error);
        }
    }

    GIVEN("a mixed list that does not contain the agent, even as a substring of another id")
    {
        const std::string config = R"({"agent_ids":["node-ab",42]})";

        THEN("the row is left alone")
        {
            REQUIRE_FALSE(remove_agent_from_view_config(config, "node-a").has_value());
        }
    }

    GIVEN("a config that is not valid JSON but mentions the agent, or could hide it behind an escape")
    {
        THEN("the delete is refused")
        {
            REQUIRE_THROWS_AS(remove_agent_from_view_config("{agent_ids: node-a", "node-a"), std::runtime_error);
            REQUIRE_THROWS_AS(remove_agent_from_view_config(R"({"agent_ids":["node\u002da")", "node-a"),
                              std::runtime_error);
        }
    }

    GIVEN("a config that is not valid JSON and cannot mention the agent")
    {
        THEN("the row is left alone")
        {
            REQUIRE_FALSE(remove_agent_from_view_config("garbage", "node-a").has_value());
        }
    }

    GIVEN("agent_ids that is not a list but holds the agent id")
    {
        THEN("the delete is aborted")
        {
            REQUIRE_THROWS_AS(remove_agent_from_view_config(R"({"agent_ids":"node-a"})", "node-a"), std::runtime_error);
        }
    }
}
