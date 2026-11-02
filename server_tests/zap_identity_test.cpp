// Unit tests for the ZAP handler's key -> agent_id identity binding (SEC-04).
//
// The ZAP reply's user_id is what libzmq stamps onto every message from a
// CURVE peer as the "User-Id" metadata property; the server uses it as the
// authoritative identity when validating frame.agent_id.
#include "../server/zap_handler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

using namespace thewatcher::server;

SCENARIO("ZAP keys are bound to the agent_id they authenticate")
{
    GIVEN("a ZMQ context and a freshly constructed ZapHandler")
    {
        zmq::context_t ctx{1};
        ZapHandler zap{ctx};

        WHEN("a key is added with its agent_id")
        {
            const std::string key = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
            zap.add_key(key, "agent-alpha");

            THEN("the binding is visible and has_key reports the key")
            {
                REQUIRE(zap.has_key(key));
                REQUIRE(zap.user_id_for_key(key) == std::optional<std::string>{"agent-alpha"});
            }

            AND_WHEN("the same key is re-added under a different agent_id")
            {
                zap.add_key(key, "agent-beta");

                THEN("the latest binding wins")
                {
                    REQUIRE(zap.user_id_for_key(key) == std::optional<std::string>{"agent-beta"});
                }
            }

            AND_WHEN("the key is removed")
            {
                zap.remove_key(key);

                THEN("the binding disappears and has_key reports false")
                {
                    REQUIRE_FALSE(zap.has_key(key));
                    REQUIRE(zap.user_id_for_key(key) == std::nullopt);
                }
            }
        }

        WHEN("a key is added without an agent_id")
        {
            const std::string key = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
            zap.add_key(key);

            THEN("has_key reports the key but no identity is bound")
            {
                REQUIRE(zap.has_key(key));
                REQUIRE(zap.user_id_for_key(key) == std::nullopt);
            }
        }

        WHEN("an unknown key is looked up")
        {
            THEN("no identity is bound")
            {
                REQUIRE(zap.user_id_for_key("CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC") == std::nullopt);
            }
        }
    }
}