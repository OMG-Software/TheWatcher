#include "../agent/config.hpp"
#include "../agent/enrollment.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>

#include <zmq.hpp>

using namespace thewatcher::agent;
using namespace thewatcher::proto;

namespace
{

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

} // namespace

// Normal approval

SCENARIO("Enrollment completes when the server immediately approves")
{
    GIVEN("an enrollment server that approves the first request")
    {
        thewatcher::crypto::init();
        auto kp = thewatcher::crypto::generate_curve_keypair();
        auto server_kp = thewatcher::crypto::generate_curve_keypair();

        const std::string addr = "tcp://127.0.0.1:19871";

        zmq::context_t server_ctx(1);
        zmq::socket_t rep(server_ctx, ZMQ_REP);
        rep.set(zmq::sockopt::linger, 0);
        rep.set(zmq::sockopt::rcvtimeo, 3000);
        rep.bind(addr);

        AgentConfig cfg;
        cfg.agent_id = "enroll-test-immediate";
        cfg.agent_public_key = kp.public_key_z85;
        cfg.agent_secret_key = kp.secret_key_z85;
        cfg.enrollment_address = addr;

        // Server thread: receive one request, respond approved.
        std::thread server_thread([&]() {
            zmq::message_t req;
            if (!rep.recv(req, zmq::recv_flags::none))
                return;
            send_enroll_response(rep, cfg.agent_id, true, server_kp.public_key_z85);
        });

        WHEN("enroll is called")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            thewatcher::agent::enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);

            server_thread.join();

            THEN("it returns without throwing")
            {
                // Reaching here means enroll() returned normally.
                REQUIRE(cfg.server_public_key == server_kp.public_key_z85);
                REQUIRE(cfg.server_public_key_fingerprint ==
                        thewatcher::crypto::server_public_key_fingerprint(server_kp.public_key_z85));
            }
        }
    }
}

SCENARIO("Enrollment rejects an approved response when the pinned fingerprint changes")
{
    GIVEN("an agent config with a pinned server fingerprint")
    {
        thewatcher::crypto::init();
        auto agent_kp = thewatcher::crypto::generate_curve_keypair();
        auto original_server_kp = thewatcher::crypto::generate_curve_keypair();
        auto changed_server_kp = thewatcher::crypto::generate_curve_keypair();

        const std::string addr = "tcp://127.0.0.1:19873";

        zmq::context_t server_ctx(1);
        zmq::socket_t rep(server_ctx, ZMQ_REP);
        rep.set(zmq::sockopt::linger, 0);
        rep.set(zmq::sockopt::rcvtimeo, 3000);
        rep.bind(addr);

        AgentConfig cfg;
        cfg.agent_id = "enroll-test-pin";
        cfg.agent_public_key = agent_kp.public_key_z85;
        cfg.agent_secret_key = agent_kp.secret_key_z85;
        cfg.enrollment_address = addr;
        cfg.server_public_key = original_server_kp.public_key_z85;
        cfg.server_public_key_fingerprint =
            thewatcher::crypto::server_public_key_fingerprint(original_server_kp.public_key_z85);

        std::thread server_thread([&]() {
            zmq::message_t req;
            if (!rep.recv(req, zmq::recv_flags::none))
                return;
            send_enroll_response(rep, cfg.agent_id, true, changed_server_kp.public_key_z85);
        });

        WHEN("enroll receives approval for a different server key")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);

            THEN("the pinned fingerprint prevents accepting the changed key")
            {
                try
                {
                    thewatcher::agent::enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
                    FAIL("enrollment accepted a changed server fingerprint");
                }
                catch (const std::runtime_error& ex)
                {
                    // SEC-08: the configured key pin fires before the
                    // fingerprint comparison — an unauthenticated reply can
                    // neither replace nor shadow a configured key.
                    REQUIRE(std::string{ex.what()} == "Enrollment server key does not match configured key");
                }
            }
        }

        server_thread.join();
    }
}

// Socket recovery after recv timeout

SCENARIO("Enrollment recovers after a transient server non-response")
{
    GIVEN("an enrollment endpoint where no server is listening on the first attempt")
    {
        thewatcher::crypto::init();
        auto kp = thewatcher::crypto::generate_curve_keypair();

        const std::string addr = "tcp://127.0.0.1:19872";

        AgentConfig cfg;
        cfg.agent_id = "enroll-test-recovery";
        cfg.agent_public_key = kp.public_key_z85;
        cfg.agent_secret_key = kp.secret_key_z85;
        cfg.enrollment_address = addr;

        std::atomic<bool> stop{false};
        std::atomic<bool> enrolled{false};
        zmq::context_t agent_ctx(1);

        WHEN("enroll is called and the server comes online after the first recv timeout")
        {
            // Run enrollment in the background; first send lands in ZMQ's buffer
            // (no server yet), recv times out after 500 ms, then the fix recreates
            // the socket and sends a second request.
            std::thread enroll_thread([&]() {
                try
                {
                    thewatcher::agent::enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);
                    enrolled.store(true);
                }
                catch (...)
                {
                }
            });

            // Give the first recv timeout time to fire (~600 ms is enough for 500 ms timeout).
            std::this_thread::sleep_for(std::chrono::milliseconds(700));

            // Now start the server; the second request (from the recreated socket)
            // will be delivered because ZMQ queued it while connecting.
            zmq::context_t server_ctx(1);
            zmq::socket_t rep(server_ctx, ZMQ_REP);
            rep.set(zmq::sockopt::linger, 0);
            rep.set(zmq::sockopt::rcvtimeo, 3000);
            rep.bind(addr);

            zmq::message_t req;
            if (rep.recv(req, zmq::recv_flags::none))
            {
                auto server_kp = thewatcher::crypto::generate_curve_keypair();
                send_enroll_response(rep, cfg.agent_id, true, server_kp.public_key_z85);
            }

            // Wait up to 3 seconds for enrollment to complete.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!enrolled.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));

            stop.store(true);
            enroll_thread.join();

            THEN("enrollment completed successfully after socket recovery")
            {
                REQUIRE(enrolled.load());
            }
        }
    }
}
