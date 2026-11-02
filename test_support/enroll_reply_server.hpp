// A scripted enrollment server for agent tests: binds a REP socket on a free
// local port and answers the first request with a canned EnrollResponse.
#pragma once

#include "common/crypto.hpp"
#include "common/protocol.hpp"

#include <string>
#include <thread>

#include <zmq.hpp>

namespace thewatcher::test
{

// Builds and sends an EnrollResponse on a REP socket.
inline void send_enroll_response(zmq::socket_t& rep, const std::string& agent_id, bool approved,
                                 const std::string& server_public_key = {})
{
    proto::EnrollResponse resp;
    resp.approved = approved;
    resp.message = approved ? "approved" : "pending approval";
    if (approved)
    {
        resp.server_public_key_z85 = server_public_key;
        resp.server_public_key_fingerprint = crypto::server_public_key_fingerprint(server_public_key);
    }
    proto::Frame f;
    f.type = static_cast<uint8_t>(proto::FrameType::ENROLL_RESPONSE);
    f.agent_id = agent_id;
    f.timestamp_ms = 0;
    f.payload = proto::pack(resp);
    auto encoded = proto::encode_frame(f);
    rep.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
}

class EnrollReplyServer
{
public:
    // Binds immediately; use endpoint() as the agent's enrollment_address.
    EnrollReplyServer()
    {
        rep_.set(zmq::sockopt::linger, 0);
        rep_.set(zmq::sockopt::rcvtimeo, 3000);
        rep_.bind("tcp://127.0.0.1:*");
        endpoint_ = rep_.get(zmq::sockopt::last_endpoint);
    }

    ~EnrollReplyServer()
    {
        join();
    }

    EnrollReplyServer(const EnrollReplyServer&) = delete;
    EnrollReplyServer& operator=(const EnrollReplyServer&) = delete;

    const std::string& endpoint() const
    {
        return endpoint_;
    }

    // Answers one request (if one arrives within 3 s) in the background.
    void reply_once(const std::string& agent_id, bool approved, const std::string& server_public_key = {})
    {
        thread_ = std::thread([this, agent_id, approved, server_public_key] {
            zmq::message_t req;
            if (!rep_.recv(req, zmq::recv_flags::none))
                return;
            send_enroll_response(rep_, agent_id, approved, server_public_key);
        });
    }

    // Waits for the reply thread to finish. Does nothing if it is not running.
    void join()
    {
        if (thread_.joinable())
            thread_.join();
    }

private:
    zmq::context_t ctx_{1};
    zmq::socket_t rep_{ctx_, ZMQ_REP};
    std::string endpoint_;
    std::thread thread_;
};

} // namespace thewatcher::test
