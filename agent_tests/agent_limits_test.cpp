#include "../agent/agent.hpp"
#include "../agent/config.hpp"
#include "common/agent_limits.hpp"
#include "common/commands.hpp"
#include "common/protocol.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

using Catch::Matchers::ContainsSubstring;
using thewatcher::agent::AgentConfig;

namespace
{
std::filesystem::path write_config(const thewatcher::test::ScopedTempDir& dir, const std::string& body)
{
    const auto path = dir.path() / "agent.conf";
    std::ofstream(path) << body;
    return path;
}
} // namespace

SCENARIO("The agent refuses out-of-range intervals in its config file")
{
    GIVEN("a config file with an invalid setting")
    {
        const thewatcher::test::ScopedTempDir dir("thewatcher-limits");

        WHEN("HEARTBEAT_INTERVAL is 0")
        {
            const auto path = write_config(dir, "HEARTBEAT_INTERVAL=0\n");
            THEN("loading fails naming the key and the valid range")
            {
                REQUIRE_THROWS_WITH(AgentConfig::load_or_create(path),
                                    ContainsSubstring("HEARTBEAT_INTERVAL must be between 1 and 60"));
            }
        }
        WHEN("COLLECTION_INTERVAL is negative")
        {
            const auto path = write_config(dir, "COLLECTION_INTERVAL=-5\n");
            THEN("loading fails naming the key")
            {
                REQUIRE_THROWS_WITH(AgentConfig::load_or_create(path),
                                    ContainsSubstring("COLLECTION_INTERVAL must be between 1 and 3600"));
            }
        }
        WHEN("PROCESS_LIMIT is not a number")
        {
            const auto path = write_config(dir, "PROCESS_LIMIT=lots\n");
            THEN("loading fails naming the key instead of a bare stoi error")
            {
                REQUIRE_THROWS_WITH(AgentConfig::load_or_create(path), ContainsSubstring("PROCESS_LIMIT"));
            }
        }
        WHEN("PROCESS_LIMIT overflows 32 bits")
        {
            const auto path = write_config(dir, "PROCESS_LIMIT=4294967301\n");
            THEN("it is rejected rather than wrapping to 5")
            {
                REQUIRE_THROWS_WITH(AgentConfig::load_or_create(path),
                                    ContainsSubstring("PROCESS_LIMIT must be between 1 and 1000"));
            }
        }
        WHEN("every setting is inside its range")
        {
            const auto path = write_config(dir, "HEARTBEAT_INTERVAL=60\nCOLLECTION_INTERVAL=3600\nPROCESS_LIMIT=1\n");
            const auto cfg = AgentConfig::load_or_create(path);
            THEN("the values are loaded")
            {
                REQUIRE(cfg.heartbeat_interval == 60);
                REQUIRE(cfg.collection_interval == 3600);
                REQUIRE(cfg.process_limit == 1);
            }
        }
    }
}

SCENARIO("An agent cannot be constructed with settings that would busy-loop it")
{
    GIVEN("a programmatic config with a zero heartbeat interval")
    {
        AgentConfig cfg;
        cfg.agent_id = "limits-agent";
        cfg.heartbeat_interval = 0;

        THEN("construction throws")
        {
            REQUIRE_THROWS_AS(thewatcher::agent::Agent(cfg), std::invalid_argument);
        }
    }
}

SCENARIO("Command arguments are narrowed with a range check")
{
    GIVEN("a SET_INTERVAL payload whose value is 2^32 + 5")
    {
        std::vector<std::uint8_t> bytes;
        {
            // Build the oversized payload through the int64 path the wire uses.
            auto root = thewatcher::proto::detail::adopt(cbor_new_definite_array(1));
            thewatcher::proto::detail::push(root.get(), thewatcher::proto::detail::make_int64((1LL << 32) + 5));
            unsigned char* buffer = nullptr;
            std::size_t size = 0;
            cbor_serialize_alloc(root.get(), &buffer, &size);
            bytes.assign(buffer, buffer + size);
            std::free(buffer);
        }

        THEN("decoding throws instead of silently becoming 5")
        {
            REQUIRE_THROWS(thewatcher::proto::unpack<thewatcher::SetIntervalArgs>(bytes));
        }
    }
}

SCENARIO("The shared setting ranges accept their bounds and reject everything else")
{
    using namespace thewatcher;
    REQUIRE(in_range(1, kHeartbeatIntervalRange));
    REQUIRE(in_range(60, kHeartbeatIntervalRange));
    REQUIRE_FALSE(in_range(0, kHeartbeatIntervalRange));
    REQUIRE_FALSE(in_range(61, kHeartbeatIntervalRange));
    REQUIRE_FALSE(in_range(3601, kCollectionIntervalRange));
    REQUIRE_FALSE(in_range(1001, kProcessLimitRange));
}
