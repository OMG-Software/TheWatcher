// Small networking helpers (free local ports/endpoints) shared by the server,
// agent and integration tests. Also re-exports the timing helpers.
#pragma once

#include "test_support/timing.hpp"

#include <string>

#include <zmq.hpp>

namespace thewatcher::test
{

// An OS-assigned free "tcp://127.0.0.1:<port>" endpoint. Asking the OS rather
// than deriving a port from the clock avoids collisions when test
// executables run in parallel.
inline std::string free_zmq_endpoint()
{
    zmq::context_t ctx{1};
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    return endpoint;
}

// An OS-assigned free TCP port on 127.0.0.1.
inline int free_http_port()
{
    const auto endpoint = free_zmq_endpoint();
    return std::stoi(endpoint.substr(endpoint.rfind(':') + 1));
}

} // namespace thewatcher::test
