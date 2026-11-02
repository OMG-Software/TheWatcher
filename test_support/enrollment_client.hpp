// Client side of the unauthenticated enrollment socket, for server tests:
// builds ENROLL_REQUEST frames and exchanges them over a REQ socket.
#pragma once

#include "common/protocol.hpp"
#include "test_support/net_helpers.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <zmq.hpp>

namespace thewatcher::test
{

inline proto::EnrollRequest make_enroll_request(const std::string& agent_id, const std::string& public_key_z85,
                                                const std::string& hostname = "enroll-test-host")
{
    proto::EnrollRequest request;
    request.agent_id = agent_id;
    request.hostname = hostname;
    request.platform = "windows";
    request.curve_public_key_z85 = public_key_z85;
    return request;
}

// Encodes an EnrollRequest as a wire frame.
inline std::vector<uint8_t> encode_enrollment(const proto::EnrollRequest& request)
{
    proto::Frame frame;
    frame.type = static_cast<uint8_t>(proto::FrameType::ENROLL_REQUEST);
    frame.agent_id = request.agent_id;
    frame.timestamp_ms = now_ms();
    frame.payload = proto::pack(request);
    return proto::encode_frame(frame);
}

// Sends one pre-encoded enrollment message and waits up to timeout_ms for the
// raw reply bytes; nullopt when no reply arrives.
inline std::optional<std::vector<uint8_t>> exchange_enrollment(const std::string& endpoint,
                                                               const std::vector<uint8_t>& encoded,
                                                               int timeout_ms = 2000)
{
    zmq::context_t ctx{1};
    zmq::socket_t req{ctx, ZMQ_REQ};
    req.set(zmq::sockopt::linger, 0);
    req.set(zmq::sockopt::rcvtimeo, timeout_ms);
    req.connect(endpoint);
    req.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);

    zmq::message_t reply;
    if (!req.recv(reply, zmq::recv_flags::none).has_value())
        return std::nullopt;
    const auto* data = static_cast<const uint8_t*>(reply.data());
    return std::vector<uint8_t>(data, data + reply.size());
}

// Sends one pre-encoded enrollment message and returns the decoded response
// (nullopt when no reply arrives within timeout_ms).
inline std::optional<proto::EnrollResponse> send_enrollment(const std::string& endpoint,
                                                            const std::vector<uint8_t>& encoded, int timeout_ms = 2000)
{
    const auto reply = exchange_enrollment(endpoint, encoded, timeout_ms);
    if (!reply.has_value())
        return std::nullopt;
    const auto reply_frame = proto::decode_frame(reply->data(), reply->size());
    return proto::unpack<proto::EnrollResponse>(reply_frame.payload);
}

// Encodes and sends `request`, returning the decoded response.
inline std::optional<proto::EnrollResponse> enroll(const std::string& endpoint, const proto::EnrollRequest& request,
                                                   int timeout_ms = 2000)
{
    return send_enrollment(endpoint, encode_enrollment(request), timeout_ms);
}

} // namespace thewatcher::test
