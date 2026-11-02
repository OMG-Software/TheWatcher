// Server config file permission tests (SEC-15).
//
// ServerConfig::save writes SERVER_SECRET_KEY and the Postgres DSN; the file
// must be owner-only and a failed or interrupted save must never leave the
// previous config truncated. Permission assertions are POSIX-only — on
// Windows the config directory ACL is the control point (see SEC-45).
//
// These tests also run as root (which bypasses permission enforcement), so
// they check mode BITS and use failure injection that does not depend on
// permission denial.
#include "../server/config.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace thewatcher::server;

namespace fs = std::filesystem;

namespace
{

std::string read_all(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

#ifndef _WIN32
unsigned mode_bits(const fs::path& p)
{
    return static_cast<unsigned>(fs::status(p).permissions()) & 0777u;
}
#endif

// Returns true when `dir` holds an entry whose name contains ".tmp".
bool has_temp_leftovers(const fs::path& dir)
{
    for (const auto& entry : fs::directory_iterator(dir))
    {
        if (entry.path().filename().string().find(".tmp") != std::string::npos)
            return true;
    }
    return false;
}

} // namespace

SCENARIO("ServerConfig::save writes an owner-only config file")
{
    GIVEN("a fresh config path")
    {
        thewatcher::test::ScopedTempDir scratch;
        const auto path = scratch.path() / "perm-test.conf";

        WHEN("a config holding a server secret is saved")
        {
            ServerConfig cfg;
            cfg.server_secret_key = "secret-key-material";
            cfg.save(path);

#ifdef _WIN32
            SUCCEED("POSIX permission assertions do not apply; the installer-managed directory ACL covers Windows");
#else
            THEN("the file exists with mode 0600")
            {
                REQUIRE(fs::exists(path));
                REQUIRE(mode_bits(path) == 0600u);
            }

            AND_THEN("the secret was written")
            {
                REQUIRE(read_all(path).find("SERVER_SECRET_KEY=secret-key-material") != std::string::npos);
            }
#endif
        }
    }
}

SCENARIO("A successful save leaves no temporary files behind")
{
    GIVEN("a directory with an existing config that is saved twice")
    {
        thewatcher::test::ScopedTempDir scratch;
        const auto path = scratch.path() / "server.conf";

        ServerConfig cfg;
        cfg.api_port = 1111;
        cfg.save(path);
        cfg.api_port = 2222;
        cfg.save(path);

        THEN("only the config file remains and holds the latest values")
        {
            REQUIRE_FALSE(has_temp_leftovers(scratch.path()));
            REQUIRE(read_all(path).find("API_PORT=2222") != std::string::npos);
            std::size_t entries = 0;
            for (const auto& e : fs::directory_iterator(scratch.path()))
            {
                (void)e;
                ++entries;
            }
            REQUIRE(entries == 1);
        }
    }
}

SCENARIO("ServerConfig::save does not change the mode of an existing directory")
{
#ifdef _WIN32
    SUCCEED("POSIX mode bits do not apply on Windows");
#else
    GIVEN("an existing shared-style directory with mode 0755")
    {
        thewatcher::test::ScopedTempDir scratch;
        const auto shared = scratch.path() / "shared";
        fs::create_directory(shared);
        fs::permissions(shared,
                        fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read |
                            fs::perms::others_exec,
                        fs::perm_options::replace);
        REQUIRE(mode_bits(shared) == 0755u);

        WHEN("a config is saved directly inside it")
        {
            ServerConfig cfg;
            cfg.save(shared / "server.conf");

            THEN("the directory keeps its mode and the file is still 0600")
            {
                REQUIRE(mode_bits(shared) == 0755u);
                REQUIRE(mode_bits(shared / "server.conf") == 0600u);
            }
        }
    }
#endif
}

SCENARIO("ServerConfig::save creates a missing parent directory owner-only")
{
#ifdef _WIN32
    SUCCEED("POSIX mode bits do not apply on Windows");
#else
    GIVEN("a config path whose parent directories do not exist")
    {
        thewatcher::test::ScopedTempDir scratch;
        // Widen the scratch root so we can tell it was left alone.
        fs::permissions(scratch.path(), fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec,
                        fs::perm_options::replace);
        const auto outer = scratch.path() / "etc-thewatcher";
        const auto inner = outer / "nested";
        const auto path = inner / "server.conf";

        WHEN("the config is saved")
        {
            ServerConfig cfg;
            cfg.save(path);

            THEN("every directory save() created is mode 0700")
            {
                REQUIRE(fs::is_directory(outer));
                REQUIRE(fs::is_directory(inner));
                REQUIRE(mode_bits(outer) == 0700u);
                REQUIRE(mode_bits(inner) == 0700u);
            }

            AND_THEN("the pre-existing parent directory is unchanged")
            {
                REQUIRE(mode_bits(scratch.path()) == 0750u);
            }

            AND_THEN("the file is mode 0600")
            {
                REQUIRE(mode_bits(path) == 0600u);
            }
        }
    }
#endif
}

SCENARIO("A failed save leaves the previous config intact")
{
#ifdef _WIN32
    SUCCEED("POSIX-only failure injection; not runnable on Windows");
#else
    GIVEN("a config path occupied by a non-empty directory (rename over it fails, even for root)")
    {
        thewatcher::test::ScopedTempDir scratch;
        const auto path = scratch.path() / "server.conf";
        fs::create_directory(path);
        const auto sentinel = path / "sentinel.txt";
        {
            std::ofstream f(sentinel);
            f << "previous-state";
        }

        WHEN("save fails")
        {
            bool threw = false;
            try
            {
                ServerConfig broken;
                broken.api_port = 99999;
                broken.server_secret_key = "must-not-leak";
                broken.save(path);
            }
            catch (const std::exception&)
            {
                threw = true;
            }

            THEN("it throws and the existing content is untouched")
            {
                REQUIRE(threw);
                REQUIRE(fs::is_directory(path));
                REQUIRE(read_all(sentinel) == "previous-state");
            }

            AND_THEN("no temporary files are left in the parent directory")
            {
                REQUIRE_FALSE(has_temp_leftovers(scratch.path()));
            }
        }
    }
#endif
}
