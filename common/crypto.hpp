#pragma once

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

#include <sodium.h>

namespace thewatcher::crypto
{

// z85-encoded Curve25519 keypair (format understood by ZMQ CURVE).
struct CurveKeyPair
{
    std::string public_key_z85; // 40 chars
    std::string secret_key_z85; // 40 chars
};

inline constexpr char z85_alphabet[] =
    "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#";

inline int z85_digit(char encoded)
{
    for (int i = 0; i < 85; ++i)
    {
        if (z85_alphabet[i] == encoded)
        {
            return i;
        }
    }

    throw std::invalid_argument("Invalid z85 character");
}

inline void init()
{
    if (sodium_init() < 0)
        throw std::runtime_error("libsodium initialisation failed");
}

inline std::string z85_encode(const std::array<unsigned char, 32>& raw);

// Generate a new ZMQ CURVE keypair via libsodium Curve25519.
inline CurveKeyPair generate_curve_keypair()
{
    // ZMQ CURVE uses X25519 (Curve25519 DH); crypto_box_keypair gives us the
    // same key material that zmq_curve_keypair would produce internally.
    std::array<unsigned char, 32> pk{}, sk{};
    crypto_box_keypair(pk.data(), sk.data());

    CurveKeyPair kp;
    kp.public_key_z85 = z85_encode(pk);
    kp.secret_key_z85 = z85_encode(sk);
    return kp;
}

// Decode a z85 key to its raw 32-byte representation.
inline std::array<unsigned char, 32> z85_decode(const std::string& z85)
{
    if (z85.size() != 40)
        throw std::invalid_argument("CURVE z85 key must be exactly 40 characters");

    std::array<unsigned char, 32> raw{};

    for (std::size_t i = 0; i < 8; ++i)
    {
        uint64_t value = 0;
        for (std::size_t j = 0; j < 5; ++j)
        {
            value = (value * 85u) + static_cast<uint64_t>(z85_digit(z85[(i * 5) + j]));
        }
        if (value > UINT32_MAX)
        {
            throw std::runtime_error("z85 decode overflow");
        }

        const auto decoded = static_cast<uint32_t>(value);
        raw[(i * 4) + 0] = static_cast<unsigned char>((decoded >> 24) & 0xffu);
        raw[(i * 4) + 1] = static_cast<unsigned char>((decoded >> 16) & 0xffu);
        raw[(i * 4) + 2] = static_cast<unsigned char>((decoded >> 8) & 0xffu);
        raw[(i * 4) + 3] = static_cast<unsigned char>(decoded & 0xffu);
    }

    return raw;
}

// Encode a raw 32-byte key to z85.
inline std::string z85_encode(const std::array<unsigned char, 32>& raw)
{
    std::string z85;
    z85.reserve(40);

    for (std::size_t i = 0; i < 8; ++i)
    {
        uint32_t value = (static_cast<uint32_t>(raw[(i * 4) + 0]) << 24) |
                         (static_cast<uint32_t>(raw[(i * 4) + 1]) << 16) |
                         (static_cast<uint32_t>(raw[(i * 4) + 2]) << 8) | static_cast<uint32_t>(raw[(i * 4) + 3]);

        char encoded[5]{};
        for (int j = 4; j >= 0; --j)
        {
            encoded[j] = z85_alphabet[value % 85u];
            value /= 85u;
        }
        z85.append(encoded, 5);
    }

    return z85;
}

inline std::string server_public_key_fingerprint(const std::string& public_key_z85)
{
    init();
    std::array<unsigned char, crypto_generichash_BYTES> digest{};
    crypto_generichash(digest.data(), digest.size(), reinterpret_cast<const unsigned char*>(public_key_z85.data()),
                       public_key_z85.size(), nullptr, 0);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : digest)
        out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

} // namespace thewatcher::crypto
