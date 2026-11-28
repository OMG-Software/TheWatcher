// Small networking helpers (free local ports/endpoints) shared by the server,
// agent and integration tests. Also re-exports the timing helpers.
#pragma once

#include "test_support/timing.hpp"

#include <atomic>
#include <random>
#include <string>

#include <zmq.hpp>

namespace thewatcher::test
{

// The next candidate port in 20000-32767 for this process.
//
// A fixture picks all of its ports before the server binds any of them, so a
// bind probe cannot tell that an earlier pick in the same process already
// took a port; independent random draws repeated one now and then (seen on
// FreeBSD CI as "Failed to listen"). Instead each process starts at a random
// offset, so test processes running side by side spread out, and then walks
// the range, so no port repeats within a process until all 12768 are used.
inline int next_candidate_port()
{
    static std::atomic<unsigned> next{std::random_device{}()};
    return 20000 + static_cast<int>(next.fetch_add(1) % 12768U);
}

// A free "tcp://127.0.0.1:<port>" endpoint for a server under test to bind.
//
// The port comes from next_candidate_port(), below the OS ephemeral range
// (Linux 32768-60999, Windows/macOS 49152-65535), and is checked by binding it.
// A port from the ephemeral range ("tcp://127.0.0.1:*") is released before
// the server binds it, and in that window the OS may hand it to an outgoing
// client connection, which makes the server's bind fail intermittently.
// Falls back to an OS-assigned port if no candidate can be bound.
inline std::string free_zmq_endpoint()
{
    // A context per call: destroying it waits until the probe socket is fully
    // closed, so the port is free again when this returns.
    zmq::context_t ctx{1};
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        const int port = next_candidate_port();
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
