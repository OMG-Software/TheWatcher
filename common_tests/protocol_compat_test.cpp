// Cross-version decoding tests (#102). Peers built from different releases
// must read each other: decoders accept longer arrays and ignore the unknown
// trailing fields, and a Frame carries a protocol version.
#include "common/commands.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::proto;

namespace
{
using Bytes = std::vector<uint8_t>;

// Writes a CBOR item header (major type + argument) for values up to 65535.
void head(Bytes& out, uint8_t major, uint64_t value)
{
    const auto m = static_cast<uint8_t>(major << 5);
    if (value < 24)
    {
        out.push_back(static_cast<uint8_t>(m | value));
    }
    else if (value < 256)
    {
        out.push_back(static_cast<uint8_t>(m | 24));
        out.push_back(static_cast<uint8_t>(value));
    }
    else
    {
        out.push_back(static_cast<uint8_t>(m | 25));
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value & 0xFF));
    }
}

void array(Bytes& out, std::size_t count)
{
    head(out, 4, count);
}

void uint_item(Bytes& out, uint64_t value)
{
    head(out, 0, value);
}

void text(Bytes& out, const std::string& value)
{
    head(out, 3, value.size());
    // push_back loop: vector::insert on small vectors trips a GCC 11 -Werror false positive.
    for (const auto byte : value)
        out.push_back(static_cast<uint8_t>(byte));
}

void bytes_item(Bytes& out, const Bytes& value)
{
    head(out, 2, value.size());
    // push_back loop: vector::insert on small vectors trips a GCC 11 -Werror false positive.
    for (const auto byte : value)
        out.push_back(static_cast<uint8_t>(byte));
}

void boolean(Bytes& out, bool value)
{
    out.push_back(value ? 0xF5 : 0xF4);
}

// A Frame array with `element_count` elements. The first four are the legacy
// layout; the fifth is the protocol version; any further element is an
// unknown future field (a text string).
Bytes frame_bytes(std::size_t element_count, uint64_t version = 2)
{
    Bytes out;
    array(out, element_count);
    uint_item(out, static_cast<uint8_t>(FrameType::HEARTBEAT));
    text(out, "agent-1");
    uint_item(out, 1234);
    bytes_item(out, Bytes{0x01, 0x02});
    if (element_count >= 5)
        uint_item(out, version);
    for (std::size_t i = 5; i < element_count; ++i)
        text(out, "future");
    return out;
}

// A NetworkMetrics array: the 12 legacy elements, then `extra` more elements
// (the first extra one is the is_loopback bool, the rest are unknown text).
Bytes network_bytes(std::size_t extra, bool loopback = false)
{
    Bytes out;
    array(out, 12 + extra);
    text(out, "eth0");
    for (int i = 0; i < 8; ++i)
        uint_item(out, 10 + i); // sent, recv, packets x2, errors x2, drops x2
    text(out, "192.0.2.5");
    text(out, "aa:bb:cc:dd:ee:ff");
    boolean(out, true);
    for (std::size_t i = 0; i < extra; ++i)
    {
        if (i == 0)
            boolean(out, loopback);
        else
            text(out, "future");
    }
    return out;
}
} // namespace

SCENARIO("Frames carry a protocol version")
{
    GIVEN("a Frame built by the current release")
    {
        Frame frame;
        frame.type = static_cast<uint8_t>(FrameType::HEARTBEAT);
        frame.agent_id = "agent-1";

        WHEN("it is encoded and decoded")
        {
            const auto encoded = encode_frame(frame);
            const auto decoded = decode_frame(encoded.data(), encoded.size());

            THEN("the decoded frame reports the current protocol version")
            {
                REQUIRE(decoded.protocol_version == PROTOCOL_VERSION);
            }
        }
    }
}

SCENARIO("Frame decoding is compatible across protocol versions")
{
    GIVEN("a legacy four-element frame from an agent that predates versioning")
    {
        const auto bytes = frame_bytes(4);

        WHEN("the current server decodes it")
        {
            const auto decoded = decode_frame(bytes.data(), bytes.size());

            THEN("it is accepted as protocol version 1 with every field intact")
            {
                REQUIRE(decoded.protocol_version == 1);
                REQUIRE(decoded.agent_id == "agent-1");
                REQUIRE(decoded.timestamp_ms == 1234);
                REQUIRE(decoded.payload == Bytes{0x01, 0x02});
            }
        }
    }

    GIVEN("a frame from a newer minor release that appends unknown fields")
    {
        const auto bytes = frame_bytes(7);

        WHEN("the current server decodes it")
        {
            const auto decoded = decode_frame(bytes.data(), bytes.size());

            THEN("the known fields decode and the trailing ones are ignored")
            {
                REQUIRE(decoded.protocol_version == 2);
                REQUIRE(decoded.agent_id == "agent-1");
                REQUIRE(decoded.payload == Bytes{0x01, 0x02});
            }
        }
    }

    GIVEN("a frame declaring a protocol major version newer than this build supports")
    {
        const auto bytes = frame_bytes(5, PROTOCOL_VERSION + 1);

        THEN("decoding fails with an explicit version error")
        {
            try
            {
                (void)decode_frame(bytes.data(), bytes.size());
                FAIL("decode_frame should have thrown");
            }
            catch (const std::runtime_error& error)
            {
                REQUIRE(std::string(error.what()) == "Unsupported protocol version");
            }
        }
    }

    GIVEN("a frame declaring protocol version 0")
    {
        const auto bytes = frame_bytes(5, 0);

        THEN("decoding fails")
        {
            REQUIRE_THROWS_AS(decode_frame(bytes.data(), bytes.size()), std::runtime_error);
        }
    }

    GIVEN("a frame with fewer than the four legacy elements")
    {
        Bytes bytes;
        array(bytes, 3);
        uint_item(bytes, 1);
        text(bytes, "agent-1");
        uint_item(bytes, 5);

        THEN("decoding fails")
        {
            REQUIRE_THROWS_AS(decode_frame(bytes.data(), bytes.size()), std::runtime_error);
        }
    }
}

SCENARIO("NetworkMetrics decoding is compatible across releases")
{
    GIVEN("a legacy twelve-element interface record without the loopback flag")
    {
        const auto bytes = network_bytes(0);

        WHEN("it is unpacked")
        {
            const auto net = unpack<NetworkMetrics>(bytes);

            THEN("the known fields decode and is_loopback defaults to false")
            {
                REQUIRE(net.interface_name == "eth0");
                REQUIRE(net.ip_address == "192.0.2.5");
                REQUIRE(net.mac_address == "aa:bb:cc:dd:ee:ff");
                REQUIRE(net.is_up);
                REQUIRE_FALSE(net.is_loopback);
            }
        }
    }

    GIVEN("an interface record from a newer release with the loopback flag and an unknown extra field")
    {
        const auto bytes = network_bytes(2, true);

        WHEN("it is unpacked")
        {
            const auto net = unpack<NetworkMetrics>(bytes);

            THEN("is_loopback is read and the unknown trailing field is ignored")
            {
                REQUIRE(net.is_loopback);
                REQUIRE(net.interface_name == "eth0");
            }
        }
    }

    GIVEN("an interface record flagged as loopback")
    {
        NetworkMetrics net;
        net.interface_name = "lo0";
        net.is_loopback = true;

        WHEN("it is packed and unpacked")
        {
            const auto decoded = unpack<NetworkMetrics>(pack(net));

            THEN("the flag survives the round trip")
            {
                REQUIRE(decoded.is_loopback);
            }
        }
    }
}

SCENARIO("Command and enrollment messages ignore unknown trailing fields")
{
    GIVEN("a CommandMessage with an extra trailing field")
    {
        Bytes bytes;
        array(bytes, 4);
        uint_item(bytes, 1);
        text(bytes, "cmd-1");
        bytes_item(bytes, Bytes{0x09});
        text(bytes, "future");

        WHEN("it is unpacked")
        {
            const auto cmd = unpack<CommandMessage>(bytes);

            THEN("the known fields decode")
            {
                REQUIRE(cmd.command_id == "cmd-1");
                REQUIRE(cmd.args == Bytes{0x09});
            }
        }
    }

    GIVEN("an AckMessage with an extra trailing field")
    {
        Bytes bytes;
        array(bytes, 4);
        text(bytes, "cmd-1");
        boolean(bytes, true);
        text(bytes, "done");
        uint_item(bytes, 7);

        THEN("it decodes")
        {
            const auto ack = unpack<AckMessage>(bytes);
            REQUIRE(ack.success);
            REQUIRE(ack.message == "done");
        }
    }

    GIVEN("an EnrollResponse with an extra trailing field")
    {
        Bytes bytes;
        array(bytes, 5);
        boolean(bytes, true);
        text(bytes, "welcome");
        text(bytes, "key");
        text(bytes, "fingerprint");
        text(bytes, "future");

        THEN("it decodes")
        {
            const auto response = unpack<EnrollResponse>(bytes);
            REQUIRE(response.approved);
            REQUIRE(response.server_public_key_fingerprint == "fingerprint");
        }
    }

    GIVEN("an EnrollRequest with an extra trailing field after the token")
    {
        Bytes bytes;
        array(bytes, 6);
        text(bytes, "a");
        text(bytes, "b");
        text(bytes, "c");
        text(bytes, "d");
        text(bytes, "token");
        text(bytes, "future");

        THEN("it decodes and keeps the token")
        {
            const auto request = unpack<EnrollRequest>(bytes);
            REQUIRE(request.enrollment_token == "token");
        }
    }
}

SCENARIO("SystemMetrics from a newer release with an extra trailing field still decodes")
{
    GIVEN("a packed SystemMetrics whose outer array gained a trailing element")
    {
        SystemMetrics metrics;
        metrics.hostname = "host-1";
        auto bytes = pack(metrics);
        // The outer array header is a single byte 0x8B (11 elements); bump it
        // to 12 and append a text element, exactly what a future agent would do.
        REQUIRE(bytes.front() == 0x8B);
        bytes.front() = 0x8C;
        text(bytes, "future");

        WHEN("it is unpacked")
        {
            const auto decoded = unpack<SystemMetrics>(bytes);

            THEN("the known fields are preserved")
            {
                REQUIRE(decoded.hostname == "host-1");
            }
        }
    }
}
