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

#include <mutex>
#include <set>
#include <stdexcept>
#include <string>

#include <zmq.hpp>

namespace thewatcher::test
{

// A free "tcp://127.0.0.1:<port>" endpoint for a server under test to bind.
//
// The OS picks the port: a probe binds "tcp://127.0.0.1:*", so the port comes
// from the platform's own ephemeral range and nothing had it bound at that
// moment. There is no fixed port range, so nothing here assumes where a
// platform's ephemeral range starts (FreeBSD's starts at 10000, OpenBSD's
// default covers 1024-49151).
//
// The probe is released before returning, so the port is only a hint: between
// the release and the code under test binding it (the race window) another
// process or an outgoing connection may take it. The OS may also hand the same
// port back to a later probe, so the ports handed out are remembered and none
// is handed out twice in one process; a caller may ask for several before it
// binds any. Two processes can still be handed the same port.
inline std::string free_zmq_endpoint()
{
    static std::mutex mutex;
    static std::set<std::string> handed_out;

    // A context per call: destroying it waits until the probe sockets are fully
    // closed, so the port is free again when this returns.
    zmq::context_t ctx{1};
    for (int attempt = 0; attempt < 64; ++attempt)
    {
        std::string endpoint;
        {
            zmq::socket_t probe{ctx, ZMQ_REP};
            probe.set(zmq::sockopt::linger, 0);
            probe.bind("tcp://127.0.0.1:*");
            endpoint = probe.get(zmq::sockopt::last_endpoint);
        }
        const std::lock_guard<std::mutex> lock(mutex);
        if (handed_out.insert(endpoint).second)
            return endpoint;
        // Already handed out in this process: ask the OS again.
    }
    throw std::runtime_error("free_zmq_endpoint: the OS kept returning ports already handed out");
}

// A free TCP port on 127.0.0.1 (see free_zmq_endpoint).
inline int free_http_port()
{
    const auto endpoint = free_zmq_endpoint();
    return std::stoi(endpoint.substr(endpoint.rfind(':') + 1));
}

} // namespace thewatcher::test
