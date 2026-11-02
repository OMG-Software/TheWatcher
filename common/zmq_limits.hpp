#pragma once

#include <cstdint>

namespace thewatcher::proto
{

// SEC-02: hard bounds on inbound ZMQ messages. libzmq buffers each message
// in full before the application sees it, so without a cap any host that
// can reach a socket can exhaust server/agent RAM with a single message.
//
// Enrollment requests/responses are a few hundred bytes; 64 KiB is a
// generous ceiling. Data-plane frames carry SystemMetrics payloads (many
// processes/disks/interfaces); a few MiB leaves ample headroom.
inline constexpr int64_t kEnrollMaxMessageBytes = 64 * 1024;
inline constexpr int64_t kDataPlaneMaxMessageBytes = 4 * 1024 * 1024;

// ZMQ_HANDSHAKE_IVL: how long to wait for a peer to complete the connection
// handshake (NULL/CURVE) before dropping it. Bounds slow-loris connection
// buildup on the unauthenticated enrollment socket in particular.
inline constexpr int kHandshakeIvlMs = 10'000;

// Enrollment reply send timeout: a client that accepts the request but never
// reads the reply cannot block the server's single data-plane thread.
inline constexpr int kEnrollSendTimeoutMs = 5'000;

} // namespace thewatcher::proto