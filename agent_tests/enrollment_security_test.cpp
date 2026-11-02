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

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <zmq.hpp>

using namespace thewatcher::agent;
using namespace thewatcher::proto;

namespace
{

// Unique-enough port space for these tests.
constexpr int base_port = 19910;

// Builds and sends an EnrollResponse on a REP socket.
void send_enroll_response(zmq::socket_t& rep, const std::string& agent_id, bool approved,
                          const std::string& server_public_key = {})
{
    EnrollResponse resp;
    resp.approved = approved;
    resp.message = approved ? "approved" : "pending approval";
    if (approved)
    {
        resp.server_public_key_z85 = server_public_key;
        resp.server_public_key_fingerprint = thewatcher::crypto::server_public_key_fingerprint(server_public_key);
    }
    Frame f;
    f.type = static_cast<uint8_t>(FrameType::ENROLL_RESPONSE);
    f.agent_id = agent_id;
    f.timestamp_ms = 0;
    f.payload = pack(resp);
    auto encoded = encode_frame(f);
    rep.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
}

struct EnrollServer
{
    explicit EnrollServer(int port)
    {
        rep.set(zmq::sockopt::linger, 0);
        rep.set(zmq::sockopt::rcvtimeo, 3000);
        rep.bind("tcp://127.0.0.1:" + std::to_string(port));
    }

    zmq::context_t ctx{1};
    zmq::socket_t rep{ctx, ZMQ_REP};
};

} // namespace

SCENARIO("Enrollment fails closed when an approved reply contradicts a configured server key")
{
    GIVEN("an agent config with a configured server key but no fingerprint pin")
    {
        thewatcher::crypto::init();
        auto agent_kp = thewatcher::crypto::generate_curve_keypair();
        auto configured_kp = thewatcher::crypto::generate_curve_keypair();
        auto attacker_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollServer server(base_port);

        AgentConfig cfg;
        cfg.agent_id = "enroll-sec-key-pin";
        cfg.agent_public_key = agent_kp.public_key_z85;
        cfg.agent_secret_key = agent_kp.secret_key_z85;
        cfg.enrollment_address = server.rep.get(zmq::sockopt::last_endpoint);
        cfg.server_public_key = configured_kp.public_key_z85;

        std::thread server_thread([&] {
            zmq::message_t req;
            if (!server.rep.recv(req, zmq::recv_flags::none))
                return;
            // Rogue server: approved, but presenting its own key.
            send_enroll_response(server.rep, cfg.agent_id, true, attacker_kp.public_key_z85);
        });

        WHEN("enroll receives approval carrying a different server key")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);

            THEN("the configured key pin rejects the reply")
            {
                try
                {
                    enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
                    FAIL("enrollment accepted a changed server key over a configured pin");
                }
                catch (const std::runtime_error& ex)
                {
                    REQUIRE(std::string{ex.what()} == "Enrollment server key does not match configured key");
                }
                REQUIRE(cfg.server_public_key == configured_kp.public_key_z85);
            }
        }

        server_thread.join();
    }
}

SCENARIO("Enrollment fails closed when an approved reply has no server key")
{
    GIVEN("an agent config with no server key and no insecure opt-in")
    {
        thewatcher::crypto::init();
        auto agent_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollServer server(base_port + 1);

        AgentConfig cfg;
        cfg.agent_id = "enroll-sec-no-key";
        cfg.agent_public_key = agent_kp.public_key_z85;
        cfg.agent_secret_key = agent_kp.secret_key_z85;
        cfg.enrollment_address = server.rep.get(zmq::sockopt::last_endpoint);

        std::thread server_thread([&] {
            zmq::message_t req;
            if (!server.rep.recv(req, zmq::recv_flags::none))
                return;
            // Rogue server: approved, no key and no fingerprint at all.
            send_enroll_response(server.rep, cfg.agent_id, true);
        });

        WHEN("enroll receives approval without a server key pin")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);

            THEN("enrollment fails instead of connecting without CURVE")
            {
                try
                {
                    enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
                    FAIL("enrollment approved a keyless server without an explicit insecure opt-in");
                }
                catch (const std::runtime_error& ex)
                {
                    REQUIRE(std::string{ex.what()} == "Approved enrollment response missing server key pin");
                }
            }
        }

        server_thread.join();
    }
}

SCENARIO("Enrollment accepts a keyless reply only with an explicit insecure opt-in")
{
    GIVEN("an agent config with allow_insecure set and no server key")
    {
        thewatcher::crypto::init();
        auto agent_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollServer server(base_port + 2);

        AgentConfig cfg;
        cfg.agent_id = "enroll-sec-insecure";
        cfg.agent_public_key = agent_kp.public_key_z85;
        cfg.agent_secret_key = agent_kp.secret_key_z85;
        cfg.enrollment_address = server.rep.get(zmq::sockopt::last_endpoint);
        cfg.allow_insecure = true;

        std::thread server_thread([&] {
            zmq::message_t req;
            if (!server.rep.recv(req, zmq::recv_flags::none))
                return;
            send_enroll_response(server.rep, cfg.agent_id, true);
        });

        WHEN("enroll receives approval without a server key pin")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
            server_thread.join();

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
        auto agent_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollServer server(base_port + 4);

        AgentConfig cfg;
        cfg.agent_id = "enroll-sec-insecure-cli";
        cfg.agent_public_key = agent_kp.public_key_z85;
        cfg.agent_secret_key = agent_kp.secret_key_z85;
        cfg.enrollment_address = server.rep.get(zmq::sockopt::last_endpoint);
        cfg.allow_insecure_cli = true;

        std::thread server_thread([&] {
            zmq::message_t req;
            if (!server.rep.recv(req, zmq::recv_flags::none))
                return;
            send_enroll_response(server.rep, cfg.agent_id, true);
        });

        WHEN("enroll receives approval without a server key pin")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
            server_thread.join();

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
        auto agent_kp = thewatcher::crypto::generate_curve_keypair();
        auto server_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollServer server(base_port + 3);

        AgentConfig cfg;
        cfg.agent_id = "enroll-sec-learn";
        cfg.agent_public_key = agent_kp.public_key_z85;
        cfg.agent_secret_key = agent_kp.secret_key_z85;
        cfg.enrollment_address = server.rep.get(zmq::sockopt::last_endpoint);

        std::thread server_thread([&] {
            zmq::message_t req;
            if (!server.rep.recv(req, zmq::recv_flags::none))
                return;
            send_enroll_response(server.rep, cfg.agent_id, true, server_kp.public_key_z85);
        });

        WHEN("enroll receives approval with a consistent key and fingerprint")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
            server_thread.join();

            THEN("the key and fingerprint are learned for the data plane")
            {
                REQUIRE(cfg.server_public_key == server_kp.public_key_z85);
                REQUIRE(cfg.server_public_key_fingerprint ==
                        thewatcher::crypto::server_public_key_fingerprint(server_kp.public_key_z85));
            }
        }
    }
}