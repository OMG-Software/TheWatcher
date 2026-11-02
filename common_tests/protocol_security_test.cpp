// Security tests for the CBOR decode path.
//
// Covers SEC-03: libcbor's builder allocates declared definite-container
// lengths up front (size*8 bytes) and cbor_decref recurses over children,
// so a tiny hostile payload could force a huge allocation or a stack
// overflow. These tests pin the pre-validation that rejects such payloads
// before cbor_load ever runs.
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace thewatcher::proto;

namespace
{

// CBOR array header with a 64-bit definite element count (major type 4,
// additional info 27) followed by the count bytes 0x0000000100000000
// (~4.3 billion elements) in only 9 payload bytes.
std::vector<uint8_t> huge_definite_array_payload()
{
    return {0x9B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
}

// Same trick with a map (major type 5): a 9-byte payload declaring ~4.3
// billion key/value pairs.
std::vector<uint8_t> huge_definite_map_payload()
{
    return {0xBB, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00};
}

// A chain of single-element definite arrays nested far deeper than any
// legitimate TheWatcher frame (Frame array -> opaque payload byte string).
std::vector<uint8_t> deeply_nested_payload(std::size_t depth)
{
    std::vector<uint8_t> bytes(depth, 0x81); // definite array, one element
    bytes.push_back(0x01);                   // innermost value
    return bytes;
}

} // namespace

SCENARIO("Hostile CBOR payloads are rejected by the structure pre-validation")
{
    GIVEN("a 9-byte payload declaring a huge definite array")
    {
        const auto payload = huge_definite_array_payload();

        WHEN("the payload is validated")
        {
            THEN("validation fails")
            {
                REQUIRE_THROWS_AS(validate_cbor_structure(payload.data(), payload.size()), std::runtime_error);
            }
        }
    }

    GIVEN("a 9-byte payload declaring a huge definite map")
    {
        const auto payload = huge_definite_map_payload();

        WHEN("the payload is validated")
        {
            THEN("validation fails")
            {
                REQUIRE_THROWS_AS(validate_cbor_structure(payload.data(), payload.size()), std::runtime_error);
            }
        }
    }

    GIVEN("a payload nested deeper than the depth cap")
    {
        const auto payload = deeply_nested_payload(30);

        WHEN("the payload is validated")
        {
            THEN("validation fails")
            {
                REQUIRE_THROWS_AS(validate_cbor_structure(payload.data(), payload.size()), std::runtime_error);
            }
        }
    }

    GIVEN("a payload nested within the depth cap")
    {
        const auto payload = deeply_nested_payload(4);

        WHEN("the payload is validated")
        {
            THEN("validation succeeds")
            {
                REQUIRE_NOTHROW(validate_cbor_structure(payload.data(), payload.size()));
            }
        }
    }

    GIVEN("a valid encoded Frame")
    {
        Frame original;
        original.type = static_cast<uint8_t>(FrameType::HEARTBEAT);
        original.agent_id = "agent-abc-123";
        original.timestamp_ms = 1'700'000'000'000LL;
        original.payload = {0xAA, 0xBB, 0xCC};
        const auto encoded = encode_frame(original);

        WHEN("the encoded frame is validated")
        {
            THEN("validation succeeds")
            {
                REQUIRE_NOTHROW(validate_cbor_structure(encoded.data(), encoded.size()));
            }
        }
    }

    GIVEN("a truncated payload")
    {
        const auto full = encode_frame([] {
            Frame f;
            f.type = static_cast<uint8_t>(FrameType::HEARTBEAT);
            f.agent_id = "truncated-agent";
            f.timestamp_ms = 1;
            f.payload = {0x01, 0x02, 0x03, 0x04};
            return f;
        }());
        const std::vector<uint8_t> truncated(full.begin(), full.begin() + 3);

        WHEN("the truncated payload is validated")
        {
            THEN("validation fails")
            {
                REQUIRE_THROWS_AS(validate_cbor_structure(truncated.data(), truncated.size()), std::runtime_error);
            }
        }
    }
}

SCENARIO("Hostile CBOR payloads are rejected end-to-end by decode_frame")
{
    GIVEN("a payload declaring a huge definite array")
    {
        const auto payload = huge_definite_array_payload();

        WHEN("decode_frame is called on it")
        {
            THEN("decoding fails without allocating the declared size")
            {
                REQUIRE_THROWS_AS(decode_frame(payload.data(), payload.size()), std::runtime_error);
            }
        }
    }

    GIVEN("a deeply nested payload")
    {
        const auto payload = deeply_nested_payload(30);

        WHEN("decode_frame is called on it")
        {
            THEN("decoding fails before any recursive free can overflow the stack")
            {
                REQUIRE_THROWS_AS(decode_frame(payload.data(), payload.size()), std::runtime_error);
            }
        }
    }
}