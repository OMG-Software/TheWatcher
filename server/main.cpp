#include "common/SingleLog.hpp"
#include "common/crypto.hpp"
#include "common/signal_guard.hpp"
#include "common/stop_relay.hpp"
#include "common/windows_service.hpp"
#include "config.hpp"
#include "server.hpp"
#include "startup_signal.hpp"

#include <atomic>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
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
              << "  --log-level <l>   Log file level: trace, debug, info, notice, warning, error,\n"
              << "                    critical or off (overrides LOG_LEVEL in the config)\n"
              << "  --genkey          Print a new CURVE keypair and exit\n"
              << "  --service         Run under Windows Service Control Manager\n"
              << "  --service-name <n> Service name (default: TheWatcherServer)\n"
              << "  --install-service Install the Windows service and exit\n"
              << "  --uninstall-service Remove the Windows service and exit\n"
              << "  --help            Show this help\n";
}

int main(int argc, char* argv[])
{
    namespace fs = std::filesystem;
    using namespace thewatcher::server;

    // First, before any thread exists, so every later thread inherits the
    // blocked SIGINT/SIGTERM mask (issue #84).
    thewatcher::process::SignalGuard signals;

    std::string config_path;
    std::string cli_log_level;
    std::string service_name = "TheWatcherServer";
    bool do_genkey = false;
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

    fs::path cfg_path = config_path.empty() ? ServerConfig::default_path() : fs::path(config_path);
    auto log_path = log_path_for_config(cfg_path, "TheWatcherServer.log");
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

        return thewatcher::service::install_windows_service({
            service_name,
            "TheWatcher Server",
            "TheWatcher monitoring server",
            thewatcher::service::build_service_command_line(service_args),
        });
    }

    if (uninstall_service)
    {
        return thewatcher::service::uninstall_windows_service(service_name);
    }
    LOG_FUNCTION_TRACE
    LOG_INFO("Starting TheWatcher Server");
    LOGF_INFO("Config: %s", cfg_path.string().c_str());
    LOGF_INFO("Log: %s", log_path.string().c_str());
    LOGF_DEBUG("Service mode=%d install=%d uninstall=%d", service_mode ? 1 : 0, install_service ? 1 : 0,
               uninstall_service ? 1 : 0);

    ServerConfig config;
    try
    {
        config = ServerConfig::load_or_create(cfg_path);
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

    LOGF_INFO("Public key: %s", config.server_public_key.c_str());
    LOGF_DEBUG("Server bind=%s enrollment=%s api=%s:%d db_type=%s db_path=%s offline_after_seconds=%d",
               config.bind_address.c_str(), config.enrollment_address.c_str(), config.api_host.c_str(), config.api_port,
               config.db_type.c_str(), config.db_path.c_str(), config.offline_after_seconds);

    try
    {
        if (service_mode)
        {
            LOGF_INFO("Starting Windows service mode as %s", service_name.c_str());
            // The SCM calls the stop callback on its own thread, possibly before
            // the worker has built the server; the relay keeps that request.
            thewatcher::process::StopRelay stop_relay;
            return thewatcher::service::run_windows_service(
                service_name,
                [&] {
                    LOG_DEBUG("Service worker constructing server");
                    Server service_server(std::move(config));
                    // Detached again before service_server is destroyed.
                    const auto attachment = stop_relay.attach_scoped([&] {
                        service_server.stop();
                    });
                    LOG_DEBUG("Service worker running server");
                    service_server.run();
                },
                [&] {
                    LOG_DEBUG("Service stop callback received");
                    stop_relay.request_stop();
                });
        }

        Server server(std::move(config));
        StartupSignal startup;
        std::exception_ptr server_error;
        std::mutex server_error_mutex;
        std::atomic_bool server_failed{false};

        // Run the server on a background thread so the main thread can poll
        // for the stop signal without blocking.
        std::thread srv_thread([&] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                {
                    std::lock_guard lock(server_error_mutex);
                    server_error = std::current_exception();
                }
                startup.fail(server_error);
                server_failed.store(true);
            }
        });

        auto startup_result = startup.wait();
        if (!startup_result.started)
        {
            if (srv_thread.joinable())
                srv_thread.join();
            LOGF_CRITICAL("Startup failed: %s", startup_result.message.c_str());
            return 1;
        }

        LOG_INFO("Running. Press Ctrl+C to stop.");
        while (!server_failed.load() && !signals.wait_for(std::chrono::milliseconds(200)))
        {
        }

        if (server_failed.load())
        {
            std::exception_ptr error;
            {
                std::lock_guard lock(server_error_mutex);
                error = server_error;
            }
            if (error)
            {
                try
                {
                    std::rethrow_exception(error);
                }
                catch (const std::exception& e)
                {
                    LOGF_CRITICAL("Stopped after worker error: %s", e.what());
                }
                catch (...)
                {
                    LOG_CRITICAL("Stopped after unknown worker error");
                }
            }
        }

        LOG_INFO("Shutting down...");
        server.stop();
        srv_thread.join();
        // A worker that died is a failure, whatever the supervisor does next.
        if (server_failed.load())
            return 1;
    }
    catch (const std::exception& e)
    {
        LOGF_CRITICAL("Fatal: %s", e.what());
        return 1;
    }

    return 0;
}
