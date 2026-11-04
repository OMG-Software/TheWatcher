// Security tests for the CBOR decode path.
//
// Covers SEC-03: libcbor's builder allocates declared definite-container
// lengths up front (size*8 bytes) and cbor_decref recurses over children,
// so a tiny hostile payload could force a huge allocation or a stack
// overflow. These tests pin the pre-validation that rejects such payloads
// before cbor_load ever runs.
#include "common/collector_config.hpp"
#include "common/metrics.hpp"
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

namespace
{

// TemperatureMetrics is ["sensor", "label", <double>]; the last item is
// replaced by the raw CBOR bytes under test.
std::vector<uint8_t> temperature_payload(const std::vector<uint8_t>& value)
{
    std::vector<uint8_t> bytes{0x83, 0x61, 'a', 0x61, 'b'};
    bytes.insert(bytes.end(), value.begin(), value.end());
    return bytes;
}

// PercentThresholds is [<double>, <double>, <double>]; the first item is
// replaced by the raw CBOR bytes under test.
std::vector<uint8_t> thresholds_payload(const std::vector<uint8_t>& value)
{
    std::vector<uint8_t> bytes{0x83};
    bytes.insert(bytes.end(), value.begin(), value.end());
    const std::vector<uint8_t> two_doubles{0xFB, 0x40, 0x59, 0, 0, 0, 0, 0, 0,  // 100.0
                                           0xFB, 0x40, 0x59, 0, 0, 0, 0, 0, 0}; // 100.0
    bytes.insert(bytes.end(), two_doubles.begin(), two_doubles.end());
    return bytes;
}

} // namespace

SCENARIO("CBOR simple values in a double position are rejected (issue #139)")
{
    // null, undefined, true, false share major type 7 with floats but have
    // no float payload; reading them as float8 dereferenced NULL.
    const std::vector<std::vector<uint8_t>> simple_values{{0xF6}, {0xF7}, {0xF5}, {0xF4}};

    GIVEN("a metrics payload with a simple value where a double is expected")
    {
        for (const auto& value : simple_values)
        {
            const auto payload = temperature_payload(value);
            // DYNAMIC_SECTION: a plain THEN in a loop runs only for the first
            // value, because Catch2 identifies sections by name and line.
            DYNAMIC_SECTION("unpack throws instead of crashing for simple value 0x" << std::hex
                                                                                    << static_cast<int>(value[0]))
            {
                REQUIRE_THROWS_AS(unpack<thewatcher::TemperatureMetrics>(payload), std::runtime_error);
            }
        }
    }

    GIVEN("a collector-config payload with a simple value where a double is expected")
    {
        for (const auto& value : simple_values)
        {
            const auto payload = thresholds_payload(value);
            DYNAMIC_SECTION("unpack throws instead of crashing for simple value 0x" << std::hex
                                                                                    << static_cast<int>(value[0]))
            {
                REQUIRE_THROWS_AS(unpack<thewatcher::PercentThresholds>(payload), std::runtime_error);
            }
        }
    }
}

SCENARIO("Half and single precision CBOR floats decode as doubles (issue #139)")
{
    GIVEN("a half-precision 1.0 (f9 3c00) in a metrics payload")
    {
        const auto payload = temperature_payload({0xF9, 0x3C, 0x00});
        THEN("it decodes to 1.0 without reading past the item")
        {
            REQUIRE(unpack<thewatcher::TemperatureMetrics>(payload).temperature_celsius == 1.0);
        }
    }

    GIVEN("a single-precision 1.5 (fa 3fc00000) in a collector-config payload")
    {
        const auto payload = thresholds_payload({0xFA, 0x3F, 0xC0, 0x00, 0x00});
        THEN("it decodes to 1.5 without reading past the item")
        {
            REQUIRE(unpack<thewatcher::PercentThresholds>(payload).warning_percent == 1.5);
        }
    }

    GIVEN("a half-precision 1.0 (f9 3c00) in a collector-config payload")
    {
        const auto payload = thresholds_payload({0xF9, 0x3C, 0x00});
        THEN("it decodes to 1.0 without reading past the item")
        {
            REQUIRE(unpack<thewatcher::PercentThresholds>(payload).warning_percent == 1.0);
        }
    }

    GIVEN("a single-precision 1.5 (fa 3fc00000) in a metrics payload")
    {
        const auto payload = temperature_payload({0xFA, 0x3F, 0xC0, 0x00, 0x00});
        THEN("it decodes to 1.5 without reading past the item")
        {
            REQUIRE(unpack<thewatcher::TemperatureMetrics>(payload).temperature_celsius == 1.5);
        }
    }
}
