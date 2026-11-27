// Unit tests for the ZAP handler's key -> agent_id identity binding (SEC-04).
//
// The ZAP reply's user_id is what libzmq stamps onto every message from a
// CURVE peer as the "User-Id" metadata property; the server uses it as the
// authoritative identity when validating frame.agent_id.
#include "../server/zap_handler.hpp"
#include "common/crypto.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

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

SCENARIO("A session identity names the agent and the key it authenticated with")
{
    GIVEN("an agent_id and a key")
    {
        const std::string key = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";

        WHEN("an identity is made and parsed back")
        {
            const auto parsed = parse_session_identity(make_session_identity("agent-alpha", key));

            THEN("both parts round-trip")
            {
                REQUIRE(parsed.has_value());
                REQUIRE(parsed->agent_id == "agent-alpha");
                REQUIRE(parsed->key_z85 == key);
            }
        }

        WHEN("a user_id has no agent_id, no key or no separator")
        {
            THEN("it does not parse")
            {
                REQUIRE_FALSE(parse_session_identity("").has_value());
                REQUIRE_FALSE(parse_session_identity("agent-alpha").has_value());
                REQUIRE_FALSE(parse_session_identity(":" + key).has_value());
                REQUIRE_FALSE(parse_session_identity("agent-alpha:").has_value());
            }
        }
    }
}

SCENARIO("ZAP authenticates a real CURVE connection and stamps the session identity")
{
    GIVEN("a CURVE ROUTER served by a ZapHandler with one approved agent key")
    {
        thewatcher::crypto::init();
        const auto server_keys = thewatcher::crypto::generate_curve_keypair();
        const auto agent_keys = thewatcher::crypto::generate_curve_keypair();
        const auto stranger_keys = thewatcher::crypto::generate_curve_keypair();

        zmq::context_t ctx{1};
        ZapHandler zap{ctx};
        zap.add_key(agent_keys.public_key_z85, "agent-alpha");
        // The handler binds its inproc endpoint on its own thread. libzmq does
        // not authenticate a handshake that starts before the bind, so wait.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        zmq::socket_t router{ctx, ZMQ_ROUTER};
        router.set(zmq::sockopt::linger, 0);
        router.set(zmq::sockopt::rcvtimeo, 3000);
        router.set(zmq::sockopt::curve_server, true);
        router.set(zmq::sockopt::curve_secretkey, std::string_view(server_keys.secret_key_z85));
        router.bind("tcp://127.0.0.1:*");
        const auto endpoint = router.get(zmq::sockopt::last_endpoint);

        auto connect_dealer = [&](const thewatcher::crypto::CurveKeyPair& keys) {
            zmq::socket_t dealer{ctx, ZMQ_DEALER};
            dealer.set(zmq::sockopt::linger, 0);
            dealer.set(zmq::sockopt::curve_serverkey, std::string_view(server_keys.public_key_z85));
            dealer.set(zmq::sockopt::curve_publickey, std::string_view(keys.public_key_z85));
            dealer.set(zmq::sockopt::curve_secretkey, std::string_view(keys.secret_key_z85));
            dealer.connect(endpoint);
            dealer.send(zmq::message_t("hi", 2), zmq::send_flags::none);
            return dealer;
        };

        WHEN("the approved agent connects and sends a message")
        {
            auto dealer = connect_dealer(agent_keys);
            zmq::message_t identity, payload;

            THEN("the message carries User-Id agent_id:key")
            {
                REQUIRE(router.recv(identity, zmq::recv_flags::none).has_value());
                REQUIRE(router.recv(payload, zmq::recv_flags::none).has_value());
                REQUIRE(std::string(identity.gets("User-Id")) ==
                        make_session_identity("agent-alpha", agent_keys.public_key_z85));
            }
        }

        WHEN("a peer with an unapproved key connects")
        {
            auto dealer = connect_dealer(stranger_keys);
            zmq::message_t identity;

            THEN("nothing is delivered")
            {
                REQUIRE_FALSE(router.recv(identity, zmq::recv_flags::none).has_value());
            }
        }

        WHEN("the approved key is removed before a new peer connects")
        {
            zap.remove_key(agent_keys.public_key_z85);
            auto dealer = connect_dealer(agent_keys);
            zmq::message_t identity;

            THEN("the handshake is refused")
            {
                REQUIRE_FALSE(router.recv(identity, zmq::recv_flags::none).has_value());
            }
        }
    }
}
