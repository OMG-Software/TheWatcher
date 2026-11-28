#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <zmq.hpp>

namespace thewatcher::server
{

// Sends [routing id][empty delimiter][payload] to one ROUTER peer without
// blocking: every part uses dontwait, so a full send queue cannot stall the
// data plane. Returns false when any part is not accepted (queue full), in
// which case the command was not delivered. With ZMQ_ROUTER_MANDATORY a peer
// that is gone makes the send throw zmq::error_t (EHOSTUNREACH).
inline bool send_to_peer(zmq::socket_t& router, const std::string& routing_id, const uint8_t* payload, std::size_t size)
{
    constexpr auto more = zmq::send_flags::sndmore | zmq::send_flags::dontwait;
    if (!router.send(zmq::message_t(routing_id.data(), routing_id.size()), more))
        return false;
    if (!router.send(zmq::message_t{}, more))
        return false;
    return router.send(zmq::message_t(payload, size), zmq::send_flags::dontwait).has_value();
}

} // namespace thewatcher::server
