// Startup ordering of TheWatcherServer: --uninstall-service needs no config, so
// it must run before the config directory is validated. A machine with a loose
// legacy config directory (or none) must still be able to remove the service,
// while a normal start and --install-service keep refusing such a directory.
//
// Runs the real server executable (path in THEWATCHER_SERVER_EXE, set by Meson).
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace
{
std::string read_all(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// Path of the server executable from THEWATCHER_SERVER_EXE (set by Meson).
std::string server_executable()
{
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    REQUIRE(_dupenv_s(&value, &length, "THEWATCHER_SERVER_EXE") == 0);
    REQUIRE(value != nullptr);
    const std::string exe = value;
    std::free(value);
    return exe;
#else
    const char* value = std::getenv("THEWATCHER_SERVER_EXE");
    REQUIRE(value != nullptr);
    return value;
#endif
}

// Runs the server with `arguments`, merging stderr into stdout, and returns what it printed.
//
// The exit status is not asserted on: the scenarios judge what the server printed,
// and removing or installing a service this test never created may fail for reasons
// of its own. It is attached to the next assertion so a failure shows it. (glibc
// marks system() warn_unused_result under _FORTIFY_SOURCE, so it must not be
// dropped silently either.)
std::string run_server(const std::string& arguments, const fs::path& output)
{
    const std::string exe = server_executable();
    std::ostringstream command;
    command << '"' << exe << "\" " << arguments << " > \"" << output.string() << "\" 2>&1";
#ifdef _WIN32
    // cmd.exe /c strips the outer quotes.
    const int status = std::system(("\"" + command.str() + "\"").c_str());
#else
    const int status = std::system(command.str().c_str());
#endif
    UNSCOPED_INFO("server exit status for \"" << arguments << "\": " << status);
    return read_all(output);
}

// A directory that other users can write to.
fs::path make_loose_directory(const fs::path& dir)
{
    fs::create_directories(dir);
#ifdef _WIN32
    const std::string command = "icacls \"" + dir.string() + "\" /grant *S-1-5-32-545:(OI)(CI)W > NUL";
    REQUIRE(std::system(("\"" + command + "\"").c_str()) == 0);
#else
    fs::permissions(dir, fs::perms::all, fs::perm_options::replace); // 0777
#endif
    return dir;
}
} // namespace

SCENARIO("--uninstall-service works when the config directory is not private")
{
    GIVEN("a config path inside a directory that other users can write to")
    {
        // The default scratch base (the build tree) is enough here: every refusal
        // of the private-directory check, for the directory itself or for one of
        // its ancestors, says "is not private", and these scenarios only ask
        // whether the check ran.
        const thewatcher::test::ScopedTempDir scratch("thewatcher-uninstall-order");
        const auto loose = make_loose_directory(scratch.path() / "loose");
        const auto config = loose / "TheWatcherServer.conf";
        const auto output = scratch.path() / "output.txt";
        const std::string config_argument = "--config \"" + config.string() + "\"";

        WHEN("the server is run with --uninstall-service")
        {
            const auto text =
                run_server("--uninstall-service --service-name thewatcher-order-probe " + config_argument, output);

            THEN("it is not stopped by the private-directory check")
            {
                REQUIRE(text.find("is not private") == std::string::npos);
            }

            AND_THEN("it creates nothing in the config directory")
            {
                REQUIRE_FALSE(fs::exists(config));
            }
        }

        WHEN("the server is run with --install-service")
        {
            const auto text =
                run_server("--install-service --service-name thewatcher-order-probe " + config_argument, output);

            THEN("it still refuses the directory")
            {
                REQUIRE(text.find("is not private") != std::string::npos);
            }
        }
    }
}
