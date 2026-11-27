#include "common/crypto.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <stdexcept>

using namespace thewatcher::crypto;

SCENARIO("CURVE Z85 keys are encoded without depending on ZeroMQ")
{
    GIVEN("a raw all-zero Curve25519 key")
    {
        const std::array<unsigned char, 32> raw{};

        WHEN("the key is encoded as Z85")
        {
            const auto encoded = z85_encode(raw);

            THEN("the stable Z85 representation is produced")
            {
                REQUIRE(encoded == std::string(40, '0'));
            }

            THEN("the encoded key decodes back to the original bytes")
            {
                REQUIRE(z85_decode(encoded) == raw);
            }
        }
    }
}

SCENARIO("CURVE Z85 decoding rejects invalid key material")
{
    GIVEN("a Z85 key with the wrong encoded length")
    {
        const std::string short_key = "0";

        WHEN("the key is decoded")
        {
            THEN("the key is rejected")
            {
                REQUIRE_THROWS_AS(z85_decode(short_key), std::invalid_argument);
            }
        }
    }

    GIVEN("a Z85 key containing a byte outside the Z85 alphabet")
    {
        std::string invalid_key(40, '0');
        invalid_key[0] = '\\';

        WHEN("the key is decoded")
        {
            THEN("the key is rejected")
            {
                REQUIRE_THROWS_AS(z85_decode(invalid_key), std::invalid_argument);
            }
        }
    }
}
