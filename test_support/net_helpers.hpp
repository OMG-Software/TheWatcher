// Small networking helpers (free local ports/endpoints) shared by the server,
// agent and integration tests. Also re-exports the timing helpers.
#pragma once

#include "test_support/timing.hpp"

#include <random>
#include <string>

#include <zmq.hpp>

namespace thewatcher::test
{

// A free "tcp://127.0.0.1:<port>" endpoint for a server under test to bind.
//
// The port is drawn at random from 20000-32767, below the OS ephemeral range
// (Linux 32768-60999, Windows/macOS 49152-65535), and checked by binding it.
// A port from the ephemeral range ("tcp://127.0.0.1:*") is released before
// the server binds it, and in that window the OS may hand it to an outgoing
// client connection, which makes the server's bind fail intermittently.
// Falls back to an OS-assigned port if no candidate can be bound.
inline std::string free_zmq_endpoint()
{
    zmq::context_t ctx{1};
    std::random_device rd; // non-deterministic source; no seeded PRNG
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const int port = 20000 + static_cast<int>(rd() % 12768U);
        const std::string endpoint = "tcp://127.0.0.1:" + std::to_string(port);
        try
        {
            zmq::socket_t probe{ctx, ZMQ_REP};
            probe.set(zmq::sockopt::linger, 0);
            probe.bind(endpoint);
            probe.close();
            return endpoint;
        }
        catch (const zmq::error_t&)
        {
            // In use: try another candidate.
        }
    }
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    return endpoint;
}

// A free TCP port on 127.0.0.1 (see free_zmq_endpoint).
inline int free_http_port()
{
    const auto endpoint = free_zmq_endpoint();
    return std::stoi(endpoint.substr(endpoint.rfind(':') + 1));
}

} // namespace thewatcher::test
