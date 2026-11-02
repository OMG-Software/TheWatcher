// Server config file permission tests (SEC-15).
//
// ServerConfig::save writes SERVER_SECRET_KEY and the Postgres DSN; the file
// must be owner-only and a failed or interrupted save must never leave the
// previous config truncated. Permission assertions are POSIX-only — on
// Windows the config directory ACL is the control point (see SEC-45).
#include "../server/config.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace thewatcher::server;

namespace
{

std::filesystem::path temp_config_path(const char* name)
{
    const auto dir = std::filesystem::temp_directory_path() / "thewatcher-server-config-perm-tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::filesystem::remove(path);
    return path;
}

std::string read_all(const std::filesystem::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

} // namespace

SCENARIO("ServerConfig::save writes an owner-only config file")
{
    GIVEN("a fresh config path")
    {
        auto path = temp_config_path("perm-test.conf");

        WHEN("a config holding a server secret is saved")
        {
            ServerConfig cfg;
            cfg.server_secret_key = "secret-key-material";
            cfg.save(path);

#ifdef _WIN32
            SUCCEED("POSIX permission assertions do not apply; the installer-managed directory ACL covers Windows");
#else
            THEN("the file exists with owner-only permissions")
            {
                REQUIRE(std::filesystem::exists(path));
                const auto perms = std::filesystem::status(path).permissions();
                REQUIRE((perms & std::filesystem::perms::owner_rw) == std::filesystem::perms::owner_rw);
                REQUIRE((perms & std::filesystem::perms::group_all) == std::filesystem::perms::none);
                REQUIRE((perms & std::filesystem::perms::others_all) == std::filesystem::perms::none);
            }

            AND_THEN("no temp file is left behind")
            {
                REQUIRE_FALSE(std::filesystem::exists(path.string() + ".tmp"));
            }
#endif
        }
    }
}

SCENARIO("A failed save leaves the previous config intact")
{
#ifdef _WIN32
    SUCCEED("POSIX-only failure injection; not runnable on Windows");
#else
    GIVEN("an existing config file and a save that cannot complete")
    {
        auto path = temp_config_path("crash-test.conf");

        ServerConfig original;
        original.api_port = 12345;
        original.save(path);
        REQUIRE(read_all(path).find("API_PORT=12345") != std::string::npos);

        // Make the directory read-only so the temp-file write inside save()
        // fails after the target file already exists.
        std::filesystem::permissions(path.parent_path(),
                                     std::filesystem::perms::owner_all & ~std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace);

        WHEN("save fails")
        {
            bool threw = false;
            try
            {
                ServerConfig broken;
                broken.api_port = 99999;
                broken.save(path);
            }
            catch (const std::exception&)
            {
                threw = true;
            }

            THEN("the previous config is untouched")
            {
                std::filesystem::permissions(path.parent_path(), std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace);
                REQUIRE(threw);
                REQUIRE(read_all(path).find("API_PORT=12345") != std::string::npos);
                REQUIRE(read_all(path).find("API_PORT=99999") == std::string::npos);
            }
        }
    }
#endif
}