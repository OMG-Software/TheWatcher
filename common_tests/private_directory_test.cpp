// ensure_private_directory: the directory that holds a secret (the server
// config with the CURVE secret key) must not be writable by other users, or a
// local user could plant or swap the file the server then trusts.
#include "common/secure_file.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

using thewatcher::common::ensure_private_directory;
namespace fs = std::filesystem;

namespace
{
fs::path fresh_parent(const char* name)
{
    auto dir = thewatcher::test::process_temp_dir().path() / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

#ifdef _WIN32
// Grants the Users group (S-1-5-32-545) write access, inherited by files and subfolders.
void grant_users_write(const fs::path& dir)
{
    const std::string command = "icacls \"" + dir.string() + "\" /grant *S-1-5-32-545:(OI)(CI)W > NUL";
    // The outer quotes are stripped by cmd.exe /c.
    REQUIRE(std::system(("\"" + command + "\"").c_str()) == 0);
}
#else
void set_mode(const fs::path& dir, fs::perms mode)
{
    fs::permissions(dir, mode, fs::perm_options::replace);
}
#endif
} // namespace

SCENARIO("A missing directory is created private and then passes the check")
{
    GIVEN("a path whose last directory does not exist")
    {
        const auto dir = fresh_parent("private-dir-missing") / "TheWatcher";

        WHEN("ensure_private_directory is called")
        {
            REQUIRE_NOTHROW(ensure_private_directory(dir));

            THEN("the directory exists and a second call accepts it")
            {
                REQUIRE(fs::is_directory(dir));
                REQUIRE_NOTHROW(ensure_private_directory(dir));
            }

            AND_THEN("a file can still be created in it by this user")
            {
                std::ofstream file(dir / "probe.txt");
                REQUIRE(file.is_open());
            }
        }
    }

    GIVEN("a path with several missing levels")
    {
        const auto dir = fresh_parent("private-dir-nested") / "a" / "b" / "c";

        THEN("every level is created and the result passes the check")
        {
            REQUIRE_NOTHROW(ensure_private_directory(dir));
            REQUIRE(fs::is_directory(dir));
            REQUIRE_NOTHROW(ensure_private_directory(dir));
        }
    }
}

SCENARIO("An existing directory that other users can write to is refused, not repaired")
{
    GIVEN("a private directory that is then opened up to other users")
    {
        const auto dir = fresh_parent("private-dir-open") / "conf";
        ensure_private_directory(dir);
#ifdef _WIN32
        grant_users_write(dir);
#else
        set_mode(dir, fs::perms::owner_all | fs::perms::group_all | fs::perms::others_all); // 0777
#endif

        WHEN("ensure_private_directory is called")
        {
            THEN("it throws an error that names the directory and says how to fix it")
            {
                try
                {
                    ensure_private_directory(dir);
                    FAIL("the unsafe directory was accepted");
                }
                catch (const std::runtime_error& e)
                {
                    const std::string message = e.what();
                    REQUIRE(message.find(dir.string()) != std::string::npos);
#ifdef _WIN32
                    REQUIRE(message.find("icacls") != std::string::npos);
#else
                    REQUIRE(message.find("chmod") != std::string::npos);
#endif
                    REQUIRE(message.find("--config") != std::string::npos);
                }
            }

            AND_THEN("the permissions were not rewritten behind the caller's back")
            {
                REQUIRE_THROWS(ensure_private_directory(dir));
                REQUIRE_THROWS(ensure_private_directory(dir));
            }
        }
    }
}

#ifdef _WIN32
SCENARIO("The ACL the installer sets on the config directory passes the check")
{
    GIVEN("a directory with the installer's ACL: Administrators and SYSTEM full control, nothing inherited")
    {
        const auto dir = fresh_parent("private-dir-installer") / "TheWatcher";
        fs::create_directories(dir);
        const auto run = [](const std::string& command) {
            return std::system(("\"" + command + " > NUL\"").c_str());
        };
        REQUIRE(run("icacls \"" + dir.string() +
                    "\" /inheritance:r /remove:g *S-1-5-32-545 *S-1-5-11 *S-1-1-0 "
                    "/grant:r *S-1-5-32-544:(OI)(CI)F *S-1-5-18:(OI)(CI)F") == 0);

        THEN("it is accepted")
        {
            REQUIRE_NOTHROW(ensure_private_directory(dir));
        }

        // Give the current user access back so the test directory can be cleaned up.
        REQUIRE(run("icacls \"" + dir.string() + "\" /grant:r \"%USERNAME%\":(OI)(CI)F") == 0);
    }
}
#endif

#ifndef _WIN32
SCENARIO("A POSIX directory is private when only its owner can write")
{
    GIVEN("directories with different modes")
    {
        const auto parent = fresh_parent("private-dir-modes");
        const auto owner_only = parent / "owner-only";
        const auto readable = parent / "readable";
        const auto group_write = parent / "group-write";
        const auto other_write = parent / "other-write";
        for (const auto& dir : {owner_only, readable, group_write, other_write})
            fs::create_directories(dir);
        set_mode(owner_only, fs::perms::owner_all); // 0700
        set_mode(readable, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                               fs::perms::others_read | fs::perms::others_exec); // 0755
        set_mode(group_write, fs::perms::owner_all | fs::perms::group_write);    // 0720
        set_mode(other_write, fs::perms::owner_all | fs::perms::others_write);   // 0702

        THEN("0700 and 0755 are accepted, group or other write is refused")
        {
            REQUIRE_NOTHROW(ensure_private_directory(owner_only));
            REQUIRE_NOTHROW(ensure_private_directory(readable));
            REQUIRE_THROWS(ensure_private_directory(group_write));
            REQUIRE_THROWS(ensure_private_directory(other_write));
        }

        THEN("a newly created directory is 0700")
        {
            const auto created = parent / "created";
            ensure_private_directory(created);
            const auto perms = fs::status(created).permissions();
            REQUIRE((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
        }
    }
}
#endif
