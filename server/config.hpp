#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace thewatcher::server
{

struct ServerConfig
{
    std::string bind_address = "tcp://*:5555";
    std::string enrollment_address = "tcp://*:5556";
    std::string api_host = "0.0.0.0";
    int api_port = 8080;
    std::string db_path = "thewatcher.db";
    std::string db_type = "sqlite"; // "sqlite" or "postgres"
    std::string postgres_dsn;       // only used when db_type == "postgres"
    int offline_after_seconds = 60;

    // CURVE keypair (z85-encoded). Empty = disable encryption.
    std::string server_public_key;
    std::string server_secret_key;

    // SEC-05: optional pre-shared enrollment token. When non-empty, every
    // enrollment request must carry a matching token, so unauthenticated
    // hosts can no longer create pending-agent records at will.
    std::string enrollment_token;

    // Reverse-proxy addresses whose X-Forwarded-For / X-Real-IP headers the
    // API trusts for login throttling. Empty = trust none.
    // Config key TRUSTED_PROXIES (comma-separated).
    std::vector<std::string> trusted_proxies;

    // Keys found in the loaded file that this release does not recognise
    // (usually typos). Set by load_or_create, which also logs a warning for
    // each; they are not an error so a config written by a newer release still
    // loads after a downgrade. Never written by save().
    std::vector<std::string> unknown_keys;

    // Throws std::runtime_error on Windows when the common application data
    // folder cannot be determined (it never guesses a location).
    static std::filesystem::path default_path();
    // `preferred` when that file exists, else `legacy` when only it exists, else
    // `preferred`. Throws std::runtime_error when `preferred` cannot be inspected:
    // the legacy file is used only once the preferred one is known to be absent.
    static std::filesystem::path existing_path_or_legacy(const std::filesystem::path& preferred,
                                                         const std::filesystem::path& legacy);
    // Loads the KEY=VALUE config at path, or creates it with a new CURVE keypair
    // when missing. A relative sqlite DB_PATH resolves against the config
    // directory. Throws std::runtime_error if the file is over 1 MiB, a line is
    // over 8 KiB, or the file cannot be read or written. Throws
    // std::invalid_argument for a bad value: "API_PORT: not a number (line 3)",
    // a port outside 1-65535, OFFLINE_AFTER_SECONDS outside 0-604800 (0 disables
    // offline detection), a DB_TYPE other than sqlite/postgres, postgres without
    // POSTGRES_DSN, or a CURVE keypair that is half set, malformed or mismatched.
    // Unknown keys only warn (see unknown_keys). SEC-15: warns when group or
    // others can read an existing file.
    static ServerConfig load_or_create(const std::filesystem::path& path);
    // Writes the config to path atomically (SEC-15): an owner-only temp file is
    // flushed, then renamed over the target, so a crash cannot truncate an
    // existing config. On POSIX, directories created here are limited to the
    // owner. ENROLLMENT_TOKEN and TRUSTED_PROXIES are written only when set.
    // Throws std::runtime_error on an empty filename or any directory, write or
    // rename failure.
    void save(const std::filesystem::path& path) const;
};

} // namespace thewatcher::server
