// Small networking helpers (free local ports/endpoints) shared by the server,
// agent and integration tests. Also re-exports the timing helpers.
//
// PREFER NOT TO USE free_zmq_endpoint() / free_http_port() in new tests. They
// probe a port, release it, and let the code under test bind it later, and in
// that window another process or an outgoing connection can take it. Start a
// server through ServerFixture / ServerRunner (which bind OS-assigned ports
// that the server holds from the moment of bind and reports back), or give a
// Server "tcp://127.0.0.1:*" / an ApiServer port 0 and read the actual
// endpoint or port from it after startup.
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
// Unique per process only: each process starts at a random offset and walks
// the range, so no port repeats within one process until all 12768 are used.
// Two processes can still pick the same port. The range is below the Linux,
// Windows and macOS ephemeral ranges but inside FreeBSD's default one (from
// 10000) and overlaps OpenBSD's, so an outgoing connection can be given a
// candidate port too.
inline int next_candidate_port()
{
    static std::atomic<unsigned> next{std::random_device{}()};
    return 20000 + static_cast<int>(next.fetch_add(1) % 12768U);
}

// A free "tcp://127.0.0.1:<port>" endpoint for a server under test to bind.
//
// The port comes from next_candidate_port() and is checked by binding it, then
// released before returning. It is therefore only a hint: between the release
// and the code under test binding it (the race window) another process may
// pick the same port, and on FreeBSD/OpenBSD an outgoing connection may take
// it. Falls back to an OS-assigned port if no candidate can be bound.
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
