#pragma once

#include <filesystem>
#include <optional>
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

    // Issue #150: the file values of fields replaced by a command-line
    // override. save() writes these, not the overrides, so --server,
    // --enrollment and --server-key apply to the running process only.
    // nullopt means "not overridden": save() writes the current value.
    std::optional<std::string> file_server_address;
    std::optional<std::string> file_enrollment_address;
    std::optional<std::string> file_server_public_key;
    std::optional<std::string> file_server_public_key_fingerprint;

    // Applies the non-empty command-line overrides for this process and
    // remembers the file values they replace (issue #150). A --server-key
    // override also keeps the file's fingerprint pin: the key and its pin are
    // restored together on save.
    void apply_cli_overrides(const std::string& server, const std::string& enrollment, const std::string& server_key);

    static AgentConfig load_or_create(const std::filesystem::path& path);
    // Writes the config as KEY=VALUE lines. Throws std::runtime_error if the file
    // cannot be opened. ALLOW_INSECURE is written only when allow_insecure is
    // set; allow_insecure_cli is never persisted. Fields overridden on the
    // command line are written with their file values (issue #150). ENROLLMENT_TOKEN is written
    // only when non-empty. On POSIX the file is then restricted to owner
    // read/write because it holds the CURVE secret key.
    void save(const std::filesystem::path& path) const;

    static std::filesystem::path default_path();
};

} // namespace thewatcher::agent
