// Unit tests for the data-plane hardening that needs no running server:
// ingest sanitising of agent-supplied text (#56) and the socket liveness
// options that detect dead peers (#143).
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/text_safety.hpp"
#include "common/zmq_liveness.hpp"
#include "server/ingest_sanitize.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

#include <zmq.hpp>

using namespace thewatcher;
using namespace thewatcher::server;

namespace
{

SystemMetrics metrics_with_bad_text()
{
    SystemMetrics metrics;
    metrics.hostname = "host\xE9";
    metrics.platform = "linux";
    DiskMetrics disk;
    disk.mount_point = "/mnt/caf\xE9";
    disk.device = "/dev/sda1";
    disk.filesystem = "ext4";
    metrics.disks.push_back(disk);
    NetworkMetrics network;
    network.interface_name = "eth\xFF";
    metrics.networks.push_back(network);
    ProcessInfo process;
    process.name = "bad\xC0\xAF";
    metrics.top_processes.push_back(process);
    return metrics;
}

} // namespace

SCENARIO("Agent-reported metrics with invalid UTF-8 are cleaned on ingest")
{
    GIVEN("metrics whose hostname, mount point, interface and process name are not valid UTF-8")
    {
        auto metrics = metrics_with_bad_text();

        WHEN("they are sanitised")
        {
            const bool changed = sanitize_agent_text(metrics);

            THEN("every string is valid UTF-8 with replacement characters")
            {
                REQUIRE(changed);
                REQUIRE(metrics.hostname == "host\xEF\xBF\xBD");
                REQUIRE(metrics.disks.front().mount_point == "/mnt/caf\xEF\xBF\xBD");
                REQUIRE(text::is_valid_utf8(metrics.networks.front().interface_name));
                REQUIRE(text::is_valid_utf8(metrics.top_processes.front().name));
            }

            AND_THEN("JSON built from them serialises without throwing")
            {
                REQUIRE_NOTHROW(nlohmann::json{
                    {"host",  metrics.hostname                 },
                    {"mount", metrics.disks.front().mount_point}
                }
                                    .dump());
            }

            AND_THEN("sanitising again changes nothing")
            {
                REQUIRE_FALSE(sanitize_agent_text(metrics));
            }
        }
    }

    GIVEN("a log match with invalid UTF-8 in every field")
    {
        proto::LogMatch match{"ind\xE9", "/var/log/\xFF", "line \xC3", "red\x80"};

        WHEN("it is sanitised")
        {
            const bool changed = sanitize_agent_text(match);

            THEN("all fields are valid UTF-8")
            {
                REQUIRE(changed);
                REQUIRE(text::is_valid_utf8(match.indicator_name));
                REQUIRE(text::is_valid_utf8(match.path));
                REQUIRE(text::is_valid_utf8(match.matched_line));
                REQUIRE(text::is_valid_utf8(match.severity));
            }
        }
    }

    GIVEN("clean metrics")
    {
        SystemMetrics metrics;
        metrics.hostname = "caf\xC3\xA9-host";

        WHEN("they are sanitised")
        {
            THEN("nothing changes")
            {
                REQUIRE_FALSE(sanitize_agent_text(metrics));
                REQUIRE(metrics.hostname == "caf\xC3\xA9-host");
            }
        }
    }
}

SCENARIO("Data-plane sockets enable heartbeats and TCP keepalive")
{
    GIVEN("a ROUTER and a DEALER")
    {
        zmq::context_t ctx{1};
        zmq::socket_t router{ctx, ZMQ_ROUTER};
        zmq::socket_t dealer{ctx, ZMQ_DEALER};

        WHEN("the liveness options are applied")
        {
            proto::apply_connection_liveness(router);
            proto::apply_connection_liveness(dealer);

            THEN("both report the heartbeat and keepalive settings")
            {
                for (auto* socket : {&router, &dealer})
                {
                    REQUIRE(socket->get(zmq::sockopt::heartbeat_ivl) == proto::kHeartbeatIvlMs);
                    REQUIRE(socket->get(zmq::sockopt::heartbeat_timeout) == proto::kHeartbeatTimeoutMs);
                    REQUIRE(socket->get(zmq::sockopt::heartbeat_ttl) == proto::kHeartbeatTtlMs);
                    REQUIRE(socket->get(zmq::sockopt::tcp_keepalive) == 1);
                    REQUIRE(socket->get(zmq::sockopt::tcp_keepalive_idle) == proto::kTcpKeepaliveIdleSeconds);
                }
            }
        }
    }
}
