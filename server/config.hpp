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

    static std::filesystem::path default_path();
    static ServerConfig load_or_create(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;
};

} // namespace thewatcher::server
