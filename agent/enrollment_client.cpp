#pragma once

#include <memory>
#include <string>

#include <zmq.hpp>

namespace thewatcher::agent
{

/**
 * @brief Handles the initial, unencrypted enrollment handshake with the server.
 *
 * This client connects to a dedicated REQ socket (port 5556) to send an EnrollRequest
 * and polls until it receives an approved response or times out/fails.
 */
class EnrollmentClient
{
public:
    EnrollmentClient(zmq::context_t& ctx, const std::string& server_addr);
    ~EnrollmentClient();

    /**
     * @brief Attempts to enroll the agent with the server.
     * @return true if enrollment was successful and approved; false otherwise.
     */
    bool enroll(const std::string& agent_id, const std::string& hostname, const std::string& platform,
                const std::string& curve_public_key);

private:
    zmq::context_t& ctx_;
    std::unique_ptr<zmq::socket_t> req_socket_;
    std::string server_address_;
};