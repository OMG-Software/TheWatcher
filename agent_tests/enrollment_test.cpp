#include "../agent/config.hpp"
#include "../agent/enrollment.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"
#include "test_support/enroll_reply_server.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <zmq.hpp>

using namespace thewatcher::agent;
using namespace thewatcher::proto;

namespace
{

using thewatcher::test::EnrollReplyServer;
using thewatcher::test::send_enroll_response;

} // namespace

// Normal approval

SCENARIO("Enrollment completes when the server immediately approves")
{
    GIVEN("an enrollment server that approves the first request")
    {
        thewatcher::crypto::init();
        auto kp = thewatcher::crypto::generate_curve_keypair();
        auto server_kp = thewatcher::crypto::generate_curve_keypair();

        EnrollReplyServer server;

        AgentConfig cfg;
        cfg.agent_id = "enroll-test-immediate";
        cfg.agent_public_key = kp.public_key_z85;
        cfg.agent_secret_key = kp.secret_key_z85;
        cfg.enrollment_address = server.endpoint();

        // Server thread: receive one request, respond approved.
        server.reply_once(cfg.agent_id, true, server_kp.public_key_z85);

        WHEN("enroll is called")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            thewatcher::agent::enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/500);

            server.join();

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

        EnrollReplyServer server;

        AgentConfig cfg;
        cfg.agent_id = "enroll-test-pin";
        cfg.agent_public_key = agent_kp.public_key_z85;
        cfg.agent_secret_key = agent_kp.secret_key_z85;
        cfg.enrollment_address = server.endpoint();
        cfg.server_public_key = original_server_kp.public_key_z85;
        cfg.server_public_key_fingerprint =
            thewatcher::crypto::server_public_key_fingerprint(original_server_kp.public_key_z85);

        server.reply_once(cfg.agent_id, true, changed_server_kp.public_key_z85);

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

// Issues #86 and #160: a bad reply must not end enrollment.

namespace
{

// Answers each request with the next canned raw reply, then answers one more
// request with a genuine approval. Raw replies model garbled or foreign answers.
class ScriptedEnrollServer
{
public:
    ScriptedEnrollServer(std::vector<std::string> raw_replies, const std::string& agent_id,
                         const std::string& approved_key)
    {
        rep_.set(zmq::sockopt::linger, 0);
        rep_.set(zmq::sockopt::rcvtimeo, 5000);
        rep_.bind("tcp://127.0.0.1:*");
        endpoint_ = rep_.get(zmq::sockopt::last_endpoint);
        thread_ = std::thread([this, raw_replies = std::move(raw_replies), agent_id, approved_key] {
            for (const auto& raw : raw_replies)
            {
                zmq::message_t req;
                if (!rep_.recv(req, zmq::recv_flags::none))
                    return;
                ++requests_;
                rep_.send(zmq::message_t(raw.data(), raw.size()), zmq::send_flags::none);
            }
            zmq::message_t req;
            if (!rep_.recv(req, zmq::recv_flags::none))
                return;
            ++requests_;
            send_enroll_response(rep_, agent_id, true, approved_key);
        });
    }

    ~ScriptedEnrollServer()
    {
        if (thread_.joinable())
            thread_.join();
    }

    const std::string& endpoint() const
    {
        return endpoint_;
    }

    int requests() const
    {
        return requests_.load();
    }

private:
    zmq::context_t ctx_{1};
    zmq::socket_t rep_{ctx_, ZMQ_REP};
    std::string endpoint_;
    std::atomic<int> requests_{0};
    std::thread thread_;
};

// Encodes an ENROLL_RESPONSE frame carrying `payload`.
std::string response_frame_bytes(const std::string& agent_id, const std::vector<uint8_t>& payload)
{
    Frame frame;
    frame.type = static_cast<uint8_t>(FrameType::ENROLL_RESPONSE);
    frame.agent_id = agent_id;
    frame.payload = payload;
    const auto encoded = encode_frame(frame);
    return std::string(encoded.begin(), encoded.end());
}

AgentConfig make_config(const std::string& agent_id)
{
    const auto agent_kp = thewatcher::crypto::generate_curve_keypair();
    AgentConfig cfg;
    cfg.agent_id = agent_id;
    cfg.agent_public_key = agent_kp.public_key_z85;
    cfg.agent_secret_key = agent_kp.secret_key_z85;
    return cfg;
}

} // namespace

SCENARIO("Enrollment retries after a malformed reply instead of exiting (issue #160)")
{
    GIVEN("a server that first sends bytes that are not a frame and then approves")
    {
        thewatcher::crypto::init();
        const auto server_kp = thewatcher::crypto::generate_curve_keypair();
        auto cfg = make_config("enroll-test-malformed");
        ScriptedEnrollServer server({"definitely not a frame", std::string(3, '\0')}, cfg.agent_id,
                                    server_kp.public_key_z85);
        cfg.enrollment_address = server.endpoint();

        WHEN("enroll is called")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            thewatcher::agent::enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/2000);

            THEN("it keeps asking until the genuine approval arrives")
            {
                REQUIRE(server.requests() == 3);
                REQUIRE(cfg.server_public_key == server_kp.public_key_z85);
            }
        }
    }
}

SCENARIO("Enrollment retries after a reply whose payload does not decode (issue #160)")
{
    GIVEN("a server that first answers with a valid frame carrying a garbage payload")
    {
        thewatcher::crypto::init();
        const auto server_kp = thewatcher::crypto::generate_curve_keypair();
        auto cfg = make_config("enroll-test-bad-payload");
        ScriptedEnrollServer server({response_frame_bytes(cfg.agent_id, {0xff, 0xff, 0xff})}, cfg.agent_id,
                                    server_kp.public_key_z85);
        cfg.enrollment_address = server.endpoint();

        WHEN("enroll is called")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            thewatcher::agent::enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/2000);

            THEN("the second request is answered and enrollment succeeds")
            {
                REQUIRE(server.requests() == 2);
                REQUIRE(cfg.server_public_key == server_kp.public_key_z85);
            }
        }
    }
}

SCENARIO("Enrollment keeps asking while the server answers pending (issue #86)")
{
    GIVEN("a server that answers pending twice and then approves")
    {
        thewatcher::crypto::init();
        const auto server_kp = thewatcher::crypto::generate_curve_keypair();
        auto cfg = make_config("enroll-test-pending");

        EnrollResponse pending;
        pending.approved = false;
        pending.message = "pending approval";
        const auto pending_bytes = response_frame_bytes(cfg.agent_id, pack(pending));

        ScriptedEnrollServer server({pending_bytes, pending_bytes}, cfg.agent_id, server_kp.public_key_z85);
        cfg.enrollment_address = server.endpoint();

        WHEN("enroll is called")
        {
            std::atomic<bool> stop{false};
            zmq::context_t agent_ctx(1);
            thewatcher::agent::enroll(cfg, agent_ctx, stop, /*poll_interval=*/0, /*recv_timeout_ms=*/2000);

            THEN("it enrolls on the third request")
            {
                REQUIRE(server.requests() == 3);
                REQUIRE(cfg.server_public_key == server_kp.public_key_z85);
            }
        }
    }
}
