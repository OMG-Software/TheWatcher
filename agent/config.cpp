#include "config.hpp"

#include "common/SingleLog.hpp"
#include "common/crypto.hpp"
#include "common/secure_file.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include <sodium.h>
// JSON support intentionally absent: TheWatcherAgent.conf is KEY=VALUE only.

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace thewatcher::agent
{

namespace
{
    constexpr std::uintmax_t max_config_file_bytes = 1024 * 1024;
    constexpr std::size_t max_config_line_bytes = 8 * 1024;

    std::string generate_uuid()
    {
        uint8_t b[16];
        ::randombytes_buf(b, sizeof(b));
        b[6] = (b[6] & 0x0f) | 0x40; // version 4
        b[8] = (b[8] & 0x3f) | 0x80; // variant 2
        char buf[37];
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0],
                      b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return buf;
    }

    std::string trim(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    std::unordered_map<std::string, std::string> read_key_value_file(const std::filesystem::path& path)
    {
        std::error_code ec;
        const auto file_size = std::filesystem::file_size(path, ec);
        if (!ec && file_size > max_config_file_bytes)
            throw std::runtime_error("Config file is too large: " + path.string());

        std::ifstream f(path);
        if (!f)
            throw std::runtime_error("Cannot open " + path.string());

        std::unordered_map<std::string, std::string> values;
        std::string line;
        while (std::getline(f, line))
        {
            if (line.size() > max_config_line_bytes)
                throw std::runtime_error("Config line is too long: " + path.string());
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            const auto pos = line.find('=');
            if (pos == std::string::npos)
                continue;
            auto key = trim(line.substr(0, pos));
            auto value = trim(line.substr(pos + 1));
            values[key] = value;
        }
        return values;
    }

    // Extracts the host from "host", "host:port", "tcp://host:port" or the
    // IPv6 forms "[addr]" / "[addr]:port". The result keeps the brackets so it
    // can be put straight back into an endpoint. An unbracketed IPv6 address
    // is ambiguous with host:port and is rejected (issue #98). Throws
    // std::invalid_argument.
    std::string host_from_server_value(const std::string& value)
    {
        auto host = trim(value);
        constexpr std::string_view tcp_prefix = "tcp://";
        if (host.rfind(tcp_prefix, 0) == 0)
            host = host.substr(tcp_prefix.size());

        if (!host.empty() && host.front() == '[')
        {
            const auto close = host.find(']');
            if (close == std::string::npos)
                throw std::invalid_argument("Unterminated '[' in server address: " + value);
            const auto rest = host.substr(close + 1);
            const bool port_ok = rest.empty() || (rest.size() > 1 && rest.front() == ':' &&
                                                  rest.find_first_not_of("0123456789", 1) == std::string::npos);
            if (close == 1 || !port_ok)
                throw std::invalid_argument("Invalid bracketed IPv6 server address: " + value);
            return host.substr(0, close + 1);
        }

        const auto colons = std::count(host.begin(), host.end(), ':');
        if (colons > 1)
            throw std::invalid_argument("IPv6 server addresses must be written in brackets, e.g. [::1]:5555: " + value);
        if (colons == 1)
            host = host.substr(0, host.find(':'));
        return host.empty() ? "127.0.0.1" : host;
    }

    std::string endpoint_for_host(const std::string& host, int port)
    {
        return "tcp://" + host_from_server_value(host) + ":" + std::to_string(port);
    }

    // Value for the informational THEWATCHER_SERVER line, or empty when the
    // endpoint has no host that can be read back (the line is then omitted;
    // SERVER_ADDRESS is authoritative on load).
    std::string server_host_from_endpoint(const std::string& endpoint)
    {
        try
        {
            return host_from_server_value(endpoint);
        }
        catch (const std::invalid_argument&)
        {
            return {};
        }
    }

    // Throws std::runtime_error if value contains CR or LF, which would let it
    // start a new KEY=VALUE line in the file (issue #148).
    void require_single_line(const char* name, const std::string& value)
    {
        if (value.find_first_of("\r\n") != std::string::npos)
            throw std::runtime_error(std::string("Refusing to save config: ") + name + " contains a line break");
    }

    // Copies recognised KEY=VALUE entries onto cfg; absent keys keep their
    // defaults. ALLOW_INSECURE is true only for "1", "true" or "yes" (SEC-08).
    // Throws std::invalid_argument or std::out_of_range if a numeric
    // value is not a valid integer.
    void apply_key_value(AgentConfig& cfg, const std::unordered_map<std::string, std::string>& values)
    {
        if (auto it = values.find("THEWATCHER_SERVER"); it != values.end())
        {
            cfg.server_address = endpoint_for_host(it->second, 5555);
            cfg.enrollment_address = endpoint_for_host(it->second, 5556);
        }
        if (auto it = values.find("SERVER_ADDRESS"); it != values.end())
            cfg.server_address = it->second;
        if (auto it = values.find("ENROLLMENT_ADDRESS"); it != values.end())
            cfg.enrollment_address = it->second;
        if (auto it = values.find("SERVER_PUBLIC_KEY"); it != values.end())
            cfg.server_public_key = it->second;
        if (auto it = values.find("SERVER_PUBLIC_KEY_FINGERPRINT"); it != values.end())
            cfg.server_public_key_fingerprint = it->second;
        if (auto it = values.find("ALLOW_INSECURE"); it != values.end())
            cfg.allow_insecure = (it->second == "1" || it->second == "true" || it->second == "yes");
        if (auto it = values.find("ENROLLMENT_TOKEN"); it != values.end())
            cfg.enrollment_token = it->second;
        if (auto it = values.find("AGENT_ID"); it != values.end())
            cfg.agent_id = it->second;
        if (auto it = values.find("AGENT_PUBLIC_KEY"); it != values.end())
            cfg.agent_public_key = it->second;
        if (auto it = values.find("AGENT_SECRET_KEY"); it != values.end())
            cfg.agent_secret_key = it->second;
        if (auto it = values.find("COLLECTION_INTERVAL"); it != values.end())
            cfg.collection_interval = std::stoi(it->second);
        if (auto it = values.find("HEARTBEAT_INTERVAL"); it != values.end())
            cfg.heartbeat_interval = std::stoi(it->second);
        if (auto it = values.find("PROCESS_LIMIT"); it != values.end())
            cfg.process_limit = std::stoi(it->second);
        if (auto it = values.find("LOG_LEVEL"); it != values.end())
            cfg.log.set_level(it->second);
        if (auto it = values.find("LOG_MAX_SIZE_MB"); it != values.end())
            cfg.log.set_max_size_mib(it->second);
        if (auto it = values.find("LOG_MAX_FILES"); it != values.end())
            cfg.log.set_max_files(it->second);
    }

} // namespace

std::filesystem::path AgentConfig::default_path()
{
    LOG_FUNCTION_TRACE
#ifdef _WIN32
    char path[MAX_PATH];
    ::SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path);
    return std::filesystem::path(path) / "TheWatcher" / "TheWatcherAgent.conf";
#else
    const char* home = ::getenv("HOME");
    if (!home)
    {
        struct passwd pw{}, *result = nullptr;
        char buf[4096];
        ::getpwuid_r(::getuid(), &pw, buf, sizeof(buf), &result);
        home = result ? result->pw_dir : "/tmp";
    }
    return std::filesystem::path(home) / ".config" / "thewatcher" / "TheWatcherAgent.conf";
#endif
}

AgentConfig AgentConfig::load_or_create(const std::filesystem::path& path)
{
    LOG_FUNCTION_TRACE
    LOGF_DEBUG("Loading agent config from %s", path.string().c_str());
    thewatcher::crypto::init();

    if (std::filesystem::exists(path))
    {
        LOG_DEBUG("Agent config exists");
        AgentConfig cfg;
        cfg.config_path = path.string();
        LOG_DEBUG("Loading key-value agent config");
        apply_key_value(cfg, read_key_value_file(path));
        if (cfg.agent_id.empty() || cfg.agent_public_key.empty() || cfg.agent_secret_key.empty())
        {
            LOG_INFO("Agent config is missing identity fields; generating stable identity values");
            auto kp = thewatcher::crypto::generate_curve_keypair();
            if (cfg.agent_id.empty())
                cfg.agent_id = generate_uuid();
            if (cfg.agent_public_key.empty())
                cfg.agent_public_key = kp.public_key_z85;
            if (cfg.agent_secret_key.empty())
                cfg.agent_secret_key = kp.secret_key_z85;
            cfg.save(path);
        }
        LOGF_DEBUG(
            "Loaded agent config id=%s server=%s enrollment=%s interval=%d heartbeat_interval=%d process_limit=%d",
            cfg.agent_id.c_str(), cfg.server_address.c_str(), cfg.enrollment_address.c_str(), cfg.collection_interval,
            cfg.heartbeat_interval, cfg.process_limit);
        return cfg;
    }

    // First run: generate new agent identity
    LOG_INFO("Agent config does not exist; creating a new config with generated identity");
    AgentConfig cfg;
    cfg.config_path = path.string();
    cfg.agent_id = generate_uuid();
    auto kp = thewatcher::crypto::generate_curve_keypair();
    cfg.agent_public_key = kp.public_key_z85;
    cfg.agent_secret_key = kp.secret_key_z85;

    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    cfg.save(path);
    LOGF_DEBUG("Created agent config id=%s path=%s", cfg.agent_id.c_str(), path.string().c_str());
    return cfg;
}

// Applies the non-empty command-line overrides for this process and keeps the
// file values they replace, so save() does not persist them (issue #150).
void AgentConfig::apply_cli_overrides(const std::string& server, const std::string& enrollment,
                                      const std::string& server_key)
{
    // The server key pin belongs to the file's server. Any override can make
    // this process enroll with (and learn the key of) a different server, so
    // the file's key and fingerprint are kept for save() as well.
    if ((!server.empty() || !enrollment.empty() || !server_key.empty()) && !file_server_public_key)
    {
        file_server_public_key = server_public_key;
        file_server_public_key_fingerprint = server_public_key_fingerprint;
    }
    if (!server.empty())
    {
        if (!file_server_address)
            file_server_address = server_address;
        server_address = server;
    }
    if (!enrollment.empty())
    {
        if (!file_enrollment_address)
            file_enrollment_address = enrollment_address;
        enrollment_address = enrollment;
    }
    if (!server_key.empty())
    {
        // The file's fingerprint pins the file's key, not this one. Clear the
        // active fingerprint so enrollment checks the reply against the
        // override key alone (and learns the matching fingerprint); the file's
        // value is kept above for save().
        server_public_key = server_key;
        server_public_key_fingerprint.clear();
    }
}

// Writes the config as KEY=VALUE lines, creating missing parent directories.
// The file is replaced atomically through an owner-only temporary file (it
// holds the CURVE secret key), so a failed write never truncates the old
// config. Throws std::runtime_error on a value containing CR/LF (issue #148)
// or on any write or rename failure (issue #149).
void AgentConfig::save(const std::filesystem::path& path) const
{
    LOG_FUNCTION_TRACE
    LOGF_DEBUG("Saving agent config to %s", path.string().c_str());
    // Issue #150: command-line overrides are not persisted.
    const auto& saved_server_address = file_server_address.value_or(server_address);
    const auto& saved_enrollment_address = file_enrollment_address.value_or(enrollment_address);
    const auto& saved_server_key = file_server_public_key.value_or(server_public_key);
    const auto& saved_fingerprint = file_server_public_key_fingerprint.value_or(server_public_key_fingerprint);

    require_single_line("SERVER_ADDRESS", saved_server_address);
    require_single_line("ENROLLMENT_ADDRESS", saved_enrollment_address);
    require_single_line("SERVER_PUBLIC_KEY", saved_server_key);
    require_single_line("SERVER_PUBLIC_KEY_FINGERPRINT", saved_fingerprint);
    require_single_line("ENROLLMENT_TOKEN", enrollment_token);
    require_single_line("AGENT_ID", agent_id);
    require_single_line("AGENT_PUBLIC_KEY", agent_public_key);
    require_single_line("AGENT_SECRET_KEY", agent_secret_key);

    std::ostringstream f;
    if (const auto host = server_host_from_endpoint(saved_server_address); !host.empty())
        f << "THEWATCHER_SERVER=" << host << '\n';
    f << "SERVER_ADDRESS=" << saved_server_address << '\n';
    f << "ENROLLMENT_ADDRESS=" << saved_enrollment_address << '\n';
    f << "SERVER_PUBLIC_KEY=" << saved_server_key << '\n';
    f << "SERVER_PUBLIC_KEY_FINGERPRINT=" << saved_fingerprint << '\n';
    if (allow_insecure)
        f << "ALLOW_INSECURE=1" << '\n';
    if (!enrollment_token.empty())
        f << "ENROLLMENT_TOKEN=" << enrollment_token << '\n';
    f << "AGENT_ID=" << agent_id << '\n';
    f << "AGENT_PUBLIC_KEY=" << agent_public_key << '\n';
    f << "AGENT_SECRET_KEY=" << agent_secret_key << '\n';
    f << "COLLECTION_INTERVAL=" << collection_interval << '\n';
    f << "HEARTBEAT_INTERVAL=" << heartbeat_interval << '\n';
    f << "PROCESS_LIMIT=" << process_limit << '\n';
    f << "LOG_LEVEL=" << thewatcher::logging::LogSettings::level_name(log.level) << '\n';
    f << "LOG_MAX_SIZE_MB=" << log.max_size_mib << '\n';
    f << "LOG_MAX_FILES=" << log.max_files << '\n';

    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    thewatcher::common::write_private_file_atomic(path, f.str());
}

} // namespace thewatcher::agent
