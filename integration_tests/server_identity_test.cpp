#include "common/crypto.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "test_support/enrollment_client.hpp"
#include "test_support/server_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include <httplib.h>
#include <zmq.hpp>

using namespace std::chrono_literals;
using namespace thewatcher::test;
using json = nlohmann::json;

namespace
{

// A CURVE-enabled server (unless disabled) with an HTTP client for the REST API.
struct IdentityFixture : ServerFixture
{
    explicit IdentityFixture(bool with_curve = true)
        : ServerFixture(ServerFixtureOptions::with_curve(with_curve))
    {
    }

    // Enrolls an agent over the enrollment socket (no CURVE there by design).
    void enroll_with_key(const std::string& agent_id, const std::string& public_key_z85)
    {
        REQUIRE(exchange_enrollment(config.enrollment_address, encode_enrollment(make_enroll_request(
                                                                   agent_id, public_key_z85, "identity-test-host")))
                    .has_value());
    }

    void enroll_and_approve_with_key(const std::string& agent_id, const std::string& public_key_z85)
    {
        enroll_with_key(agent_id, public_key_z85);
        REQUIRE(approve(agent_id));
    }
};

// Number of metric rows currently stored for agent_id.
std::size_t metric_row_count(httplib::Client& client, const std::string& agent_id)
{
    auto res = client.Get("/api/metrics");
    if (!res || res->status != 200)
        return 0;
    const auto rows = json::parse(res->body);
    std::size_t count = 0;
    for (const auto& row : rows)
        if (row.value("agent_id", "") == agent_id)
            ++count;
    return count;
}

// A persistent CURVE DEALER speaking the data-plane protocol.
struct AgentPeer
{
    explicit AgentPeer(const thewatcher::server::ServerConfig& cfg, const std::string& routing_id,
                       const std::string& public_key_z85, const std::string& secret_key_z85)
    {
        socket = std::make_unique<zmq::socket_t>(ctx, ZMQ_DEALER);
        socket->set(zmq::sockopt::linger, 0);
        socket->set(zmq::sockopt::routing_id, routing_id);
        socket->set(zmq::sockopt::curve_serverkey, std::string_view(cfg.server_public_key));
        socket->set(zmq::sockopt::curve_publickey, std::string_view(public_key_z85));
        socket->set(zmq::sockopt::curve_secretkey, std::string_view(secret_key_z85));
        socket->connect(cfg.bind_address);
        std::this_thread::sleep_for(500ms);
    }

    // Sends one METRICS frame claiming agent_id with the given cpu usage.
    // frame_timestamp_ms lets tests forge an agent-supplied clock (SEC-07);
    // 0 means "use the current time".
    void send_metrics(const std::string& agent_id, double cpu_percent, int64_t frame_timestamp_ms = 0)
    {
        thewatcher::SystemMetrics metrics;
        metrics.cpu.usage_percent = cpu_percent;
        metrics.hostname = "identity-test-host";
        metrics.platform = "windows";
        metrics.uptime_seconds = 60.0;

        thewatcher::proto::Frame frame;
        frame.type = static_cast<uint8_t>(thewatcher::proto::FrameType::METRICS);
        frame.agent_id = agent_id;
        frame.timestamp_ms = frame_timestamp_ms != 0 ? frame_timestamp_ms : now_ms();
        frame.payload = thewatcher::proto::pack(metrics);

        const auto encoded = thewatcher::proto::encode_frame(frame);
        socket->send(zmq::message_t{}, zmq::send_flags::sndmore);
        socket->send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
        std::this_thread::sleep_for(500ms);
    }

    zmq::context_t ctx{1};
    std::unique_ptr<zmq::socket_t> socket;
};

} // namespace

SCENARIO("An approved agent cannot spoof another agent's identity")
{
    GIVEN("a CURVE server with two separately approved agents")
    {
        thewatcher::crypto::init();
        IdentityFixture fx(/*with_curve=*/true);
        fx.start_and_login();

        const auto keys_a = thewatcher::crypto::generate_curve_keypair();
        const auto keys_b = thewatcher::crypto::generate_curve_keypair();
        // The server rate-limits enrollment per source IP (SEC-05), so space
        // the two enrollments out.
        fx.enroll_and_approve_with_key("agent-victim", keys_b.public_key_z85);
        std::this_thread::sleep_for(1100ms);
        fx.enroll_and_approve_with_key("agent-rogue", keys_a.public_key_z85);

        WHEN("the rogue agent's key connects as the victim's routing id and sends METRICS claiming the victim's id")
        {
            AgentPeer rogue(fx.config, "agent-victim", keys_a.public_key_z85, keys_a.secret_key_z85);
            rogue.send_metrics("agent-victim", 42.0);

            THEN("no metrics are stored for the victim and the server stays healthy")
            {
                REQUIRE(metric_row_count(fx.client, "agent-victim") == 0);
                REQUIRE(metric_row_count(fx.client, "agent-rogue") == 0);
                REQUIRE(fx.error == nullptr);
                const auto agents = fx.client.Get("/api/agents");
                REQUIRE(agents);
                REQUIRE(agents->status == 200);
            }
        }
    }
}

SCENARIO("A rejected agent cannot keep submitting data on its live session")
{
    GIVEN("a CURVE server with an approved agent that has already delivered metrics")
    {
        thewatcher::crypto::init();
        IdentityFixture fx(/*with_curve=*/true);
        fx.start_and_login();

        const auto keys = thewatcher::crypto::generate_curve_keypair();
        fx.enroll_and_approve_with_key("agent-doomed", keys.public_key_z85);

        AgentPeer peer(fx.config, "agent-doomed", keys.public_key_z85, keys.secret_key_z85);
        peer.send_metrics("agent-doomed", 10.0);

        REQUIRE(eventually(
            [&] {
                return metric_row_count(fx.client, "agent-doomed") == 1;
            },
            5s));

        WHEN("the agent is rejected and sends another metrics frame on the same session")
        {
            REQUIRE(fx.reject("agent-doomed"));
            peer.send_metrics("agent-doomed", 20.0);
            std::this_thread::sleep_for(2s);

            THEN("the frame is dropped and no new metrics row appears")
            {
                REQUIRE(metric_row_count(fx.client, "agent-doomed") == 1);
                REQUIRE(fx.error == nullptr);
            }
        }
    }
}

SCENARIO("Only approved agents' frames are processed when CURVE is unavailable")
{
    GIVEN("a server without CURVE keys and a pending (not yet approved) agent")
    {
        thewatcher::crypto::init();
        IdentityFixture fx(/*with_curve=*/false);
        fx.start_and_login();

        const auto keys = thewatcher::crypto::generate_curve_keypair();
        fx.enroll_with_key("agent-pending", keys.public_key_z85);

        // Without CURVE the peer connects to the plain ROUTER.
        zmq::context_t ctx{1};
        zmq::socket_t dealer{ctx, ZMQ_DEALER};
        dealer.set(zmq::sockopt::linger, 0);
        dealer.set(zmq::sockopt::routing_id, "agent-pending");
        dealer.connect(fx.config.bind_address);

        auto send_metrics = [&](double cpu_percent) {
            thewatcher::SystemMetrics metrics;
            metrics.cpu.usage_percent = cpu_percent;
            metrics.hostname = "identity-test-host";
            metrics.platform = "windows";

            thewatcher::proto::Frame frame;
            frame.type = static_cast<uint8_t>(thewatcher::proto::FrameType::METRICS);
            frame.agent_id = "agent-pending";
            frame.timestamp_ms = now_ms();
            frame.payload = thewatcher::proto::pack(metrics);

            const auto encoded = thewatcher::proto::encode_frame(frame);
            dealer.send(zmq::message_t{}, zmq::send_flags::sndmore);
            dealer.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
            std::this_thread::sleep_for(500ms);
        };

        WHEN("the pending agent sends metrics before approval")
        {
            send_metrics(10.0);
            std::this_thread::sleep_for(1s);

            THEN("the frames are dropped — pending agents are not trusted on the data plane")
            {
                REQUIRE(metric_row_count(fx.client, "agent-pending") == 0);

                AND_WHEN("the agent is approved and sends metrics again")
                {
                    REQUIRE(fx.approve("agent-pending"));
                    send_metrics(20.0);

                    THEN("the frame is processed")
                    {
                        REQUIRE(eventually(
                            [&] {
                                return metric_row_count(fx.client, "agent-pending") == 1;
                            },
                            5s));
                        REQUIRE(fx.error == nullptr);
                    }
                }
            }
        }
    }
}

SCENARIO("An agent-supplied future timestamp cannot wipe metric history")
{
    GIVEN("a CURVE server with two approved agents that have both delivered metrics")
    {
        thewatcher::crypto::init();
        IdentityFixture fx(/*with_curve=*/true);
        fx.start_and_login();

        const auto keys_a = thewatcher::crypto::generate_curve_keypair();
        const auto keys_b = thewatcher::crypto::generate_curve_keypair();
        // Space the enrollments out to clear the per-source rate limit.
        fx.enroll_and_approve_with_key("agent-future", keys_a.public_key_z85);
        std::this_thread::sleep_for(1100ms);
        fx.enroll_and_approve_with_key("agent-stable", keys_b.public_key_z85);

        AgentPeer peer_a(fx.config, "agent-future", keys_a.public_key_z85, keys_a.secret_key_z85);
        AgentPeer peer_b(fx.config, "agent-stable", keys_b.public_key_z85, keys_b.secret_key_z85);
        peer_a.send_metrics("agent-future", 10.0);
        peer_b.send_metrics("agent-stable", 10.0);

        REQUIRE(eventually(
            [&] {
                return metric_row_count(fx.client, "agent-future") == 1 &&
                       metric_row_count(fx.client, "agent-stable") == 1;
            },
            5s));

        WHEN("one agent sends a METRICS frame stamped with a year-2100 timestamp")
        {
            // Pre-SEC-07 the retention prune ran on this timestamp, so its
            // cutoff moved to 2070 and deleted every other agent's history.
            peer_a.send_metrics("agent-future", 99.0, /*frame_timestamp_ms=*/4'102'444'800'000LL);
            std::this_thread::sleep_for(1s);

            THEN("the other agent's history is untouched")
            {
                REQUIRE(metric_row_count(fx.client, "agent-stable") == 1);
                REQUIRE(metric_row_count(fx.client, "agent-future") == 1);
                REQUIRE(fx.error == nullptr);
            }
        }
    }
}