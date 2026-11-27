#include "agent.hpp"
#include "common/SingleLog.hpp"
#include "common/crypto.hpp"
#include "common/signal_guard.hpp"
#include "common/stop_relay.hpp"
#include "common/windows_service.hpp"
#include "config.hpp"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::filesystem::path log_path_for_config(const std::filesystem::path& config_path, const char* filename)
{
    auto directory = config_path.has_parent_path() ? config_path.parent_path() : std::filesystem::current_path();
    return directory / filename;
}

} // namespace

static void print_usage(const char* prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "Options:\n"
              << "  --config <path>   Config file path (default: platform-specific)\n"
              << "  --server <addr>   Server ZMQ address (e.g. tcp://host:5555)\n"
              << "  --enrollment <a>  Enrollment ZMQ address\n"
              << "  --server-key <k>  Server CURVE public key (40-char z85)\n"
              << "  --insecure        Allow enrollment replies without a server key pin\n"
              << "                    (data plane runs unencrypted; use only on\n"
              << "                    trusted networks)\n"
              << "  --log-level <l>   Log file level: trace, debug, info, notice, warning, error,\n"
              << "                    critical or off (overrides LOG_LEVEL in the config)\n"
              << "  --genkey          Generate a CURVE keypair and print it, then exit\n"
              << "  --service         Run under Windows Service Control Manager\n"
              << "  --service-name <n> Service name (default: TheWatcherAgent)\n"
              << "  --install-service Install the Windows service and exit\n"
              << "  --uninstall-service Remove the Windows service and exit\n"
              << "  --help            Show this help\n";
}

// Agent entry point. Parses CLI options, loads or creates the config, and runs
// the agent until a signal or service stop. Returns 0 on clean exit and 1 on
// an unknown argument, config load failure or fatal error. --insecure sets
// allow_insecure_cli for this process only (SEC-08).
int main(int argc, char* argv[])
{
    namespace fs = std::filesystem;
    using namespace thewatcher::agent;

    // First, before any thread exists, so every later thread inherits the
    // blocked SIGINT/SIGTERM mask (issue #84).
    thewatcher::process::SignalGuard signals;

    std::string config_path;
    std::string override_server;
    std::string override_enrollment;
    std::string override_server_key;
    std::string cli_log_level;
    std::string service_name = "TheWatcherAgent";
    bool do_genkey = false;
    bool allow_insecure = false;
    bool service_mode = false;
    bool install_service = false;
    bool uninstall_service = false;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            print_usage(argv[0]);
            return 0;
        }
        else if (arg == "--genkey")
        {
            do_genkey = true;
        }
        else if (arg == "--service")
        {
            service_mode = true;
        }
        else if (arg == "--install-service")
        {
            install_service = true;
        }
        else if (arg == "--uninstall-service")
        {
            uninstall_service = true;
        }
        else if (arg == "--service-name" && i + 1 < argc)
        {
            service_name = argv[++i];
        }
        else if (arg == "--log-level" && i + 1 < argc)
        {
            cli_log_level = argv[++i];
        }
        else if (arg == "--config" && i + 1 < argc)
        {
            config_path = argv[++i];
        }
        else if (arg == "--server" && i + 1 < argc)
        {
            override_server = argv[++i];
        }
        else if (arg == "--enrollment" && i + 1 < argc)
        {
            override_enrollment = argv[++i];
        }
        else if (arg == "--server-key" && i + 1 < argc)
        {
            override_server_key = argv[++i];
        }
        else if (arg == "--insecure")
        {
            allow_insecure = true;
        }
        else
        {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (do_genkey)
    {
        thewatcher::crypto::init();
        auto kp = thewatcher::crypto::generate_curve_keypair();
        std::cout << "public_key: " << kp.public_key_z85 << "\n"
                  << "secret_key: " << kp.secret_key_z85 << "\n";
        return 0;
    }

    fs::path cfg_path = config_path.empty() ? AgentConfig::default_path() : fs::path(config_path);
    auto log_path = log_path_for_config(cfg_path, "TheWatcherAgent.log");
    auto& logger = thewatcher::logging::SingleLog::GetInstance();
    logger.SetConsoleLogLevel(thewatcher::logging::LogLevel::L_INFO);
    // Defaults until the config is read; --log-level also applies from here on.
    thewatcher::logging::LogSettings log_settings;
    try
    {
        if (!cli_log_level.empty())
            log_settings.set_level(cli_log_level);
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << "\n";
        return 1;
    }
    log_settings.apply(logger);
    logger.SetLogFilePath(log_path.string());

    if (install_service)
    {
        std::vector<std::string> service_args{
            thewatcher::service::current_executable_path(),
            "--service",
            "--service-name",
            service_name,
        };
        if (!config_path.empty())
        {
            service_args.push_back("--config");
            service_args.push_back(config_path);
        }
        if (!cli_log_level.empty())
        {
            service_args.push_back("--log-level");
            service_args.push_back(cli_log_level);
        }
        if (!override_server.empty())
        {
            service_args.push_back("--server");
            service_args.push_back(override_server);
        }
        if (!override_enrollment.empty())
        {
            service_args.push_back("--enrollment");
            service_args.push_back(override_enrollment);
        }
        if (!override_server_key.empty())
        {
            service_args.push_back("--server-key");
            service_args.push_back(override_server_key);
        }
        if (allow_insecure)
        {
            service_args.push_back("--insecure");
        }

        return thewatcher::service::install_windows_service({
            service_name,
            "TheWatcher Agent",
            "TheWatcher monitoring agent",
            thewatcher::service::build_service_command_line(service_args),
        });
    }

    if (uninstall_service)
    {
        return thewatcher::service::uninstall_windows_service(service_name);
    }

    LOG_FUNCTION_TRACE
    LOG_INFO("Starting TheWatcher Agent");
    LOGF_INFO("Config: %s", cfg_path.string().c_str());
    LOGF_INFO("Log: %s", log_path.string().c_str());
    LOGF_DEBUG("Service mode=%d install=%d uninstall=%d", service_mode ? 1 : 0, install_service ? 1 : 0,
               uninstall_service ? 1 : 0);

    AgentConfig config;
    try
    {
        config = AgentConfig::load_or_create(cfg_path);
    }
    catch (const std::exception& e)
    {
        LOGF_CRITICAL("Failed to load config: %s", e.what());
        return 1;
    }

    // The config's log settings take effect now; --log-level still wins.
    log_settings = config.log;
    if (!cli_log_level.empty())
        log_settings.set_level(cli_log_level);
    log_settings.apply(logger);

    // Issue #150: overrides apply to this process only and are not saved.
    if (!override_server.empty())
        LOGF_DEBUG("Overriding server address from CLI: %s", override_server.c_str());
    if (!override_enrollment.empty())
        LOGF_DEBUG("Overriding enrollment address from CLI: %s", override_enrollment.c_str());
    if (!override_server_key.empty())
        LOG_DEBUG("Overriding server public key from CLI");
    config.apply_cli_overrides(override_server, override_enrollment, override_server_key);
    if (allow_insecure)
    {
        // SEC-08: explicit operator opt-in to run without CURVE; without this
        // flag an enrollment reply missing a server key pin is a hard error.
        LOG_WARNING("Insecure mode enabled: the agent may connect without CURVE encryption");
        config.allow_insecure_cli = true;
    }

    LOGF_INFO("Agent ID: %s", config.agent_id.c_str());
    LOGF_INFO("Public key: %s", config.agent_public_key.c_str());
    LOGF_DEBUG("Server address: %s", config.server_address.c_str());
    LOGF_DEBUG("Enrollment address: %s", config.enrollment_address.c_str());
    LOGF_DEBUG("Initial collection interval=%d heartbeat_interval=%d process_limit=%d", config.collection_interval,
               config.heartbeat_interval, config.process_limit);

    try
    {
        if (service_mode)
        {
            LOGF_INFO("Starting Windows service mode as %s", service_name.c_str());
            // The SCM calls the stop callback on its own thread, possibly while
            // the worker is still building the agent; the relay keeps that request.
            thewatcher::process::StopRelay stop_relay;
            return thewatcher::service::run_windows_service(
                service_name,
                [&] {
                    LOG_DEBUG("Service worker constructing agent");
                    Agent service_agent(std::move(config));
                    // Detached again before service_agent is destroyed.
                    const auto attachment = stop_relay.attach_scoped([&] {
                        service_agent.stop();
                    });
                    LOG_DEBUG("Service worker starting agent");
                    service_agent.start();
                    while (!stop_relay.stop_requested())
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    LOG_DEBUG("Service worker stopping agent");
                    service_agent.stop();
                },
                [&] {
                    LOG_DEBUG("Service stop callback received");
                    stop_relay.request_stop();
                });
        }

        Agent agent(std::move(config));
        LOG_DEBUG("Starting foreground agent");
        std::jthread signal_watchdog([&agent, &signals](std::stop_token st) {
            while (!st.stop_requested() && !signals.wait_for(std::chrono::milliseconds(100)))
            {
            }
            if (signals.stop_requested())
                agent.stop();
        });
        agent.start();

        LOG_INFO("Running. Press Ctrl+C to stop.");
        while (!signals.wait_for(std::chrono::milliseconds(200)))
        {
        }

        LOG_INFO("Shutting down...");
        signal_watchdog.request_stop();
        agent.stop();
    }
    catch (const std::exception& e)
    {
        LOGF_CRITICAL("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
