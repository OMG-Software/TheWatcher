#pragma once

#include <array>
#include <string>

#include <sodium.h>

namespace thewatcher::server
{

// Key in server_settings that records the one-time upgrade which cleared the
// sessions stored as raw tokens. Present means the upgrade already ran.
inline constexpr const char* session_tokens_hashed_setting = "session_tokens_hashed";

// Returns the lowercase hex SHA-256 of a session token. Stores keep this
// value instead of the token, so a copy of the database (or a backup) cannot
// be replayed as a login. A session token is 256 bits of randomness, so a
// plain unsalted hash is enough: there is nothing to brute-force.
inline std::string hash_session_token(const std::string& token)
{
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(), reinterpret_cast<const unsigned char*>(token.data()), token.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    return hex.data();
}

// Hash of the "keep this session" argument of delete_sessions_for_user. Empty
// stays empty, which matches no stored row, so an empty argument keeps none.
inline std::string hash_kept_session_token(const std::string& keep_token)
{
    return keep_token.empty() ? std::string{} : hash_session_token(keep_token);
}

} // namespace thewatcher::server
