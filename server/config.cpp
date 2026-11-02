#include "config.hpp"

#include "common/SingleLog.hpp"
#include "common/crypto.hpp"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

// JSON support intentionally absent: TheWatcherServer.conf is KEY=VALUE only.

namespace thewatcher::server
{

namespace fs = std::filesystem;

namespace
{
    constexpr std::uintmax_t max_config_file_bytes = 1024 * 1024;
    constexpr std::size_t max_config_line_bytes = 8 * 1024;

    std::string trim(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    std::unordered_map<std::string, std::string> read_key_value_file(const fs::path& path)
    {
        std::error_code ec;
        const auto file_size = fs::file_size(path, ec);
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

    void apply_key_value(ServerConfig& cfg, const std::unordered_map<std::string, std::string>& values)
    {
        if (auto it = values.find("BIND_ADDRESS"); it != values.end())
            cfg.bind_address = it->second;
        if (auto it = values.find("ENROLLMENT_ADDRESS"); it != values.end())
            cfg.enrollment_address = it->second;
        if (auto it = values.find("API_HOST"); it != values.end())
            cfg.api_host = it->second;
        if (auto it = values.find("API_PORT"); it != values.end())
            cfg.api_port = std::stoi(it->second);
        if (auto it = values.find("DB_PATH"); it != values.end())
            cfg.db_path = it->second;
        if (auto it = values.find("DB_TYPE"); it != values.end())
            cfg.db_type = it->second;
        if (auto it = values.find("POSTGRES_DSN"); it != values.end())
            cfg.postgres_dsn = it->second;
        if (auto it = values.find("OFFLINE_AFTER_SECONDS"); it != values.end())
            cfg.offline_after_seconds = std::stoi(it->second);
        if (auto it = values.find("SERVER_PUBLIC_KEY"); it != values.end())
            cfg.server_public_key = it->second;
        if (auto it = values.find("SERVER_SECRET_KEY"); it != values.end())
            cfg.server_secret_key = it->second;
        if (auto it = values.find("ENROLLMENT_TOKEN"); it != values.end())
            cfg.enrollment_token = it->second;
    }

    std::string resolve_sqlite_db_path(const fs::path& config_path, const std::string& db_path)
    {
        fs::path raw = db_path.empty() ? fs::path("thewatcher.db") : fs::path(db_path);
        if (raw.is_absolute())
            return raw.lexically_normal().string();

        const auto base = config_path.has_parent_path() ? config_path.parent_path() : fs::current_path();
        return (base / raw).lexically_normal().string();
    }

    void resolve_runtime_paths(ServerConfig& cfg, const fs::path& config_path)
    {
        if (cfg.db_type == "sqlite")
            cfg.db_path = resolve_sqlite_db_path(config_path, cfg.db_path);
    }
} // namespace

fs::path ServerConfig::default_path()
{
    LOG_FUNCTION_TRACE
#ifdef _WIN32
    char buf[MAX_PATH];
    SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, buf);
    return fs::path(buf) / "TheWatcher" / "TheWatcherServer.conf";
#else
    return fs::path("/etc/thewatcher/TheWatcherServer.conf");
#endif
}

ServerConfig ServerConfig::load_or_create(const fs::path& path)
{
    LOG_FUNCTION_TRACE
    LOGF_DEBUG("Loading server config from %s", path.string().c_str());
    if (fs::exists(path))
    {
        LOG_DEBUG("Server config exists; loading KEY=VALUE");
        ServerConfig cfg;
        apply_key_value(cfg, read_key_value_file(path));
        resolve_runtime_paths(cfg, path);
#ifndef _WIN32
        {
            // SEC-15: the file holds the server CURVE secret and possibly
            // the Postgres DSN — warn loudly when other users can read it.
            std::error_code ec;
            const auto perms = fs::status(path, ec).permissions();
            if (!ec && (perms & (fs::perms::group_read | fs::perms::others_read)) != fs::perms::none)
            {
                LOG_WARNING("Server config is readable by group/others; it contains the server CURVE secret. "
                            "Run: chmod 600 on the config file");
            }
        }
#endif
        LOGF_DEBUG("Loaded server config bind=%s enrollment=%s api=%s:%d db_type=%s db_path=%s offline_after=%d",
                   cfg.bind_address.c_str(), cfg.enrollment_address.c_str(), cfg.api_host.c_str(), cfg.api_port,
                   cfg.db_type.c_str(), cfg.db_path.c_str(), cfg.offline_after_seconds);
        return cfg;
    }

    LOG_INFO("Server config does not exist; creating a new config with generated CURVE keys");
    ServerConfig cfg;
    thewatcher::crypto::init();
    auto kp = thewatcher::crypto::generate_curve_keypair();
    cfg.server_public_key = kp.public_key_z85;
    cfg.server_secret_key = kp.secret_key_z85;
    resolve_runtime_paths(cfg, path);

    if (path.has_parent_path())
        fs::create_directories(path.parent_path());
    cfg.save(path);
    return cfg;
}

void ServerConfig::save(const fs::path& path) const
{
    LOG_FUNCTION_TRACE
    LOGF_DEBUG("Saving server config to %s", path.string().c_str());
    if (path.has_parent_path())
    {
        std::error_code dir_ec;
        fs::create_directories(path.parent_path(), dir_ec);
#ifndef _WIN32
        // SEC-15: the directory holds SERVER_SECRET_KEY and the Postgres DSN;
        // newly created directories are restricted to the owner. (On Windows
        // the installer grants only SYSTEM/Administrators on the config dir.)
        fs::permissions(path.parent_path(), fs::perms::owner_all, fs::perm_options::replace, dir_ec);
#endif
    }

    // SEC-15: write to a sibling temp file and rename over the target so a
    // crash mid-write can never truncate an existing config, and apply
    // owner-only permissions BEFORE the file ever holds secrets — a plain
    // ofstream would create the file world-readable until a later chmod.
    const fs::path tmp = fs::path(path.string() + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            throw std::runtime_error("Cannot write " + tmp.string());
        f << "BIND_ADDRESS=" << bind_address << '\n';
        f << "ENROLLMENT_ADDRESS=" << enrollment_address << '\n';
        f << "API_HOST=" << api_host << '\n';
        f << "API_PORT=" << api_port << '\n';
        f << "DB_PATH=" << db_path << '\n';
        f << "DB_TYPE=" << db_type << '\n';
        f << "POSTGRES_DSN=" << postgres_dsn << '\n';
        f << "OFFLINE_AFTER_SECONDS=" << offline_after_seconds << '\n';
        f << "SERVER_PUBLIC_KEY=" << server_public_key << '\n';
        f << "SERVER_SECRET_KEY=" << server_secret_key << '\n';
        if (!enrollment_token.empty())
            f << "ENROLLMENT_TOKEN=" << enrollment_token << '\n';
        f.flush();
        if (!f)
        {
            std::error_code cleanup_ec;
            fs::remove(tmp, cleanup_ec);
            throw std::runtime_error("Failed writing " + tmp.string());
        }
    }

#ifndef _WIN32
    {
        std::error_code perm_ec;
        fs::permissions(tmp, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, perm_ec);
        if (perm_ec)
        {
            std::error_code cleanup_ec;
            fs::remove(tmp, cleanup_ec);
            throw std::runtime_error("Cannot restrict permissions on " + tmp.string());
        }
    }
#endif

    std::error_code ec;
#ifdef _WIN32
    // std::filesystem::rename on Windows cannot replace an existing file.
    fs::remove(path, ec);
#endif
    fs::rename(tmp, path, ec);
    if (ec)
    {
        std::error_code cleanup_ec;
        fs::remove(tmp, cleanup_ec);
        throw std::runtime_error("Cannot replace " + path.string());
    }
}

} // namespace thewatcher::server
