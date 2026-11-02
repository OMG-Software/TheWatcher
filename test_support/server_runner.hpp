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

// Minimal server configuration for transport-level tests: free local ZMQ
// endpoints, no CURVE keys, an in-memory store and the REST API disabled.
inline thewatcher::server::ServerConfig make_in_memory_server_config()
{
    thewatcher::server::ServerConfig config;
    config.bind_address = free_zmq_endpoint();
    config.enrollment_address = free_zmq_endpoint();
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
    // Constructs the Server and immediately starts run() on a worker thread;
    // wait for readiness with `startup.wait()`.
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
    }

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
