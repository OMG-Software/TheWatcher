#include "common/commands.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::proto;

// ── Frame round-trip ──────────────────────────────────────────────────────────

SCENARIO("Frame round-trip encoding via CBOR")
{
    GIVEN("a populated Frame with all fields set")
    {
        Frame original;
        original.type = static_cast<uint8_t>(FrameType::HEARTBEAT);
        original.agent_id = "agent-abc-123";
        original.timestamp_ms = 1'700'000'000'000LL;
        original.payload = {0xAA, 0xBB, 0xCC};

        WHEN("the frame is encoded and decoded")
        {
            auto encoded = encode_frame(original);
            auto decoded = decode_frame(encoded.data(), encoded.size());

            THEN("all fields are preserved exactly")
            {
                REQUIRE(decoded.type == original.type);
                REQUIRE(decoded.agent_id == original.agent_id);
                REQUIRE(decoded.timestamp_ms == original.timestamp_ms);
                REQUIRE(decoded.payload == original.payload);
            }
        }
    }

    GIVEN("a Frame with an empty payload")
    {
        Frame f;
        f.type = static_cast<uint8_t>(FrameType::METRICS);
        f.agent_id = "agent-001";
        f.timestamp_ms = 0;
        f.payload = {};

        WHEN("encoded and decoded")
        {
            auto encoded = encode_frame(f);
            auto decoded = decode_frame(encoded.data(), encoded.size());

            THEN("the empty payload survives")
            {
                REQUIRE(decoded.payload.empty());
            }
        }
    }
}

// ── FrameType enum stability ──────────────────────────────────────────────────

SCENARIO("FrameType enum values match the wire protocol specification")
{
    GIVEN("the FrameType enum")
    {
        THEN("each constant has the correct wire value")
        {
            CHECK(static_cast<uint8_t>(FrameType::HEARTBEAT) == 0x01);
            CHECK(static_cast<uint8_t>(FrameType::METRICS) == 0x02);
            CHECK(static_cast<uint8_t>(FrameType::COMMAND) == 0x03);
            CHECK(static_cast<uint8_t>(FrameType::COMMAND_ACK) == 0x04);
            CHECK(static_cast<uint8_t>(FrameType::CONFIG_UPDATE) == 0x05);
            CHECK(static_cast<uint8_t>(FrameType::ENROLL_REQUEST) == 0x06);
            CHECK(static_cast<uint8_t>(FrameType::ENROLL_RESPONSE) == 0x07);
        }
    }
}

// ── Enrollment payloads ───────────────────────────────────────────────────────

SCENARIO("EnrollRequest is serialized and deserialized correctly")
{
    GIVEN("a fully populated EnrollRequest")
    {
        EnrollRequest req;
        req.agent_id = "agent-xyz";
        req.hostname = "my-host";
        req.platform = "linux";
        req.curve_public_key_z85 = "abcdefghijklmnopqrstuvwxyz0123456789ABCD";

        WHEN("packed into bytes and unpacked")
        {
            auto packed = pack(req);
            auto unpacked = unpack<EnrollRequest>(packed);

            THEN("all enrollment fields match the original")
            {
                REQUIRE(unpacked.agent_id == req.agent_id);
                REQUIRE(unpacked.hostname == req.hostname);
                REQUIRE(unpacked.platform == req.platform);
                REQUIRE(unpacked.curve_public_key_z85 == req.curve_public_key_z85);
            }
        }
    }
}

namespace
{
// Hand-encodes a CBOR definite array of `count` one-character text strings
// ("a", "b", ...), so tests can feed EnrollRequest decoders arrays of any size.
std::vector<uint8_t> enroll_array_payload(std::size_t count)
{
    std::vector<uint8_t> bytes;
    bytes.push_back(static_cast<uint8_t>(0x80 | count)); // definite array, count < 24
    for (std::size_t i = 0; i < count; ++i)
    {
        bytes.push_back(0x61); // text string, length 1
        bytes.push_back(static_cast<uint8_t>('a' + i));
    }
    return bytes;
}
} // namespace

SCENARIO("EnrollRequest decoding accepts legacy 4-element and current 5-element payloads")
{
    GIVEN("a 0.7.x style 4-element payload without an enrollment token")
    {
        const auto payload = enroll_array_payload(4);

        WHEN("it is unpacked as an EnrollRequest")
        {
            auto req = unpack<EnrollRequest>(payload);

            THEN("the four fields decode and the token is empty")
            {
                REQUIRE(req.agent_id == "a");
                REQUIRE(req.hostname == "b");
                REQUIRE(req.platform == "c");
                REQUIRE(req.curve_public_key_z85 == "d");
                REQUIRE(req.enrollment_token.empty());
            }
        }
    }

    GIVEN("a current 5-element payload with an enrollment token")
    {
        const auto payload = enroll_array_payload(5);

        WHEN("it is unpacked as an EnrollRequest")
        {
            auto req = unpack<EnrollRequest>(payload);

            THEN("the token is decoded")
            {
                REQUIRE(req.curve_public_key_z85 == "d");
                REQUIRE(req.enrollment_token == "e");
            }
        }
    }

    GIVEN("payloads with 3 and 6 elements")
    {
        THEN("both are rejected")
        {
            REQUIRE_THROWS_AS(unpack<EnrollRequest>(enroll_array_payload(3)), std::runtime_error);
            REQUIRE_THROWS_AS(unpack<EnrollRequest>(enroll_array_payload(6)), std::runtime_error);
        }
    }
}

SCENARIO("EnrollResponse carries approval status correctly")
{
    GIVEN("an approved EnrollResponse")
    {
        EnrollResponse resp;
        resp.approved = true;
        resp.message = "approved";
        resp.server_public_key_z85 = "abcdefghijklmnopqrstuvwxyz0123456789ABCD";
        resp.server_public_key_fingerprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

        WHEN("packed and unpacked")
        {
            auto unpacked = unpack<EnrollResponse>(pack(resp));

            THEN("approved, message, server key, and fingerprint are preserved")
            {
                REQUIRE(unpacked.approved == true);
                REQUIRE(unpacked.message == "approved");
                REQUIRE(unpacked.server_public_key_z85 == resp.server_public_key_z85);
                REQUIRE(unpacked.server_public_key_fingerprint == resp.server_public_key_fingerprint);
            }
        }
    }

    GIVEN("a pending EnrollResponse")
    {
        EnrollResponse resp;
        resp.approved = false;
        resp.message = "pending approval";

        WHEN("packed and unpacked")
        {
            auto unpacked = unpack<EnrollResponse>(pack(resp));

            THEN("approved is false")
            {
                REQUIRE(unpacked.approved == false);
                REQUIRE(unpacked.message == "pending approval");
            }
        }
    }
}

// ── CommandMessage ────────────────────────────────────────────────────────────

SCENARIO("AckMessage round-trips success and failure outcomes")
{
    GIVEN("a successful AckMessage")
    {
        AckMessage ack;
        ack.command_id = "cmd-success-001";
        ack.success = true;
        ack.message = "applied";

        WHEN("packed and unpacked")
        {
            auto unpacked = unpack<AckMessage>(pack(ack));

            THEN("all fields are preserved")
            {
                REQUIRE(unpacked.command_id == ack.command_id);
                REQUIRE(unpacked.success == true);
                REQUIRE(unpacked.message == ack.message);
            }
        }
    }

    GIVEN("a failing AckMessage")
    {
        AckMessage ack;
        ack.command_id = "cmd-fail-002";
        ack.success = false;
        ack.message = "unknown command";

        WHEN("packed and unpacked")
        {
            auto unpacked = unpack<AckMessage>(pack(ack));

            THEN("the failure outcome and explanatory message survive")
            {
                REQUIRE(unpacked.success == false);
                REQUIRE(unpacked.message == "unknown command");
            }
        }
    }
}

SCENARIO("SetProcessLimitArgs round-trips through CBOR")
{
    GIVEN("a SetProcessLimitArgs with a non-default limit")
    {
        SetProcessLimitArgs args{50};

        WHEN("packed and unpacked")
        {
            auto unpacked = unpack<SetProcessLimitArgs>(pack(args));

            THEN("the limit is preserved exactly")
            {
                REQUIRE(unpacked.limit == 50);
            }
        }
    }
}

SCENARIO("CommandMessage with SET_INTERVAL args survives serialization")
{
    GIVEN("a SET_INTERVAL command with a 60-second interval")
    {
        SetIntervalArgs args{60};
        CommandMessage cmd;
        cmd.command_type = static_cast<uint8_t>(CommandType::SET_INTERVAL);
        cmd.command_id = "cmd-001";
        cmd.args = pack(args);

        WHEN("the CommandMessage is packed and unpacked")
        {
            auto unpacked = unpack<CommandMessage>(pack(cmd));

            THEN("command type and id are preserved")
            {
                REQUIRE(unpacked.command_type == cmd.command_type);
                REQUIRE(unpacked.command_id == cmd.command_id);
            }

            AND_THEN("the nested args decode to the correct interval")
            {
                auto decoded_args = unpack<SetIntervalArgs>(unpacked.args);
                REQUIRE(decoded_args.interval_seconds == 60);
            }
        }
    }
}

SCENARIO("ConfigUpdate carries collector configuration")
{
    GIVEN("a config update with disk, network, and process collector settings")
    {
        CollectorConfig collector_config;
        collector_config.cpu.warning_percent = 80.0;
        collector_config.cpu.degraded_percent = 90.0;
        collector_config.cpu.critical_percent = 95.0;
        collector_config.cpu_readings = 2;

        DiskMonitorConfig disk;
        disk.mount_point = "/";
        disk.device = "/dev/sda1";
        disk.enabled = true;
        disk.thresholds.warning_percent = 81.0;
        collector_config.disks.push_back(disk);

        NetworkInterfaceConfig network;
        network.interface_name = "eth0";
        network.enabled = true;
        network.thresholds.warning_mbps = 100.0;
        network.thresholds.degraded_mbps = 200.0;
        network.thresholds.critical_mbps = 300.0;
        collector_config.networks.push_back(network);

        ProcessWatchConfig process;
        process.name = "TheWatcherAgent.exe";
        process.expected_count = 1;
        collector_config.processes.push_back(process);

        ConfigUpdate update;
        update.interval_seconds = 15;
        update.heartbeat_interval_seconds = 7;
        update.process_limit = 20;
        update.collector_config = collector_config;

        WHEN("the config update is packed and unpacked")
        {
            auto unpacked = unpack<ConfigUpdate>(pack(update));

            THEN("interval and process limit are preserved")
            {
                REQUIRE(unpacked.interval_seconds == 15);
                REQUIRE(unpacked.heartbeat_interval_seconds == 7);
                REQUIRE(unpacked.process_limit == 20);
            }

            AND_THEN("collector thresholds and watches are preserved")
            {
                REQUIRE(unpacked.collector_config.cpu.warning_percent == Catch::Approx(80.0));
                REQUIRE(unpacked.collector_config.cpu_readings == 2);
                REQUIRE(unpacked.collector_config.disks.size() == 1);
                REQUIRE(unpacked.collector_config.disks[0].mount_point == "/");
                REQUIRE(unpacked.collector_config.networks.size() == 1);
                REQUIRE(unpacked.collector_config.networks[0].interface_name == "eth0");
                REQUIRE(unpacked.collector_config.processes.size() == 1);
                REQUIRE(unpacked.collector_config.processes[0].name == "TheWatcherAgent.exe");
            }
        }
    }
}

SCENARIO("CommandType enum values match the wire protocol specification")
{
    GIVEN("the CommandType enum")
    {
        THEN("each constant has the correct wire value")
        {
            CHECK(static_cast<uint8_t>(CommandType::SET_INTERVAL) == 0x01);
            CHECK(static_cast<uint8_t>(CommandType::SET_PROCESS_LIMIT) == 0x02);
            CHECK(static_cast<uint8_t>(CommandType::RESTART_COLLECTORS) == 0x03);
            CHECK(static_cast<uint8_t>(CommandType::PAUSE) == 0x04);
            CHECK(static_cast<uint8_t>(CommandType::RESUME) == 0x05);
            CHECK(static_cast<uint8_t>(CommandType::GET_STATUS) == 0x06);
            CHECK(static_cast<uint8_t>(CommandType::DISCONNECT) == 0x07);
        }
    }
}

// ── SystemMetrics ─────────────────────────────────────────────────────────────

SCENARIO("SystemMetrics survives a full Frame payload round-trip")
{
    GIVEN("a SystemMetrics with representative values in all sub-structs")
    {
        SystemMetrics m;
        m.hostname = "test-host";
        m.platform = "linux";
        m.os_name = "Ubuntu 22.04";
        m.uptime_seconds = 3600.5;

        m.cpu.usage_percent = 42.7;
        m.cpu.num_logical_cores = 8;
        m.cpu.load_avg_1m = 1.5;

        m.memory.total_bytes = 8ULL * 1024 * 1024 * 1024;
        m.memory.used_bytes = 4ULL * 1024 * 1024 * 1024;

        DiskMetrics disk;
        disk.device = "/dev/sda1";
        disk.mount_point = "/";
        disk.usage_percent = 65.0;
        m.disks.push_back(disk);

        NetworkMetrics net;
        net.interface_name = "eth0";
        net.bytes_recv_per_sec = 1024;
        net.is_up = true;
        m.networks.push_back(net);

        WHEN("packed into a Frame payload, encoded, decoded, and unpacked")
        {
            Frame f;
            f.type = static_cast<uint8_t>(FrameType::METRICS);
            f.agent_id = "agent-001";
            f.timestamp_ms = 9999;
            f.payload = pack(m);

            auto encoded = encode_frame(f);
            auto decoded_f = decode_frame(encoded.data(), encoded.size());
            auto decoded_m = unpack<SystemMetrics>(decoded_f.payload);

            THEN("static system fields are preserved")
            {
                REQUIRE(decoded_m.hostname == m.hostname);
                REQUIRE(decoded_m.platform == m.platform);
                REQUIRE(decoded_m.os_name == m.os_name);
                REQUIRE(decoded_m.uptime_seconds == Catch::Approx(3600.5));
            }

            AND_THEN("CPU metrics are preserved")
            {
                REQUIRE(decoded_m.cpu.usage_percent == Catch::Approx(42.7));
                REQUIRE(decoded_m.cpu.num_logical_cores == 8);
                REQUIRE(decoded_m.cpu.load_avg_1m == Catch::Approx(1.5));
            }

            AND_THEN("memory metrics are preserved")
            {
                REQUIRE(decoded_m.memory.total_bytes == m.memory.total_bytes);
                REQUIRE(decoded_m.memory.used_bytes == m.memory.used_bytes);
            }

            AND_THEN("disk entries are preserved")
            {
                REQUIRE(decoded_m.disks.size() == 1);
                REQUIRE(decoded_m.disks[0].device == "/dev/sda1");
                REQUIRE(decoded_m.disks[0].usage_percent == Catch::Approx(65.0));
            }

            AND_THEN("network entries are preserved")
            {
                REQUIRE(decoded_m.networks.size() == 1);
                REQUIRE(decoded_m.networks[0].interface_name == "eth0");
                REQUIRE(decoded_m.networks[0].bytes_recv_per_sec == 1024u);
                REQUIRE(decoded_m.networks[0].is_up == true);
            }
        }
    }
}
