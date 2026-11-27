#include "../agent/config.hpp"
#include "../agent/enrollment.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
// windows.h first: aclapi.h and sddl.h depend on its declarations.
#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using namespace thewatcher::agent;

// ── AgentConfig create-on-first-run ───────────────────────────────────────────

SCENARIO("AgentConfig is auto-created when no config file exists")
{
    GIVEN("a path to a config file that does not exist")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "thewatcher_test_agent.json";
        fs::remove(path); // ensure clean state

        WHEN("load_or_create is called")
        {
            auto cfg = AgentConfig::load_or_create(path);

            THEN("the returned config has a non-empty UUID agent_id")
            {
                REQUIRE_FALSE(cfg.agent_id.empty());
                // UUID format: 8-4-4-4-12 = 36 chars including dashes
                REQUIRE(cfg.agent_id.size() == 36);
                REQUIRE(cfg.agent_id[8] == '-');
                REQUIRE(cfg.agent_id[13] == '-');
                REQUIRE(cfg.agent_id[18] == '-');
                REQUIRE(cfg.agent_id[23] == '-');
            }

            AND_THEN("a 40-character z85 keypair is generated")
            {
                REQUIRE(cfg.agent_public_key.size() == 40);
                REQUIRE(cfg.agent_secret_key.size() == 40);
            }

            AND_THEN("the config file is written to disk")
            {
                REQUIRE(fs::exists(path));
            }

            // Clean up
            fs::remove(path);
        }
    }
}

// ── AgentConfig round-trip save / load ────────────────────────────────────────

SCENARIO("AgentConfig default path uses TheWatcherAgent.conf")
{
    GIVEN("the platform default agent config path")
    {
        auto path = AgentConfig::default_path();

        THEN("the file name is TheWatcherAgent.conf")
        {
            REQUIRE(path.filename().string() == "TheWatcherAgent.conf");
        }
    }
}

SCENARIO("AgentConfig derives enrollment and data endpoints from THEWATCHER_SERVER")
{
    GIVEN("a TheWatcherAgent.conf file with a server host")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "TheWatcherAgent-test.conf";
        fs::remove(path);
        {
            std::ofstream f(path);
            f << "THEWATCHER_SERVER=monitor.local\n";
            f << "AGENT_ID=agent-conf\n";
            f << "AGENT_PUBLIC_KEY=1234567890123456789012345678901234567890\n";
            f << "AGENT_SECRET_KEY=abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN\n";
        }

        WHEN("load_or_create is called")
        {
            auto cfg = AgentConfig::load_or_create(path);

            THEN("the server and enrollment addresses use the configured host")
            {
                REQUIRE(cfg.server_address == "tcp://monitor.local:5555");
                REQUIRE(cfg.enrollment_address == "tcp://monitor.local:5556");
            }
        }

        fs::remove(path);
    }
}

SCENARIO("AgentConfig survives a save and reload cycle")
{
    GIVEN("a config written to a temp file")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "TheWatcherAgent-roundtrip.conf";
        fs::remove(path);

        AgentConfig original = AgentConfig::load_or_create(path);
        original.server_address = "tcp://192.168.1.100:5555";
        original.enrollment_address = "tcp://192.168.1.100:5556";
        original.server_public_key = "abcdefghijklmnopqrstuvwxyz0123456789ABCD";
        original.server_public_key_fingerprint = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        original.collection_interval = 15;
        original.heartbeat_interval = 7;
        original.process_limit = 50;
        original.save(path);

        WHEN("the config file is loaded again")
        {
            auto reloaded = AgentConfig::load_or_create(path);

            THEN("all fields match the saved values")
            {
                REQUIRE(reloaded.agent_id == original.agent_id);
                REQUIRE(reloaded.server_address == original.server_address);
                REQUIRE(reloaded.enrollment_address == original.enrollment_address);
                REQUIRE(reloaded.server_public_key == original.server_public_key);
                REQUIRE(reloaded.server_public_key_fingerprint == original.server_public_key_fingerprint);
                REQUIRE(reloaded.agent_public_key == original.agent_public_key);
                REQUIRE(reloaded.agent_secret_key == original.agent_secret_key);
                REQUIRE(reloaded.collection_interval == 15);
                REQUIRE(reloaded.heartbeat_interval == 7);
                REQUIRE(reloaded.process_limit == 50);
            }
        }

        // Clean up
        fs::remove(path);
    }
}

SCENARIO("The CLI insecure opt-in is never persisted but a configured ALLOW_INSECURE round-trips")
{
    GIVEN("a config with only the process-local allow_insecure_cli set")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "TheWatcherAgent-insecure-cli.conf";
        fs::remove(path);

        AgentConfig cfg = AgentConfig::load_or_create(path);
        cfg.allow_insecure_cli = true;

        WHEN("the config is saved and reloaded")
        {
            cfg.save(path);

            std::ifstream in(path);
            std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            auto reloaded = AgentConfig::load_or_create(path);

            THEN("ALLOW_INSECURE is not written and the reloaded config is secure")
            {
                REQUIRE(contents.find("ALLOW_INSECURE") == std::string::npos);
                REQUIRE_FALSE(reloaded.allow_insecure);
                REQUIRE_FALSE(reloaded.allow_insecure_cli);
            }
        }

        fs::remove(path);
    }

    GIVEN("a config with allow_insecure persisted by the operator")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "TheWatcherAgent-insecure-persisted.conf";
        fs::remove(path);

        AgentConfig cfg = AgentConfig::load_or_create(path);
        cfg.allow_insecure = true;
        cfg.save(path);

        WHEN("the config is reloaded")
        {
            auto reloaded = AgentConfig::load_or_create(path);

            THEN("allow_insecure is still set")
            {
                REQUIRE(reloaded.allow_insecure);
                REQUIRE_FALSE(reloaded.allow_insecure_cli);
            }
        }

        fs::remove(path);
    }
}

SCENARIO("Agent platform helpers report usable operating system details")
{
    GIVEN("the agent is running on the local operating system")
    {
        WHEN("the OS version is requested")
        {
            auto version = get_os_version();

            THEN("a non-empty version string is returned")
            {
                REQUIRE_FALSE(version.empty());
            }
        }
    }
}

SCENARIO("Loading an existing config file does not regenerate the agent id or keys")
{
    GIVEN("a config created on first run")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "thewatcher_test_stable.json";
        fs::remove(path);

        auto first = AgentConfig::load_or_create(path);

        WHEN("load_or_create is called again on the same path")
        {
            auto second = AgentConfig::load_or_create(path);

            THEN("the agent_id is unchanged")
            {
                REQUIRE(second.agent_id == first.agent_id);
            }

            AND_THEN("the keypair is unchanged")
            {
                REQUIRE(second.agent_public_key == first.agent_public_key);
                REQUIRE(second.agent_secret_key == first.agent_secret_key);
            }
        }

        // Clean up
        fs::remove(path);
    }
}

// ── Platform helpers ──────────────────────────────────────────────────────────

SCENARIO("AgentConfig rejects oversized KEY=VALUE config input")
{
    GIVEN("an existing agent config with a line above the supported size")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "TheWatcherAgent-too-long-line.conf";
        fs::remove(path);
        {
            std::ofstream f(path);
            f << std::string(9 * 1024, 'A') << "\n";
        }

        WHEN("the config is loaded")
        {
            THEN("the parser rejects the line before applying settings")
            {
                REQUIRE_THROWS(AgentConfig::load_or_create(path));
            }
        }

        fs::remove(path);
    }

    GIVEN("an existing agent config file above the supported size")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "TheWatcherAgent-too-large.conf";
        fs::remove(path);
        {
            std::ofstream f(path);
            f << std::string((1024 * 1024) + 1, 'A');
        }

        WHEN("the config is loaded")
        {
            THEN("the parser rejects the file before reading settings")
            {
                REQUIRE_THROWS(AgentConfig::load_or_create(path));
            }
        }

        fs::remove(path);
    }
}

SCENARIO("get_hostname returns a non-empty string")
{
    GIVEN("the current host")
    {
        WHEN("get_hostname is called")
        {
            auto hostname = get_hostname();

            THEN("it returns a non-empty string")
            {
                REQUIRE_FALSE(hostname.empty());
            }
        }
    }
}

SCENARIO("get_platform_string returns a known platform identifier")
{
    GIVEN("the current platform")
    {
        WHEN("get_platform_string is called")
        {
            auto platform = get_platform_string();

            THEN("it is one of the recognised platform strings")
            {
                bool known = platform == "linux" || platform == "windows" || platform == "freebsd" ||
                             platform == "netbsd" || platform == "openbsd" || platform == "unknown";
                REQUIRE(known);
            }
        }
    }
}

SCENARIO("get_uptime_seconds returns a positive value")
{
    GIVEN("a running system")
    {
        WHEN("get_uptime_seconds is called")
        {
            double uptime = get_uptime_seconds();

            THEN("uptime is greater than zero")
            {
                REQUIRE(uptime > 0.0);
            }
        }
    }
}

SCENARIO("Command-line overrides apply to the running process only (issue #150)")
{
    GIVEN("a saved config with a server address, enrollment address and pinned key")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "cli_override_agent.conf";
        fs::remove(path);
        auto original = AgentConfig::load_or_create(path);
        original.server_address = "tcp://prod.example:5555";
        original.enrollment_address = "tcp://prod.example:5556";
        original.server_public_key = std::string(40, 'P');
        original.server_public_key_fingerprint = "prod-fingerprint";
        original.save(path);

        WHEN("the agent runs with --server, --enrollment and --server-key and then saves its config")
        {
            auto cfg = AgentConfig::load_or_create(path);
            cfg.apply_cli_overrides("tcp://test.example:5555", "tcp://test.example:5556", std::string(40, 'T'));

            REQUIRE(cfg.server_address == "tcp://test.example:5555");
            REQUIRE(cfg.enrollment_address == "tcp://test.example:5556");
            REQUIRE(cfg.server_public_key == std::string(40, 'T'));

            // Enrollment learns the fingerprint for the overridden key.
            cfg.server_public_key_fingerprint = "test-fingerprint";
            cfg.save(path);

            THEN("the file keeps the original values")
            {
                const auto reloaded = AgentConfig::load_or_create(path);
                REQUIRE(reloaded.server_address == "tcp://prod.example:5555");
                REQUIRE(reloaded.enrollment_address == "tcp://prod.example:5556");
                REQUIRE(reloaded.server_public_key == std::string(40, 'P'));
                REQUIRE(reloaded.server_public_key_fingerprint == "prod-fingerprint");
                REQUIRE(reloaded.agent_id == original.agent_id);
            }
        }

        WHEN("the agent runs without overrides and enrollment updates the pin")
        {
            auto cfg = AgentConfig::load_or_create(path);
            cfg.apply_cli_overrides("", "", "");
            cfg.server_public_key_fingerprint = "rotated-fingerprint";
            cfg.save(path);

            THEN("the learned value is saved as before")
            {
                const auto reloaded = AgentConfig::load_or_create(path);
                REQUIRE(reloaded.server_address == "tcp://prod.example:5555");
                REQUIRE(reloaded.server_public_key_fingerprint == "rotated-fingerprint");
            }
        }
    }
}

SCENARIO("An endpoint-only override does not save the other server's key pin (issue #150)")
{
    GIVEN("a saved config with no key pinned yet")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "cli_endpoint_override_agent.conf";
        fs::remove(path);
        auto original = AgentConfig::load_or_create(path);
        original.server_address = "tcp://prod.example:5555";
        original.enrollment_address = "tcp://prod.example:5556";
        original.save(path);

        WHEN("the agent runs with --server and --enrollment only and enrollment learns that server's key")
        {
            auto cfg = AgentConfig::load_or_create(path);
            cfg.apply_cli_overrides("tcp://test.example:5555", "tcp://test.example:5556", "");
            cfg.server_public_key = std::string(40, 'T');
            cfg.server_public_key_fingerprint = "test-fingerprint";
            cfg.save(path);

            THEN("the file keeps its endpoints and no key pin, so the next normal start enrolls with prod")
            {
                const auto reloaded = AgentConfig::load_or_create(path);
                REQUIRE(reloaded.server_address == "tcp://prod.example:5555");
                REQUIRE(reloaded.enrollment_address == "tcp://prod.example:5556");
                REQUIRE(reloaded.server_public_key.empty());
                REQUIRE(reloaded.server_public_key_fingerprint.empty());
            }
        }
    }
}

// Issue #98: IPv6 server addresses.

namespace
{

// Writes a config file with the given THEWATCHER_SERVER value and an identity.
fs::path write_server_config(const std::string& name, const std::string& server_value)
{
    auto path = thewatcher::test::process_temp_dir().path() / name;
    fs::remove(path);
    std::ofstream f(path);
    f << "THEWATCHER_SERVER=" << server_value << "\n";
    f << "AGENT_ID=agent-ipv6\n";
    f << "AGENT_PUBLIC_KEY=1234567890123456789012345678901234567890\n";
    f << "AGENT_SECRET_KEY=abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN\n";
    return path;
}

std::string read_all(const fs::path& path)
{
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// Names of the files in `dir` other than `keep`.
std::vector<std::string> other_files(const fs::path& dir, const fs::path& keep)
{
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(dir))
        if (entry.path() != keep)
            names.push_back(entry.path().filename().string());
    return names;
}

} // namespace

SCENARIO("A bracketed IPv6 THEWATCHER_SERVER keeps its brackets and replaces the port (issue #98)")
{
    GIVEN("THEWATCHER_SERVER with a bracketed IPv6 address and a port")
    {
        const auto path = write_server_config("ipv6_bracketed_port.conf", "[2001:db8::1]:5555");

        WHEN("the config loads")
        {
            const auto cfg = AgentConfig::load_or_create(path);

            THEN("the endpoints are the data and enrollment ports of that address")
            {
                REQUIRE(cfg.server_address == "tcp://[2001:db8::1]:5555");
                REQUIRE(cfg.enrollment_address == "tcp://[2001:db8::1]:5556");
            }
        }
        fs::remove(path);
    }

    GIVEN("THEWATCHER_SERVER with a tcp:// scheme and a bracketed IPv6 address without a port")
    {
        const auto path = write_server_config("ipv6_bracketed_noport.conf", "tcp://[::1]");

        WHEN("the config loads")
        {
            const auto cfg = AgentConfig::load_or_create(path);

            THEN("the default ports are used")
            {
                REQUIRE(cfg.server_address == "tcp://[::1]:5555");
                REQUIRE(cfg.enrollment_address == "tcp://[::1]:5556");
            }
        }
        fs::remove(path);
    }
}

SCENARIO("An unbracketed IPv6 THEWATCHER_SERVER is rejected with a clear error (issue #98)")
{
    GIVEN("THEWATCHER_SERVER=2001:db8::1")
    {
        const auto path = write_server_config("ipv6_unbracketed.conf", "2001:db8::1");

        WHEN("the config loads")
        {
            THEN("loading fails instead of silently dropping the last hextet")
            {
                REQUIRE_THROWS_WITH(AgentConfig::load_or_create(path), Catch::Matchers::ContainsSubstring("IPv6"));
            }
        }
        fs::remove(path);
    }

    GIVEN("THEWATCHER_SERVER with a bracketed address followed by junk")
    {
        const auto path = write_server_config("ipv6_junk.conf", "[::1]x:5555");

        WHEN("the config loads")
        {
            THEN("loading fails")
            {
                REQUIRE_THROWS_AS(AgentConfig::load_or_create(path), std::invalid_argument);
            }
        }
        fs::remove(path);
    }
}

SCENARIO("An IPv6 server address survives a save and reload (issue #98)")
{
    GIVEN("a config whose server address is an IPv6 endpoint")
    {
        const auto path = write_server_config("ipv6_roundtrip.conf", "[::1]:5555");
        auto cfg = AgentConfig::load_or_create(path);

        WHEN("it is saved and loaded again")
        {
            cfg.save(path);
            const auto reloaded = AgentConfig::load_or_create(path);

            THEN("both endpoints are unchanged")
            {
                REQUIRE(reloaded.server_address == "tcp://[::1]:5555");
                REQUIRE(reloaded.enrollment_address == "tcp://[::1]:5556");
            }
        }
        fs::remove(path);
    }
}

// Issue #148: a value containing a line break must not inject config lines.

SCENARIO("Saving refuses any value that contains a line break (issue #148)")
{
    GIVEN("a saved config")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "injection_agent.conf";
        fs::remove(path);
        auto cfg = AgentConfig::load_or_create(path);
        const auto before = read_all(path);

        WHEN("the server key carries an injected SERVER_ADDRESS and ALLOW_INSECURE line")
        {
            cfg.server_public_key = std::string(40, 'a') + "\nSERVER_ADDRESS=tcp://attacker:5555\nALLOW_INSECURE=1";

            THEN("save throws and the file on disk is untouched")
            {
                REQUIRE_THROWS_AS(cfg.save(path), std::runtime_error);
                REQUIRE(read_all(path) == before);
            }
        }

        WHEN("any string field contains a carriage return")
        {
            using Field = std::string AgentConfig::*;
            const Field fields[] = {&AgentConfig::agent_id,
                                    &AgentConfig::server_address,
                                    &AgentConfig::enrollment_address,
                                    &AgentConfig::server_public_key,
                                    &AgentConfig::server_public_key_fingerprint,
                                    &AgentConfig::agent_public_key,
                                    &AgentConfig::agent_secret_key,
                                    &AgentConfig::enrollment_token};

            THEN("save throws for each of them and the file on disk is untouched")
            {
                for (const Field field : fields)
                {
                    auto bad = cfg;
                    bad.*field = "value\rx";
                    REQUIRE_THROWS_AS(bad.save(path), std::runtime_error);
                    REQUIRE(read_all(path) == before);
                }
            }
        }

        WHEN("a value kept for a command-line override contains a line break")
        {
            cfg.file_server_address = "tcp://ok:5555\nALLOW_INSECURE=1";

            THEN("save throws")
            {
                REQUIRE_THROWS_AS(cfg.save(path), std::runtime_error);
            }
        }
        fs::remove(path);
    }
}

// Issue #149: atomic save, checked errors, owner-only file.

SCENARIO("Saving replaces the config atomically and leaves no temporary files (issue #149)")
{
    GIVEN("a saved config in its own directory")
    {
        const auto dir = thewatcher::test::process_temp_dir().path() / "atomic_agent_conf";
        fs::remove_all(dir);
        const auto path = dir / "TheWatcherAgent.conf";
        auto cfg = AgentConfig::load_or_create(path);

        WHEN("it is changed and saved again")
        {
            cfg.collection_interval = 77;
            cfg.save(path);

            THEN("the new value is on disk and the directory holds only the config")
            {
                REQUIRE(AgentConfig::load_or_create(path).collection_interval == 77);
                REQUIRE(other_files(dir, path).empty());
            }
        }

        WHEN("the target cannot be replaced because it is a directory")
        {
            fs::remove(path);
            fs::create_directory(path);

            THEN("save throws and leaves no temporary file behind")
            {
                REQUIRE_THROWS_AS(cfg.save(path), std::runtime_error);
                REQUIRE(other_files(dir, path).empty());
            }
        }
        fs::remove_all(dir);
    }
}

SCENARIO("Saving reports a directory that cannot be created (issue #149)")
{
    GIVEN("a config path whose parent is an existing regular file")
    {
        const auto blocker = thewatcher::test::process_temp_dir().path() / "agent_conf_blocker";
        fs::remove_all(blocker);
        std::ofstream(blocker) << "not a directory";
        AgentConfig cfg;
        cfg.agent_id = "agent-err";

        WHEN("the config is saved")
        {
            THEN("save throws instead of failing silently")
            {
                REQUIRE_THROWS(cfg.save(blocker / "TheWatcherAgent.conf"));
            }
        }
        fs::remove(blocker);
    }
}

SCENARIO("The saved config holding the CURVE secret key is restricted to its owner (issue #149)")
{
    GIVEN("a config that was created, then saved again over the existing file")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "private_agent.conf";
        fs::remove(path);
        auto cfg = AgentConfig::load_or_create(path);
        cfg.save(path);

        WHEN("its access rights are inspected")
        {
#ifdef _WIN32
            PSECURITY_DESCRIPTOR sd = nullptr;
            const auto status = ::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                                        nullptr, nullptr, nullptr, nullptr, &sd);
            REQUIRE(status == ERROR_SUCCESS);
            LPWSTR sddl = nullptr;
            REQUIRE(::ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, SDDL_REVISION_1,
                                                                           DACL_SECURITY_INFORMATION, &sddl, nullptr));
            const std::wstring text(sddl);
            ::LocalFree(sddl);
            ::LocalFree(sd);

            THEN("the DACL is protected and grants nothing to Users, Everyone or Authenticated Users")
            {
                REQUIRE(text.rfind(L"D:P", 0) == 0);
                REQUIRE(text.find(L";;;BU)") == std::wstring::npos);
                REQUIRE(text.find(L";;;WD)") == std::wstring::npos);
                REQUIRE(text.find(L";;;AU)") == std::wstring::npos);
            }
#else
            struct stat st{};
            REQUIRE(::stat(path.c_str(), &st) == 0);

            THEN("only the owner can read and write it")
            {
                REQUIRE((st.st_mode & 0777) == 0600);
            }
#endif
        }
        fs::remove(path);
    }
}

SCENARIO("Agent config LOG_* keys control the log level and rotation (issue #99)")
{
    GIVEN("a config with no LOG_* keys")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "thewatcher_test_agent_log_default.conf";
        {
            std::ofstream f(path);
            f << "COLLECTION_INTERVAL=30\n";
        }

        THEN("logging defaults to INFO with 10 MiB files and 5 backups")
        {
            const auto cfg = AgentConfig::load_or_create(path);
            REQUIRE(cfg.log.level == thewatcher::logging::LogLevel::L_INFO);
            REQUIRE(cfg.log.max_size_mib == 10);
            REQUIRE(cfg.log.max_files == 5);
        }
        fs::remove(path);
    }

    GIVEN("a config that sets all LOG_* keys")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "thewatcher_test_agent_log_set.conf";
        {
            std::ofstream f(path);
            f << "LOG_LEVEL=warning\nLOG_MAX_SIZE_MB=0\nLOG_MAX_FILES=1\n";
        }

        WHEN("the config is loaded, saved and loaded again")
        {
            auto cfg = AgentConfig::load_or_create(path);
            cfg.save(path);
            const auto reloaded = AgentConfig::load_or_create(path);

            THEN("the values are read and survive the round trip")
            {
                REQUIRE(cfg.log.level == thewatcher::logging::LogLevel::L_WARNING);
                REQUIRE(cfg.log.max_size_mib == 0);
                REQUIRE(cfg.log.max_files == 1);
                REQUIRE(reloaded.log.level == thewatcher::logging::LogLevel::L_WARNING);
                REQUIRE(reloaded.log.max_size_mib == 0);
                REQUIRE(reloaded.log.max_files == 1);
            }
        }
        fs::remove(path);
    }

    GIVEN("a config with an invalid LOG_MAX_FILES")
    {
        auto path = thewatcher::test::process_temp_dir().path() / "thewatcher_test_agent_log_bad.conf";
        {
            std::ofstream f(path);
            f << "LOG_MAX_FILES=lots\n";
        }

        THEN("loading fails with a clear error")
        {
            REQUIRE_THROWS_AS(AgentConfig::load_or_create(path), std::invalid_argument);
        }
        fs::remove(path);
    }
}
