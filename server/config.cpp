#include "config.hpp"

#include "common/SingleLog.hpp"
#include "common/crypto.hpp"
#include "common/secure_file.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string_view>
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

    // One KEY=VALUE line: trimmed key and value plus its 1-based line number,
    // so a validation error can point at the line to fix.
    struct ConfigEntry
    {
        std::string key;
        std::string value;
        std::size_t line = 0;
    };

    // Parses KEY=VALUE lines in file order. Keys and values are trimmed; blank
    // lines, '#' comments and lines without '=' are skipped. Throws
    // std::runtime_error if the file is over 1 MiB, a line is over 8 KiB, or it
    // cannot be opened.
    std::vector<ConfigEntry> read_key_value_file(const fs::path& path)
    {
        std::error_code ec;
        const auto file_size = fs::file_size(path, ec);
        if (!ec && file_size > max_config_file_bytes)
            throw std::runtime_error("Config file is too large: " + path.string());

        std::ifstream f(path);
        if (!f)
            throw std::runtime_error("Cannot open " + path.string());

        std::vector<ConfigEntry> entries;
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(f, line))
        {
            ++line_number;
            if (line.size() > max_config_line_bytes)
                throw std::runtime_error("Config line is too long: " + path.string());
            line = trim(line);
            if (line.empty() || line[0] == '#')
                continue;
            const auto pos = line.find('=');
            if (pos == std::string::npos)
                continue;
            entries.push_back({trim(line.substr(0, pos)), trim(line.substr(pos + 1)), line_number});
        }
        return entries;
    }

    // Error shared by every per-key validation: "KEY: reason (line N)".
    std::invalid_argument entry_error(const ConfigEntry& entry, const std::string& reason)
    {
        return std::invalid_argument(entry.key + ": " + reason + " (line " + std::to_string(entry.line) + ")");
    }

    // Parses entry.value as a base-10 integer in [min, max]. Rejects empty
    // input, trailing characters and out-of-range values, naming key and line.
    int parse_int_entry(const ConfigEntry& entry, int min, int max)
    {
        long long parsed = 0;
        const auto* first = entry.value.data();
        const auto* last = first + entry.value.size();
        const auto [ptr, ec] = std::from_chars(first, last, parsed);
        if (ec == std::errc::result_out_of_range)
            throw entry_error(entry, "must be between " + std::to_string(min) + " and " + std::to_string(max));
        if (entry.value.empty() || ec != std::errc{} || ptr != last)
            throw entry_error(entry, "not a number");
        if (parsed < min || parsed > max)
            throw entry_error(entry, "must be between " + std::to_string(min) + " and " + std::to_string(max));
        return static_cast<int>(parsed);
    }

    // A z85 CURVE key is exactly 40 characters from the z85 alphabet.
    bool is_z85_key(const std::string& key)
    {
        if (key.size() != 40)
            return false;
        const std::string_view alphabet(thewatcher::crypto::z85_alphabet);
        return std::all_of(key.begin(), key.end(), [&](char c) {
            return alphabet.find(c) != std::string_view::npos;
        });
    }

    // True when public_z85 is the public half of secret_z85 (Curve25519 base-point multiplication).
    bool keypair_matches(const std::string& public_z85, const std::string& secret_z85)
    {
        thewatcher::crypto::init();
        const auto secret = thewatcher::crypto::z85_decode(secret_z85);
        std::array<unsigned char, 32> derived{};
        if (crypto_scalarmult_base(derived.data(), secret.data()) != 0)
            return false;
        return thewatcher::crypto::z85_encode(derived) == public_z85;
    }

    // Copies recognised keys onto cfg and validates them. Absent keys keep their
    // defaults; the last occurrence of a repeated key wins. Unknown keys are
    // recorded in cfg.unknown_keys and logged, not rejected: a config written
    // by a newer release must still load after a downgrade. Throws
    // std::invalid_argument naming the key and line for a bad value, or the
    // keys involved for an inconsistent combination.
    void apply_key_value(ServerConfig& cfg, const std::vector<ConfigEntry>& entries)
    {
        // Copies of the key lines, kept for error messages (no pointers to dereference).
        ConfigEntry public_key_entry{"SERVER_PUBLIC_KEY", "", 0};
        ConfigEntry secret_key_entry{"SERVER_SECRET_KEY", "", 0};

        // The last occurrence of a key wins, so only that one is validated: an invalid
        // value that a later line overrides is ignored, and errors name the final line.
        std::map<std::string, std::size_t> last_index;
        for (std::size_t i = 0; i < entries.size(); ++i)
            last_index[entries[i].key] = i;

        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto& entry = entries[i];
            const auto& key = entry.key;
            if (last_index[key] != i)
                continue;
            if (key == "BIND_ADDRESS")
                cfg.bind_address = entry.value;
            else if (key == "ENROLLMENT_ADDRESS")
                cfg.enrollment_address = entry.value;
            else if (key == "API_HOST")
                cfg.api_host = entry.value;
            else if (key == "API_PORT")
                cfg.api_port = parse_int_entry(entry, 1, 65535);
            else if (key == "DB_PATH")
                cfg.db_path = entry.value;
            else if (key == "DB_TYPE")
            {
                if (entry.value != "sqlite" && entry.value != "postgres")
                    throw entry_error(entry, "must be 'sqlite' or 'postgres'");
                cfg.db_type = entry.value;
            }
            else if (key == "POSTGRES_DSN")
                cfg.postgres_dsn = entry.value;
            else if (key == "OFFLINE_AFTER_SECONDS")
                // 0 is the documented way to turn offline detection off.
                cfg.offline_after_seconds = parse_int_entry(entry, 0, 7 * 24 * 60 * 60);
            else if (key == "SERVER_PUBLIC_KEY")
            {
                cfg.server_public_key = entry.value;
                public_key_entry = entry;
            }
            else if (key == "SERVER_SECRET_KEY")
            {
                cfg.server_secret_key = entry.value;
                secret_key_entry = entry;
            }
            else if (key == "ENROLLMENT_TOKEN")
                cfg.enrollment_token = entry.value;
            else if (key == "TRUSTED_PROXIES")
                cfg.trusted_proxies = split_csv(entry.value);
            else if (std::find(cfg.unknown_keys.begin(), cfg.unknown_keys.end(), key) == cfg.unknown_keys.end())
            {
                cfg.unknown_keys.push_back(key);
                LOGF_WARNING("Ignoring unknown server config key %s (line %zu); check it for a typo", key.c_str(),
                             entry.line);
            }
        }

        if (cfg.db_type == "postgres" && cfg.postgres_dsn.empty())
            throw std::invalid_argument("POSTGRES_DSN: required when DB_TYPE=postgres");

        // Both keys empty means CURVE is off; one without the other would
        // silently run without encryption.
        if (cfg.server_public_key.empty() != cfg.server_secret_key.empty())
            throw std::invalid_argument("SERVER_PUBLIC_KEY and SERVER_SECRET_KEY must be set together");
        if (!cfg.server_public_key.empty())
        {
            if (!is_z85_key(cfg.server_public_key))
                throw entry_error(public_key_entry, "must be a 40 character z85 key");
            if (!is_z85_key(cfg.server_secret_key))
                throw entry_error(secret_key_entry, "must be a 40 character z85 key");
            if (!keypair_matches(cfg.server_public_key, cfg.server_secret_key))
                throw std::invalid_argument("SERVER_PUBLIC_KEY does not match SERVER_SECRET_KEY");
        }
        if (cfg.offline_after_seconds == 0)
            LOG_WARNING("OFFLINE_AFTER_SECONDS=0: offline detection is disabled; dead agents stay marked connected");
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

fs::path ServerConfig::existing_path_or_legacy(const fs::path& preferred, const fs::path& legacy)
{
    std::error_code ec;
    const bool preferred_exists = fs::exists(preferred, ec);
    if (ec)
        throw std::runtime_error("Cannot inspect " + preferred.string() + ": " + ec.message() +
                                 "; pass --config <path>");
    if (preferred_exists)
        return preferred;
    // A legacy file that cannot be inspected is treated as absent.
    std::error_code legacy_ec;
    if (fs::exists(legacy, legacy_ec) && !legacy_ec)
        return legacy;
    return preferred;
}

fs::path ServerConfig::default_path()
{
    LOG_FUNCTION_TRACE
#ifdef _WIN32
    char buf[MAX_PATH] = {};
    // Fail closed: guessing a folder (an environment variable or a fixed
    // C:\ProgramData) could point somewhere other users can write, and this
    // file holds the CURVE secret key. The caller can pass --config instead.
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, buf)) || buf[0] == '\0')
        throw std::runtime_error("Cannot determine the common application data folder; pass --config <path>");
    return fs::path(buf) / "TheWatcher" / "TheWatcherServer.conf";
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
    // BSD convention is /usr/local/etc for add-on software. An existing config
    // under the old /etc/thewatcher location keeps being used so upgrades do
    // not lose their settings.
    const fs::path bsd_path("/usr/local/etc/thewatcher/TheWatcherServer.conf");
    const fs::path legacy_path("/etc/thewatcher/TheWatcherServer.conf");
    return existing_path_or_legacy(bsd_path, legacy_path);
#else
    return fs::path("/etc/thewatcher/TheWatcherServer.conf");
#endif
}

// Loads the KEY=VALUE config at path, or creates it with a new CURVE keypair
// when missing. Throws std::runtime_error if the file cannot be read or written,
// and std::invalid_argument ("KEY: reason (line N)") for an invalid value.
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
