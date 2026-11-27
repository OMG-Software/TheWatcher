#pragma once

#include <zmq.hpp>

namespace thewatcher::proto
{

// Issue #143: how quickly each side notices that the other has gone away
// without closing the connection (power loss, VM reset, NAT mapping change).
//
// ZMTP heartbeats: each side pings after kHeartbeatIvlMs of silence and
// drops the connection when no reply arrives within kHeartbeatTimeoutMs. The
// TTL tells the peer to drop us after the same time without any traffic.
// Heartbeats are answered by libzmq's I/O thread, so a busy application
// thread does not trigger them.
inline constexpr int kHeartbeatIvlMs = 10'000;
inline constexpr int kHeartbeatTimeoutMs = 30'000;
inline constexpr int kHeartbeatTtlMs = 30'000;

// TCP keepalive is the second line of defence for peers that do not speak ZMTP
// heartbeats (older agents): probe after 60 s idle, every 10 s, give up after 3.
inline constexpr int kTcpKeepaliveIdleSeconds = 60;
inline constexpr int kTcpKeepaliveIntervalSeconds = 10;
inline constexpr int kTcpKeepaliveProbes = 3;

// Enables ZMTP heartbeats and TCP keepalive on a data-plane socket. Call it
// before bind()/connect(): options apply to connections made afterwards.
inline void apply_connection_liveness(zmq::socket_t& socket)
{
    socket.set(zmq::sockopt::heartbeat_ivl, kHeartbeatIvlMs);
    socket.set(zmq::sockopt::heartbeat_timeout, kHeartbeatTimeoutMs);
    socket.set(zmq::sockopt::heartbeat_ttl, kHeartbeatTtlMs);
    socket.set(zmq::sockopt::tcp_keepalive, 1);
    socket.set(zmq::sockopt::tcp_keepalive_idle, kTcpKeepaliveIdleSeconds);
    socket.set(zmq::sockopt::tcp_keepalive_intvl, kTcpKeepaliveIntervalSeconds);
    socket.set(zmq::sockopt::tcp_keepalive_cnt, kTcpKeepaliveProbes);
}

} // namespace thewatcher::proto
