#pragma once

#include <filesystem>
#include <string>

namespace thewatcher::agent
{

struct AgentConfig
{
    std::string agent_id;
    std::string server_address = "tcp://127.0.0.1:5555";
    std::string enrollment_address = "tcp://127.0.0.1:5556";
    std::string server_public_key; // z85, 40 chars - set after enrollment
    std::string server_public_key_fingerprint;
    std::string agent_public_key; // z85, 40 chars
    std::string agent_secret_key; // z85, 40 chars
    // SEC-08: when false (default), an enrollment reply without a server key
    // pin is a hard error — the data plane never connects without CURVE
    // silently. Set via ALLOW_INSECURE=1 in the config or --insecure.
    bool allow_insecure = false;
    // Process-local --insecure opt-in. Never persisted by save(), so a one-off
    // CLI flag does not become a permanent ALLOW_INSECURE=1 in the config file.
    bool allow_insecure_cli = false;
    int collection_interval = 30;
    int heartbeat_interval = 5;
    int process_limit = 25;
    std::string config_path;
    // SEC-05: pre-shared enrollment token, required by servers that
    // configure ENROLLMENT_TOKEN. Empty when unused.
    std::string enrollment_token;

    static AgentConfig load_or_create(const std::filesystem::path& path);
    void save(const std::filesystem::path& path) const;

    static std::filesystem::path default_path();
};

} // namespace thewatcher::agent
