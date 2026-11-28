// Runs a thewatcher Server on a worker thread for tests, capturing any
// startup/run failure so the test thread can assert on it.
#pragma once

#include "server/server.hpp"
#include "server/startup_signal.hpp"
#include "test_support/net_helpers.hpp"

#include <exception>
#include <thread>
#include <utility>

namespace thewatcher::test
{

// Minimal server configuration for transport-level tests: OS-assigned local
// ZMQ ports, no CURVE keys and an in-memory store. The REST API is not
// disabled: Server always starts it, and port 0 makes it bind an OS-assigned
// port. The OS-assigned ports are held by the server from the moment it binds
// them, so no other process or outgoing connection can take them first; a
// ServerRunner copies the actual endpoints into its `config` once it starts.
inline thewatcher::server::ServerConfig make_in_memory_server_config()
{
    thewatcher::server::ServerConfig config;
    config.bind_address = "tcp://127.0.0.1:*";
    config.enrollment_address = "tcp://127.0.0.1:*";
    config.api_host = "127.0.0.1";
    config.api_port = 0;
    config.db_path = ":memory:";
    config.server_public_key.clear();
    config.server_secret_key.clear();
    return config;
}

class ServerRunner
{
public:
    // Constructs the Server, starts run() on a worker thread and waits for the
    // startup result (StartupSignal::wait() is a latch, so `startup.wait()`
    // still returns it afterwards). When startup succeeded, `config` holds the
    // endpoints and API port the server actually bound, so a config asking for
    // "tcp://127.0.0.1:*" or API port 0 can be read back through it. When
    // startup failed, `config` is unchanged and `error` is set.
    explicit ServerRunner(thewatcher::server::ServerConfig cfg)
        : config(std::move(cfg))
        , server(config)
    {
        thread = std::thread([this] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                error = std::current_exception();
                startup.fail(error);
            }
        });

        if (startup.wait().started)
        {
            // Written by the server thread before startup.succeed(), which the
            // wait() above synchronises with.
            config.bind_address = server.router_endpoint();
            config.enrollment_address = server.enrollment_endpoint();
            config.api_port = server.api_port();
        }
    }

    // Stops the server and joins the worker thread if it is still running.
    ~ServerRunner()
    {
        if (thread.joinable())
            stop();
    }

    ServerRunner(const ServerRunner&) = delete;
    ServerRunner& operator=(const ServerRunner&) = delete;

    // Asks the server to stop and joins the worker thread.
    void stop()
    {
        server.stop();
        join();
    }

    // Joins the worker thread without asking the server to stop (for servers
    // whose run() already failed).
    void join()
    {
        if (thread.joinable())
            thread.join();
    }

    thewatcher::server::ServerConfig config;
    thewatcher::server::Server server;
    thewatcher::server::StartupSignal startup;
    std::exception_ptr error;
    std::thread thread;
};

} // namespace thewatcher::test
