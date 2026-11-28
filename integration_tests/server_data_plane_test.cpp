// Data-plane tests through a running Server with CURVE keys and a stub agent
// (issues #59, #143, #144, #157, #56).
//
// Maintenance coverage here is limited to the data-plane entry points (a
// PAUSE command reaching the agent and its ACK arriving); the maintenance
// scheduling and expiry logic is covered with the status-engine work.
#include "common/SingleLog.hpp"
#include "common/commands.hpp"
#include "common/crypto.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "test_support/enrollment_client.hpp"
#include "test_support/server_fixture.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <zmq.hpp>

using namespace std::chrono_literals;
using namespace thewatcher;
using namespace thewatcher::test;
using json = nlohmann::json;

namespace
{

// A persistent CURVE DEALER speaking the data-plane protocol.
struct AgentPeer
{
    AgentPeer(const server::ServerConfig& cfg, const crypto::CurveKeyPair& keys, const std::string& routing_id = "")
    {
        socket = std::make_unique<zmq::socket_t>(ctx, ZMQ_DEALER);
        socket->set(zmq::sockopt::linger, 0);
        if (!routing_id.empty())
            socket->set(zmq::sockopt::routing_id, routing_id);
        socket->set(zmq::sockopt::curve_serverkey, std::string_view(cfg.server_public_key));
        socket->set(zmq::sockopt::curve_publickey, std::string_view(keys.public_key_z85));
        socket->set(zmq::sockopt::curve_secretkey, std::string_view(keys.secret_key_z85));
        socket->connect(cfg.bind_address);
        std::this_thread::sleep_for(500ms);
    }

    // Sends one frame of `type` claiming agent_id.
    void send(const std::string& agent_id, proto::FrameType type, std::vector<uint8_t> payload = {})
    {
        proto::Frame frame;
        frame.type = static_cast<uint8_t>(type);
        frame.agent_id = agent_id;
        frame.timestamp_ms = now_ms();
        frame.payload = std::move(payload);
        const auto encoded = proto::encode_frame(frame);
        socket->send(zmq::message_t{}, zmq::send_flags::sndmore);
        socket->send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
        std::this_thread::sleep_for(400ms);
    }

    void send_metrics(const std::string& agent_id, const SystemMetrics& metrics)
    {
        send(agent_id, proto::FrameType::METRICS, proto::pack(metrics));
    }

    void send_cpu(const std::string& agent_id, double cpu_percent)
    {
        SystemMetrics metrics;
        metrics.cpu.usage_percent = cpu_percent;
        metrics.hostname = "data-plane-host";
        metrics.platform = "linux";
        send_metrics(agent_id, metrics);
    }

    // Waits for one server frame. The DEALER receives [empty][payload].
    std::optional<proto::Frame> receive(std::chrono::milliseconds timeout = 3s)
    {
        socket->set(zmq::sockopt::rcvtimeo, static_cast<int>(timeout.count()));
        zmq::message_t delimiter, payload;
        if (!socket->recv(delimiter, zmq::recv_flags::none))
            return std::nullopt;
        if (!socket->get(zmq::sockopt::rcvmore) || !socket->recv(payload, zmq::recv_flags::none))
            return std::nullopt;
        return proto::decode_frame(payload.data(), payload.size());
    }

    zmq::context_t ctx{1};
    std::unique_ptr<zmq::socket_t> socket;
};

struct DataPlaneFixture : ServerFixture
{
    DataPlaneFixture()
        : ServerFixture(ServerFixtureOptions::with_curve(true))
    {
    }

    // Enrolls agent_id with `keys` and approves it.
    void enroll_and_approve_with(const std::string& agent_id, const crypto::CurveKeyPair& keys)
    {
        REQUIRE(exchange_enrollment(config.enrollment_address,
                                    encode_enrollment(make_enroll_request(agent_id, keys.public_key_z85)))
                    .has_value());
        REQUIRE(approve(agent_id));
    }

    std::size_t metric_rows(const std::string& agent_id)
    {
        auto res = client.Get("/api/metrics/" + agent_id);
        if (!res || res->status != 200)
            return 0;
        std::size_t count = 0;
        for (const auto& row : json::parse(res->body))
            count += row.value("agent_id", "") == agent_id ? 1 : 0;
        return count;
    }

    std::optional<json> agent(const std::string& agent_id)
    {
        auto res = client.Get("/api/agents");
        if (!res || res->status != 200)
            return std::nullopt;
        for (const auto& a : json::parse(res->body))
            if (a.value("agent_id", "") == agent_id)
                return a;
        return std::nullopt;
    }

    json log_matches(const std::string& agent_id)
    {
        auto res = client.Get("/api/agents/" + agent_id + "/log-matches");
        return res && res->status == 200 ? json::parse(res->body) : json::array();
    }
};

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream input(path);
    return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

} // namespace

SCENARIO("The data plane handles every frame type of an approved agent")
{
    GIVEN("a CURVE server with an approved agent connected through a stub peer")
    {
        crypto::init();
        DataPlaneFixture fx;
        fx.start_and_login();
        const auto keys = crypto::generate_curve_keypair();
        fx.enroll_and_approve_with("dp-agent", keys);
        AgentPeer peer(fx.config, keys);

        WHEN("the agent sends a METRICS frame")
        {
            peer.send_cpu("dp-agent", 12.0);

            THEN("the metrics are stored and the agent is marked connected")
            {
                REQUIRE(eventually(
                    [&] {
                        return fx.metric_rows("dp-agent") == 1;
                    },
                    5s));
                const auto agent = fx.agent("dp-agent");
                REQUIRE(agent.has_value());
                REQUIRE((*agent)["connected"].get<bool>());
            }
        }

        WHEN("the agent sends a HEARTBEAT frame")
        {
            const auto before = fx.agent("dp-agent")->value("last_seen", int64_t{0});
            std::this_thread::sleep_for(50ms);
            peer.send("dp-agent", proto::FrameType::HEARTBEAT);

            THEN("last_seen advances and no metrics are stored")
            {
                REQUIRE(eventually(
                    [&] {
                        return fx.agent("dp-agent")->value("last_seen", int64_t{0}) > before;
                    },
                    5s));
                REQUIRE(fx.metric_rows("dp-agent") == 0);
            }
        }

        WHEN("the agent sends a CONFIG_REQUEST frame")
        {
            peer.send("dp-agent", proto::FrameType::CONFIG_REQUEST);
            const auto reply = peer.receive();

            THEN("it receives its CONFIG_UPDATE")
            {
                REQUIRE(reply.has_value());
                REQUIRE(reply->type == static_cast<uint8_t>(proto::FrameType::CONFIG_UPDATE));
                REQUIRE(reply->agent_id == "dp-agent");
                REQUIRE_NOTHROW(proto::unpack<ConfigUpdate>(reply->payload));
            }
        }

        WHEN("the agent sends a LOG_MATCH frame")
        {
            proto::LogMatch match{"app-errors", "/var/log/app.log", "ERROR something broke", "red"};
            peer.send("dp-agent", proto::FrameType::LOG_MATCH, proto::pack(match));

            THEN("the match is stored and an alert is raised on its indicator")
            {
                REQUIRE(eventually(
                    [&] {
                        return fx.log_matches("dp-agent").size() == 1;
                    },
                    5s));
                auto alerts = fx.client.Get("/api/alerts");
                REQUIRE(alerts);
                bool found = false;
                for (const auto& alert : json::parse(alerts->body))
                    found = found ||
                            (alert.value("agent_id", "") == "dp-agent" && alert.value("indicator", "") == "app-errors");
                REQUIRE(found);
            }
        }

        WHEN("a DISCONNECT command is queued and the agent acknowledges it")
        {
            peer.send_cpu("dp-agent", 5.0);
            auto queued = fx.client.Post("/api/agents/dp-agent/disconnect", "", "application/json");
            REQUIRE(queued);
            REQUIRE(queued->status == 200);
            const auto command = peer.receive();
            REQUIRE(command.has_value());
            REQUIRE(command->type == static_cast<uint8_t>(proto::FrameType::COMMAND));
            const auto message = proto::unpack<CommandMessage>(command->payload);
            REQUIRE(message.command_type == static_cast<uint8_t>(CommandType::DISCONNECT));
            peer.send("dp-agent", proto::FrameType::COMMAND_ACK,
                      proto::pack(AckMessage{message.command_id, true, "bye"}));

            THEN("the agent is marked disconnected")
            {
                REQUIRE(eventually(
                    [&] {
                        const auto agent = fx.agent("dp-agent");
                        return agent && !(*agent)["connected"].get<bool>();
                    },
                    5s));
            }
        }

        WHEN("a PAUSE command is queued and the agent acknowledges it")
        {
            peer.send_cpu("dp-agent", 5.0);
            auto queued = fx.client.Post("/api/agents/dp-agent/pause", "", "application/json");
            REQUIRE(queued);
            const auto command = peer.receive();
            REQUIRE(command.has_value());
            const auto message = proto::unpack<CommandMessage>(command->payload);
            peer.send("dp-agent", proto::FrameType::COMMAND_ACK,
                      proto::pack(AckMessage{message.command_id, true, "paused"}));

            THEN("the agent is in maintenance with the command reason")
            {
                REQUIRE(eventually(
                    [&] {
                        const auto agent = fx.agent("dp-agent");
                        return agent && (*agent)["maintenance"].get<bool>() &&
                               (*agent)["maintenance_reason"].get<std::string>() == "command";
                    },
                    5s));
            }
        }
    }
}

SCENARIO("A command for an agent that has not sent a frame is dropped, not queued")
{
    GIVEN("a CURVE server with an approved agent that has sent no frame")
    {
        crypto::init();
        DataPlaneFixture fx;
        fx.start_and_login();
        const auto keys = crypto::generate_curve_keypair();
        fx.enroll_and_approve_with("dp-silent", keys);

        WHEN("a command is queued for it and it connects afterwards")
        {
            auto queued = fx.client.Post("/api/agents/dp-silent/restart_collectors", "", "application/json");
            REQUIRE(queued);
            std::this_thread::sleep_for(500ms);
            AgentPeer peer(fx.config, keys);
            peer.send_cpu("dp-silent", 1.0);

            THEN("the stale command is not delivered")
            {
                REQUIRE_FALSE(peer.receive(1s).has_value());
                REQUIRE(fx.error == nullptr);
            }
        }
    }
}

SCENARIO("Frames that claim another agent's id are dropped")
{
    GIVEN("a CURVE server with an approved agent and a peer using that agent's key")
    {
        crypto::init();
        DataPlaneFixture fx;
        fx.start_and_login();
        const auto keys = crypto::generate_curve_keypair();
        fx.enroll_and_approve_with("dp-real", keys);
        AgentPeer peer(fx.config, keys);

        WHEN("it claims a different agent_id")
        {
            peer.send_cpu("dp-someone-else", 90.0);

            THEN("nothing is stored for either id")
            {
                REQUIRE(fx.metric_rows("dp-someone-else") == 0);
                REQUIRE(fx.metric_rows("dp-real") == 0);
            }
        }
    }
}

SCENARIO("A deleted agent's old session is not accepted after re-approval with a new key (issue #144)")
{
    GIVEN("an approved agent with a live session on key K1")
    {
        crypto::init();
        DataPlaneFixture fx;
        fx.start_and_login();
        const auto k1 = crypto::generate_curve_keypair();
        const auto k2 = crypto::generate_curve_keypair();
        fx.enroll_and_approve_with("dp-rotated", k1);
        AgentPeer old_session(fx.config, k1);
        old_session.send_cpu("dp-rotated", 10.0);
        REQUIRE(eventually(
            [&] {
                return fx.metric_rows("dp-rotated") == 1;
            },
            5s));

        WHEN("the agent is deleted and the same agent_id is approved again with key K2")
        {
            auto deleted = fx.client.Delete("/api/agents/dp-rotated");
            REQUIRE(deleted);
            REQUIRE(deleted->status == 200);
            // Per-agent_id enrollment rate limit is 10 s.
            std::this_thread::sleep_for(11s);
            fx.enroll_and_approve_with("dp-rotated", k2);
            const auto rows_before = fx.metric_rows("dp-rotated");

            AND_WHEN("the old K1 session sends metrics")
            {
                old_session.send_cpu("dp-rotated", 99.0);

                THEN("they are dropped")
                {
                    std::this_thread::sleep_for(500ms);
                    REQUIRE(fx.metric_rows("dp-rotated") == rows_before);
                }

                AND_THEN("a new K2 session is accepted")
                {
                    AgentPeer new_session(fx.config, k2);
                    new_session.send_cpu("dp-rotated", 20.0);
                    REQUIRE(eventually(
                        [&] {
                            return fx.metric_rows("dp-rotated") == rows_before + 1;
                        },
                        5s));
                }
            }
        }
    }
}

SCENARIO("A deleted agent's old session stays dead when the same key is approved again (issue #183)")
{
    GIVEN("an approved agent with a live session on key K")
    {
        crypto::init();
        DataPlaneFixture fx;
        fx.start_and_login();
        const auto key = crypto::generate_curve_keypair();
        fx.enroll_and_approve_with("dp-revived", key);
        AgentPeer old_session(fx.config, key);
        old_session.send_cpu("dp-revived", 10.0);
        REQUIRE(eventually(
            [&] {
                return fx.metric_rows("dp-revived") == 1;
            },
            5s));

        WHEN("the agent is deleted and the same agent_id is approved again with the same key K")
        {
            auto deleted = fx.client.Delete("/api/agents/dp-revived");
            REQUIRE(deleted);
            REQUIRE(deleted->status == 200);
            // Per-agent_id enrollment rate limit is 10 s.
            std::this_thread::sleep_for(11s);
            fx.enroll_and_approve_with("dp-revived", key);
            const auto rows_before = fx.metric_rows("dp-revived");

            AND_WHEN("the session opened before the deletion sends metrics")
            {
                old_session.send_cpu("dp-revived", 99.0);

                THEN("they are dropped")
                {
                    std::this_thread::sleep_for(500ms);
                    REQUIRE(fx.metric_rows("dp-revived") == rows_before);
                }

                AND_THEN("a new session with the same key is accepted")
                {
                    AgentPeer new_session(fx.config, key);
                    new_session.send_cpu("dp-revived", 20.0);
                    REQUIRE(eventually(
                        [&] {
                            return fx.metric_rows("dp-revived") == rows_before + 1;
                        },
                        5s));
                }
            }
        }
    }
}

SCENARIO("A reconnecting agent replaces its dead connection (issue #143)")
{
    GIVEN("an agent whose old connection is still open on the server under the same routing id")
    {
        crypto::init();
        DataPlaneFixture fx;
        fx.start_and_login();
        const auto keys = crypto::generate_curve_keypair();
        fx.enroll_and_approve_with("dp-reconnect", keys);
        AgentPeer dead(fx.config, keys, "dp-reconnect");
        dead.send_cpu("dp-reconnect", 10.0);
        REQUIRE(eventually(
            [&] {
                return fx.metric_rows("dp-reconnect") == 1;
            },
            5s));

        WHEN("the agent reconnects with the same routing id and sends metrics")
        {
            AgentPeer fresh(fx.config, keys, "dp-reconnect");
            fresh.send_cpu("dp-reconnect", 20.0);

            THEN("the new connection is served")
            {
                REQUIRE(eventually(
                    [&] {
                        return fx.metric_rows("dp-reconnect") == 2;
                    },
                    5s));
            }

            AND_THEN("a command reaches the new connection")
            {
                auto queued = fx.client.Post("/api/agents/dp-reconnect/restart_collectors", "", "application/json");
                REQUIRE(queued);
                REQUIRE(fresh.receive().has_value());
            }
        }
    }
}

SCENARIO("Agent-supplied strings cannot forge log lines or break JSON (issues #157, #56)")
{
    GIVEN("a CURVE server logging to a file and an approved agent")
    {
        crypto::init();
        const auto log_path = process_temp_dir().path() / "thewatcher-data-plane.log";
        std::error_code ec;
        std::filesystem::remove(log_path, ec);
        auto& logger = logging::SingleLog::GetInstance();
        logger.SetConsoleLogLevel(logging::LogLevel::L_OFF);
        logger.SetFileLogLevel(logging::LogLevel::L_TRACE);
        logger.SetLogFilePath(log_path.string());

        DataPlaneFixture fx;
        fx.start_and_login();
        const auto keys = crypto::generate_curve_keypair();
        fx.enroll_and_approve_with("dp-hostile", keys);
        AgentPeer peer(fx.config, keys);

        WHEN("it sends a log match whose path forges a log line and whose line is not UTF-8")
        {
            proto::LogMatch match{"forge\ntest", "x\n2026-11-04 12:00:00 [CRITICAL] admin login from 1.2.3.4",
                                  "bad bytes \xE9\xFF here", "red"};
            peer.send("dp-hostile", proto::FrameType::LOG_MATCH, proto::pack(match));
            // A WARNING makes the logger flush everything written before it.
            peer.send_cpu("dp-someone-else", 1.0);

            THEN("the log has no forged line")
            {
                REQUIRE(eventually(
                    [&] {
                        return read_file(log_path).find("Dropping frame") != std::string::npos;
                    },
                    5s));
                const auto log = read_file(log_path);
                REQUIRE(log.find("\n2026-11-04 12:00:00 [CRITICAL]") == std::string::npos);
                REQUIRE(log.find("path=x?2026-11-04") != std::string::npos);
                // The alert pipeline logs the indicator name too (#157).
                REQUIRE(log.find("indicator=forge\ntest") == std::string::npos);
                REQUIRE(log.find("indicator=forge?test") != std::string::npos);
            }

            AND_THEN("the match is stored and the APIs still answer")
            {
                REQUIRE(eventually(
                    [&] {
                        return fx.log_matches("dp-hostile").size() == 1;
                    },
                    5s));
                auto alerts = fx.client.Get("/api/alerts");
                REQUIRE(alerts);
                REQUIRE(alerts->status == 200);
                REQUIRE(fx.error == nullptr);
            }
        }

        WHEN("it sends metrics whose hostname, mount point and interface are not UTF-8")
        {
            SystemMetrics metrics;
            metrics.hostname = "host\xE9";
            metrics.platform = "linux";
            DiskMetrics disk;
            disk.mount_point = "/mnt/caf\xE9";
            disk.device = "/dev/sdb";
            disk.filesystem = "ext4";
            disk.usage_percent = 99.5;
            disk.total_bytes = 1000;
            disk.used_bytes = 995;
            metrics.disks.push_back(disk);
            NetworkMetrics network;
            network.interface_name = "eth\xFF";
            metrics.networks.push_back(network);
            peer.send_metrics("dp-hostile", metrics);
            peer.send_cpu("dp-hostile", 3.0);

            THEN("the server keeps running, stores both frames and lists agents and alerts")
            {
                REQUIRE(eventually(
                    [&] {
                        return fx.metric_rows("dp-hostile") == 2;
                    },
                    5s));
                REQUIRE(fx.error == nullptr);
                for (const auto* path : {"/api/alerts", "/api/agents", "/api/metrics", "/api/metrics/dp-hostile"})
                {
                    auto res = fx.client.Get(path);
                    REQUIRE(res);
                    REQUIRE(res->status == 200);
                }
            }
        }
    }
}
