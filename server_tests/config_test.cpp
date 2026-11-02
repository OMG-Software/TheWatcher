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
