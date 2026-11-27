#include "config.hpp"

#include "common/SingleLog.hpp"
#include "common/crypto.hpp"
#include "common/secure_file.hpp"

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// windows.h first: sddl.h and shlobj.h depend on its declarations.
#include <sddl.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
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

    // Returns value without leading and trailing whitespace.
    std::string trim(std::string value)
    {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
            value.erase(value.begin());
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    // Comma-separated list: entries are trimmed and empty entries are skipped.
    std::vector<std::string> split_csv(const std::string& value)
    {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (start <= value.size())
        {
            auto end = value.find(',', start);
            if (end == std::string::npos)
                end = value.size();
            auto entry = trim(value.substr(start, end - start));
            if (!entry.empty())
                out.push_back(std::move(entry));
            start = end + 1;
        }
        return out;
    }

    // Parses KEY=VALUE lines into a map. Keys and values are trimmed; blank lines,
    // '#' comments and lines without '=' are skipped. Throws std::runtime_error
    // if the file is over 1 MiB, a line is over 8 KiB, or it cannot be opened.
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

    // Copies recognised keys onto cfg; absent keys keep their defaults. Throws
    // std::invalid_argument or std::out_of_range if API_PORT or
    // OFFLINE_AFTER_SECONDS is not a valid integer.
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
        if (auto it = values.find("TRUSTED_PROXIES"); it != values.end())
            cfg.trusted_proxies = split_csv(it->second);
    }

    // Returns a normalised sqlite path. An empty db_path means "thewatcher.db".
    // A relative path is anchored at the config file's directory, or at the
    // current directory when the config path has no parent.
    std::string resolve_sqlite_db_path(const fs::path& config_path, const std::string& db_path)
    {
        fs::path raw = db_path.empty() ? fs::path("thewatcher.db") : fs::path(db_path);
        if (raw.is_absolute())
            return raw.lexically_normal().string();

        const auto base = config_path.has_parent_path() ? config_path.parent_path() : fs::current_path();
        return (base / raw).lexically_normal().string();
    }

    // Rewrites cfg.db_path to its resolved form when db_type is "sqlite".
    // Other database types are left unchanged.
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

// Loads the KEY=VALUE config at path, or creates it with a new CURVE keypair
// when missing. Throws std::runtime_error if the file cannot be read or written,
// and std::invalid_argument or std::out_of_range for a non-numeric port or interval.
// On POSIX it warns when group or others can read an existing file.
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

    // save() creates (and, on POSIX, owner-restricts) any missing parent directory.
    cfg.save(path);
    return cfg;
}

// Writes the config atomically: an owner-only temp file is renamed over the
// target. Creates missing parent directories and limits those it created to the
// owner on POSIX. Throws std::runtime_error on an empty filename or any
// directory, write or rename failure.
void ServerConfig::save(const fs::path& path) const
{
    LOG_FUNCTION_TRACE
    LOGF_DEBUG("Saving server config to %s", path.string().c_str());
    if (path.filename().empty())
        throw std::runtime_error("Invalid server config path: " + path.string());

    if (path.has_parent_path())
    {
        // Remember which ancestors do not exist yet: only directories created
        // here are restricted; a pre-existing directory (e.g. /tmp or /etc)
        // is never chmod-ed.
        std::vector<fs::path> created;
        {
            std::error_code probe_ec;
            fs::path dir = path.parent_path();
            while (!dir.empty() && !fs::exists(dir, probe_ec))
            {
                created.push_back(dir);
                const auto up = dir.parent_path();
                if (up == dir)
                    break;
                dir = up;
            }
        }

        std::error_code dir_ec;
        fs::create_directories(path.parent_path(), dir_ec);
        if (dir_ec && !fs::is_directory(path.parent_path()))
            throw std::runtime_error("Cannot create directory " + path.parent_path().string() + ": " +
                                     dir_ec.message());
#ifndef _WIN32
        // SEC-15: the directory holds SERVER_SECRET_KEY and the Postgres DSN;
        // directories created by this call are restricted to the owner. (On
        // Windows the installer grants only SYSTEM/Administrators on the
        // config dir.)
        for (const auto& dir : created)
        {
            std::error_code perm_ec;
            fs::permissions(dir, fs::perms::owner_all, fs::perm_options::replace, perm_ec);
            if (perm_ec)
                throw std::runtime_error("Cannot restrict permissions on " + dir.string() + ": " + perm_ec.message());
        }
#endif
    }

    std::ostringstream out;
    out << "BIND_ADDRESS=" << bind_address << '\n';
    out << "ENROLLMENT_ADDRESS=" << enrollment_address << '\n';
    out << "API_HOST=" << api_host << '\n';
    out << "API_PORT=" << api_port << '\n';
    out << "DB_PATH=" << db_path << '\n';
    out << "DB_TYPE=" << db_type << '\n';
    out << "POSTGRES_DSN=" << postgres_dsn << '\n';
    out << "OFFLINE_AFTER_SECONDS=" << offline_after_seconds << '\n';
    out << "SERVER_PUBLIC_KEY=" << server_public_key << '\n';
    out << "SERVER_SECRET_KEY=" << server_secret_key << '\n';
    if (!enrollment_token.empty())
        out << "ENROLLMENT_TOKEN=" << enrollment_token << '\n';
    if (!trusted_proxies.empty())
    {
        out << "TRUSTED_PROXIES=";
        for (std::size_t i = 0; i < trusted_proxies.size(); ++i)
            out << (i ? "," : "") << trusted_proxies[i];
        out << '\n';
    }

    // SEC-15: write to a uniquely named, owner-only sibling temp file (created
    // exclusively, never world-readable) and rename it over the target so a
    // crash mid-write can never truncate an existing config.
    thewatcher::common::write_private_file_atomic(path, out.str());
}

} // namespace thewatcher::server
