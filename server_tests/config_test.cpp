#include "../server/config.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace thewatcher::server;

namespace
{
std::filesystem::path unique_temp_dir(const char* name)
{
    auto dir = thewatcher::test::process_temp_dir().path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

SCENARIO("Server config default path uses TheWatcherServer.conf")
{
    GIVEN("the platform default server config path")
    {
        auto path = ServerConfig::default_path();

        THEN("the file extension is .conf, not .json")
        {
            REQUIRE(path.extension().string() == ".conf");
        }
    }
}

SCENARIO("A first-run server config writes a KEY=VALUE file with generated CURVE keys")
{
    GIVEN("a config path in a clean directory")
    {
        auto dir = unique_temp_dir("server-config-first-run");
        auto path = dir / "server.conf";

        WHEN("the server config is created")
        {
            auto cfg = ServerConfig::load_or_create(path);

            THEN("the db_path is absolute and points under the config directory")
            {
                REQUIRE(std::filesystem::path(cfg.db_path).is_absolute());
                REQUIRE(std::filesystem::path(cfg.db_path) == dir / "thewatcher.db");
            }

            AND_THEN("a CURVE keypair is generated and persisted")
            {
                REQUIRE(cfg.server_public_key.size() == 40);
                REQUIRE(cfg.server_secret_key.size() == 40);
            }

            AND_THEN("the persisted file is KEY=VALUE format, not JSON")
            {
                auto content = read_file(path);
                REQUIRE_FALSE(content.empty());
                // First non-whitespace character must not be '{' (which would indicate a JSON object).
                // We can't reject '{' anywhere in the file because z85-encoded CURVE keys may contain it.
                size_t first_non_ws = content.find_first_not_of(" \t\r\n");
                REQUIRE(first_non_ws != std::string::npos);
                REQUIRE(content[first_non_ws] != '{');
                REQUIRE(content.find("DB_PATH=") != std::string::npos);
                REQUIRE(content.find("SERVER_PUBLIC_KEY=") != std::string::npos);
            }
        }
    }
}

SCENARIO("An existing relative server DB_PATH resolves beside the config file")
{
    GIVEN("an existing KEY=VALUE config with a relative SQLite database path")
    {
        auto dir = unique_temp_dir("server-config-relative-db");
        auto path = dir / "server.conf";
        {
            std::ofstream f(path);
            f << "BIND_ADDRESS=tcp://*:5555\n";
            f << "ENROLLMENT_ADDRESS=tcp://*:5556\n";
            f << "API_HOST=127.0.0.1\n";
            f << "API_PORT=8080\n";
            f << "DB_PATH=custom.db\n";
            f << "DB_TYPE=sqlite\n";
            f << "OFFLINE_AFTER_SECONDS=60\n";
        }

        WHEN("the server config is loaded")
        {
            auto cfg = ServerConfig::load_or_create(path);

            THEN("the runtime database path is resolved relative to the config directory")
            {
                REQUIRE(std::filesystem::path(cfg.db_path).is_absolute());
                REQUIRE(std::filesystem::path(cfg.db_path) == dir / "custom.db");
            }
        }
    }
}

SCENARIO("An existing absolute server DB_PATH is preserved")
{
    GIVEN("an existing KEY=VALUE config with an absolute SQLite database path")
    {
        auto dir = unique_temp_dir("server-config-absolute-db");
        auto path = dir / "server.conf";
        auto db_path = dir / "absolute.db";
        {
            std::ofstream f(path);
            f << "BIND_ADDRESS=tcp://*:5555\n";
            f << "ENROLLMENT_ADDRESS=tcp://*:5556\n";
            f << "API_HOST=127.0.0.1\n";
            f << "API_PORT=8080\n";
            f << "DB_PATH=" << db_path.string() << "\n";
            f << "DB_TYPE=sqlite\n";
            f << "OFFLINE_AFTER_SECONDS=60\n";
        }

        WHEN("the server config is loaded")
        {
            auto cfg = ServerConfig::load_or_create(path);

            THEN("the runtime database path is unchanged")
            {
                REQUIRE(std::filesystem::path(cfg.db_path) == db_path);
            }
        }
    }
}

SCENARIO("Server config respects all KEY overrides and falls back to defaults")
{
    GIVEN("a KEY=VALUE config that overrides API_PORT and OFFLINE_AFTER_SECONDS only")
    {
        auto dir = unique_temp_dir("server-config-partial");
        auto path = dir / "server.conf";
        {
            std::ofstream f(path);
            f << "# A comment line that should be ignored\n";
            f << "API_PORT=9090\n";
            f << "OFFLINE_AFTER_SECONDS=120\n";
        }

        WHEN("the server config is loaded")
        {
            auto cfg = ServerConfig::load_or_create(path);

            THEN("overridden values take effect")
            {
                REQUIRE(cfg.api_port == 9090);
                REQUIRE(cfg.offline_after_seconds == 120);
            }

            AND_THEN("missing keys keep their default values")
            {
                REQUIRE(cfg.bind_address == "tcp://*:5555");
                REQUIRE(cfg.enrollment_address == "tcp://*:5556");
                REQUIRE(cfg.api_host == "0.0.0.0");
                REQUIRE(cfg.db_type == "sqlite");
            }
        }
    }
}

SCENARIO("Server config round-trips through save and reload")
{
    GIVEN("a populated ServerConfig saved to disk")
    {
        auto dir = unique_temp_dir("server-config-roundtrip");
        auto path = dir / "server.conf";

        ServerConfig original = ServerConfig::load_or_create(path);
        original.bind_address = "tcp://*:6000";
        original.enrollment_address = "tcp://*:6001";
        original.api_host = "10.0.0.5";
        original.api_port = 7777;
        original.db_type = "sqlite";
        original.offline_after_seconds = 90;
        original.save(path);

        WHEN("the config is reloaded")
        {
            auto reloaded = ServerConfig::load_or_create(path);

            THEN("every persisted value matches")
            {
                REQUIRE(reloaded.bind_address == original.bind_address);
                REQUIRE(reloaded.enrollment_address == original.enrollment_address);
                REQUIRE(reloaded.api_host == original.api_host);
                REQUIRE(reloaded.api_port == original.api_port);
                REQUIRE(reloaded.db_type == original.db_type);
                REQUIRE(reloaded.offline_after_seconds == original.offline_after_seconds);
                REQUIRE(reloaded.server_public_key == original.server_public_key);
                REQUIRE(reloaded.server_secret_key == original.server_secret_key);
            }
        }
    }
}

SCENARIO("Server config TRUSTED_PROXIES round-trips and is parsed leniently")
{
    GIVEN("a config with no TRUSTED_PROXIES key")
    {
        auto dir = unique_temp_dir("server-config-proxies-default");
        auto path = dir / "server.conf";
        {
            std::ofstream f(path);
            f << "API_PORT=8080\n";
        }

        THEN("no proxies are trusted")
        {
            REQUIRE(ServerConfig::load_or_create(path).trusted_proxies.empty());
        }
    }

    GIVEN("a config whose TRUSTED_PROXIES has spaces and empty entries")
    {
        auto dir = unique_temp_dir("server-config-proxies-parse");
        auto path = dir / "server.conf";
        {
            std::ofstream f(path);
            f << "TRUSTED_PROXIES= 192.0.2.1 ,, 198.51.100.5,\t,::1 ,\n";
        }

        WHEN("the config is loaded")
        {
            auto cfg = ServerConfig::load_or_create(path);

            THEN("entries are trimmed and empty ones skipped")
            {
                REQUIRE(cfg.trusted_proxies == std::vector<std::string>{"192.0.2.1", "198.51.100.5", "::1"});
            }
        }
    }

    GIVEN("a ServerConfig with trusted proxies saved to disk")
    {
        auto dir = unique_temp_dir("server-config-proxies-roundtrip");
        auto path = dir / "server.conf";

        ServerConfig original = ServerConfig::load_or_create(path);
        original.trusted_proxies = {"127.0.0.1", "203.0.113.3"};
        original.save(path);

        WHEN("the config is reloaded")
        {
            auto reloaded = ServerConfig::load_or_create(path);

            THEN("the proxy list is preserved in order")
            {
                REQUIRE(reloaded.trusted_proxies == original.trusted_proxies);
                REQUIRE(read_file(path).find("TRUSTED_PROXIES=127.0.0.1,203.0.113.3\n") != std::string::npos);
            }
        }

        AND_WHEN("the list is cleared and saved again")
        {
            original.trusted_proxies.clear();
            original.save(path);

            THEN("the key is omitted and reload yields an empty list")
            {
                REQUIRE(read_file(path).find("TRUSTED_PROXIES") == std::string::npos);
                REQUIRE(ServerConfig::load_or_create(path).trusted_proxies.empty());
            }
        }
    }
}

SCENARIO("Server config rejects oversized KEY=VALUE config input")
{
    GIVEN("an existing server config with a line above the supported size")
    {
        auto dir = unique_temp_dir("server-config-too-long-line");
        auto path = dir / "server.conf";
        {
            std::ofstream f(path);
            f << std::string(9 * 1024, 'A') << "\n";
        }

        WHEN("the config is loaded")
        {
            THEN("the parser rejects the line before applying settings")
            {
                REQUIRE_THROWS(ServerConfig::load_or_create(path));
            }
        }
    }

    GIVEN("an existing server config file above the supported size")
    {
        auto dir = unique_temp_dir("server-config-too-large");
        auto path = dir / "server.conf";
        {
            std::ofstream f(path);
            f << std::string((1024 * 1024) + 1, 'A');
        }

        WHEN("the config is loaded")
        {
            THEN("the parser rejects the file before reading settings")
            {
                REQUIRE_THROWS(ServerConfig::load_or_create(path));
            }
        }
    }
}

namespace
{
// Writes a config file made of the given lines and returns its path.
std::filesystem::path write_config(const char* dir_name, const std::vector<std::string>& lines)
{
    auto dir = unique_temp_dir(dir_name);
    auto path = dir / "server.conf";
    std::ofstream f(path);
    for (const auto& line : lines)
        f << line << '\n';
    return path;
}

// Returns the message of the exception thrown by loading path, or "" if none.
std::string load_error(const std::filesystem::path& path)
{
    try
    {
        (void)ServerConfig::load_or_create(path);
    }
    catch (const std::exception& e)
    {
        return e.what();
    }
    return {};
}
} // namespace

SCENARIO("Server config reports a non-numeric number with its key and line")
{
    GIVEN("a config whose API_PORT is not a number on line 2")
    {
        const auto path = write_config("server-config-bad-port", {"# comment", "API_PORT=abc"});

        WHEN("the config is loaded")
        {
            THEN("startup fails with an error naming the key and the line")
            {
                REQUIRE(load_error(path) == "API_PORT: not a number (line 2)");
            }
        }
    }

    GIVEN("a config whose OFFLINE_AFTER_SECONDS has trailing garbage")
    {
        const auto path = write_config("server-config-bad-offline", {"OFFLINE_AFTER_SECONDS=60s"});

        THEN("the value is rejected rather than read as 60")
        {
            REQUIRE(load_error(path) == "OFFLINE_AFTER_SECONDS: not a number (line 1)");
        }
    }
}

SCENARIO("Server config range-checks API_PORT and OFFLINE_AFTER_SECONDS")
{
    GIVEN("API_PORT values outside 1-65535")
    {
        THEN("zero, negative and oversized ports are rejected with the key and line")
        {
            REQUIRE(load_error(write_config("server-config-port-0", {"API_PORT=0"})) ==
                    "API_PORT: must be between 1 and 65535 (line 1)");
            REQUIRE(load_error(write_config("server-config-port-neg", {"API_PORT=-1"})) ==
                    "API_PORT: must be between 1 and 65535 (line 1)");
            REQUIRE(load_error(write_config("server-config-port-big", {"API_PORT=70000"})) ==
                    "API_PORT: must be between 1 and 65535 (line 1)");
            REQUIRE(load_error(write_config("server-config-port-huge", {"API_PORT=99999999999999999999"})) ==
                    "API_PORT: must be between 1 and 65535 (line 1)");
        }
    }

    GIVEN("API_PORT=65535")
    {
        THEN("the boundary value is accepted")
        {
            REQUIRE(ServerConfig::load_or_create(write_config("server-config-port-max", {"API_PORT=65535"})).api_port ==
                    65535);
        }
    }

    GIVEN("a negative OFFLINE_AFTER_SECONDS")
    {
        THEN("it is rejected")
        {
            REQUIRE(load_error(write_config("server-config-offline-neg", {"OFFLINE_AFTER_SECONDS=-5"})) ==
                    "OFFLINE_AFTER_SECONDS: must be between 0 and 604800 (line 1)");
        }
    }

    GIVEN("OFFLINE_AFTER_SECONDS=0, the documented way to disable offline detection")
    {
        THEN("it is accepted")
        {
            REQUIRE(ServerConfig::load_or_create(write_config("server-config-offline-0", {"OFFLINE_AFTER_SECONDS=0"}))
                        .offline_after_seconds == 0);
        }
    }
}

SCENARIO("Server config warns about unknown keys without refusing to start")
{
    GIVEN("a config with a mistyped OFFLINE_AFTER_SECONDS key")
    {
        const auto path = write_config("server-config-unknown-key", {"OFFLNE_AFTER_SECONDS=30", "API_PORT=9090"});

        WHEN("the config is loaded")
        {
            const auto cfg = ServerConfig::load_or_create(path);

            THEN("the server still starts and the unknown key is reported")
            {
                REQUIRE(cfg.unknown_keys == std::vector<std::string>{"OFFLNE_AFTER_SECONDS"});
            }

            AND_THEN("known keys are applied and the mistyped one is not")
            {
                REQUIRE(cfg.api_port == 9090);
                REQUIRE(cfg.offline_after_seconds == 60);
            }
        }
    }

    GIVEN("a config with only known keys")
    {
        THEN("no unknown keys are reported")
        {
            REQUIRE(ServerConfig::load_or_create(write_config("server-config-known-keys", {"API_PORT=9090"}))
                        .unknown_keys.empty());
        }
    }
}

SCENARIO("Server config validates DB_TYPE and the CURVE keypair")
{
    GIVEN("an unsupported DB_TYPE")
    {
        THEN("startup fails naming the key and line")
        {
            REQUIRE(load_error(write_config("server-config-db-type", {"DB_TYPE=mysql"})) ==
                    "DB_TYPE: must be 'sqlite' or 'postgres' (line 1)");
        }
    }

    GIVEN("DB_TYPE=postgres without a POSTGRES_DSN")
    {
        THEN("startup fails because there is nothing to connect to")
        {
            REQUIRE(load_error(write_config("server-config-pg-no-dsn", {"DB_TYPE=postgres"})) ==
                    "POSTGRES_DSN: required when DB_TYPE=postgres");
        }
    }

    GIVEN("a generated keypair")
    {
        auto dir = unique_temp_dir("server-config-keys");
        const auto good = ServerConfig::load_or_create(dir / "good.conf");
        const auto other = ServerConfig::load_or_create(dir / "other.conf");

        THEN("only the public key set is rejected")
        {
            const auto path =
                write_config("server-config-only-public", {"SERVER_PUBLIC_KEY=" + good.server_public_key});
            REQUIRE(load_error(path) == "SERVER_PUBLIC_KEY and SERVER_SECRET_KEY must be set together");
        }

        THEN("only the secret key set is rejected")
        {
            const auto path =
                write_config("server-config-only-secret", {"SERVER_SECRET_KEY=" + good.server_secret_key});
            REQUIRE(load_error(path) == "SERVER_PUBLIC_KEY and SERVER_SECRET_KEY must be set together");
        }

        THEN("a public key that does not belong to the secret key is rejected")
        {
            const auto path = write_config("server-config-mismatch", {"SERVER_PUBLIC_KEY=" + other.server_public_key,
                                                                      "SERVER_SECRET_KEY=" + good.server_secret_key});
            REQUIRE(load_error(path) == "SERVER_PUBLIC_KEY does not match SERVER_SECRET_KEY");
        }

        THEN("a key that is not 40 z85 characters is rejected")
        {
            const auto path = write_config("server-config-short-key",
                                           {"SERVER_PUBLIC_KEY=short", "SERVER_SECRET_KEY=" + good.server_secret_key});
            REQUIRE(load_error(path) == "SERVER_PUBLIC_KEY: must be a 40 character z85 key (line 1)");
        }

        THEN("a matching pair is accepted")
        {
            const auto path = write_config("server-config-match", {"SERVER_PUBLIC_KEY=" + good.server_public_key,
                                                                   "SERVER_SECRET_KEY=" + good.server_secret_key});
            REQUIRE(load_error(path).empty());
        }
    }
}
