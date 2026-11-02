// Fail-closed enrollment tests (SEC-08).
//
// The enrollment reply arrives on the unauthenticated, unencrypted REP
// socket. An on-path attacker or rogue host can answer "approved" with its
// own key — or with no key at all. The agent must refuse anything that
// contradicts an already-configured pin, and must never silently connect
// the data plane without CURVE unless the operator explicitly opted in.
#include "../agent/config.hpp"
#include "../agent/enrollment.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"
#include "test_support/enroll_reply_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <stdexcept>
#include <string>

#include <zmq.hpp>

using namespace thewatcher::agent;
using namespace thewatcher::proto;
using thewatcher::test::EnrollReplyServer;

namespace
{

// Agent config enrolling at `server`'s endpoint with a freshly generated keypair.
AgentConfig make_agent_config(const std::string& agent_id, const EnrollReplyServer& server)
{
    const auto agent_kp = thewatcher::crypto::generate_curve_keypair();
    AgentConfig cfg;
    cfg.agent_id = agent_id;
    cfg.agent_public_key = agent_kp.public_key_z85;
    cfg.agent_secret_key = agent_kp.secret_key_z85;
    cfg.enrollment_address = server.endpoint();
    return cfg;
}

// Runs one enrollment attempt against the scripted server.
void run_enroll(AgentConfig& cfg)
{
    std::atomic<bool> stop{false};
    zmq::context_t agent_ctx(1);
    enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
}

// Runs one enrollment attempt and returns the runtime_error message it must throw.
std::string enroll_error(AgentConfig& cfg)
{
    try
    {
        run_enroll(cfg);
    }
    catch (const std::runtime_error& ex)
    {
        return ex.what();
    }
    return {};
}

} // namespace

SCENARIO("Enrollment fails closed when an approved reply contradicts a configured server key")
{
    GIVEN("an agent config with a configured server key but no fingerprint pin")
    {
        thewatcher::crypto::init();
        auto configured_kp = thewatcher::crypto::generate_curve_keypair();
        auto attacker_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollReplyServer server;
        auto cfg = make_agent_config("enroll-sec-key-pin", server);
        cfg.server_public_key = configured_kp.public_key_z85;

        // Rogue server: approved, but presenting its own key.
        server.reply_once(cfg.agent_id, true, attacker_kp.public_key_z85);

        WHEN("enroll receives approval carrying a different server key")
        {
            const auto error = enroll_error(cfg);

            THEN("the configured key pin rejects the reply")
            {
                REQUIRE(error == "Enrollment server key does not match configured key");
                REQUIRE(cfg.server_public_key == configured_kp.public_key_z85);
            }
        }
    }
}

SCENARIO("Enrollment fails closed when an approved reply has no server key")
{
    GIVEN("an agent config with no server key and no insecure opt-in")
    {
        thewatcher::crypto::init();

        EnrollReplyServer server;
        auto cfg = make_agent_config("enroll-sec-no-key", server);

        // Rogue server: approved, no key and no fingerprint at all.
        server.reply_once(cfg.agent_id, true);

        WHEN("enroll receives approval without a server key pin")
        {
            const auto error = enroll_error(cfg);

            THEN("enrollment fails instead of connecting without CURVE")
            {
                REQUIRE(error == "Approved enrollment response missing server key pin");
            }
        }
    }
}

SCENARIO("Enrollment accepts a keyless reply only with an explicit insecure opt-in")
{
    GIVEN("an agent config with allow_insecure set and no server key")
    {
        thewatcher::crypto::init();

        EnrollReplyServer server;
        auto cfg = make_agent_config("enroll-sec-insecure", server);
        cfg.allow_insecure = true;
        server.reply_once(cfg.agent_id, true);

        WHEN("enroll receives approval without a server key pin")
        {
            run_enroll(cfg);
            server.join();

            THEN("enrollment completes and no key is learned")
            {
                REQUIRE(cfg.server_public_key.empty());
            }
        }
    }
}

SCENARIO("Enrollment accepts a keyless reply when only the process-local CLI opt-in is set")
{
    GIVEN("an agent config with allow_insecure_cli set, allow_insecure unset and no server key")
    {
        thewatcher::crypto::init();

        EnrollReplyServer server;
        auto cfg = make_agent_config("enroll-sec-insecure-cli", server);
        cfg.allow_insecure_cli = true;
        server.reply_once(cfg.agent_id, true);

        WHEN("enroll receives approval without a server key pin")
        {
            run_enroll(cfg);
            server.join();

            THEN("enrollment completes without persisting the opt-in into allow_insecure")
            {
                REQUIRE(cfg.server_public_key.empty());
                REQUIRE_FALSE(cfg.allow_insecure);
            }
        }
    }
}

SCENARIO("Enrollment still learns the server key on first run")
{
    GIVEN("an agent config with no key and a server that presents its key")
    {
        thewatcher::crypto::init();
        auto server_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollReplyServer server;
        auto cfg = make_agent_config("enroll-sec-learn", server);
        server.reply_once(cfg.agent_id, true, server_kp.public_key_z85);

        WHEN("enroll receives approval with a consistent key and fingerprint")
        {
            run_enroll(cfg);
            server.join();

            THEN("the key and fingerprint are learned for the data plane")
            {
                REQUIRE(cfg.server_public_key == server_kp.public_key_z85);
                REQUIRE(cfg.server_public_key_fingerprint ==
                        thewatcher::crypto::server_public_key_fingerprint(server_kp.public_key_z85));
            }
        }
    }
}
