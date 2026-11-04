#include "../agent/config.hpp"
#include "../agent/enrollment.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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
