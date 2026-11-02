// Transport-level hardening tests for the server's ZMQ sockets.
//
// Covers SEC-01 (unauthenticated remote crash via multipart enrollment
// message) and the enrollment side of SEC-02 (ZMQ_MAXMSGSIZE on the
// enrollment REP socket).
#include "../server/server.hpp"
#include "../server/startup_signal.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

using namespace thewatcher::server;
using namespace thewatcher::proto;

namespace
{

std::string free_zmq_tcp_endpoint()
{
    zmq::context_t ctx{1};
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    return endpoint;
}

int64_t test_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Minimal server configuration bound to free local ports with no CURVE keys,
// an in-memory store, and the REST API disabled (port 0).
ServerConfig make_transport_config()
{
    ServerConfig config;
    config.bind_address = free_zmq_tcp_endpoint();
    config.enrollment_address = free_zmq_tcp_endpoint();
    config.api_host = "127.0.0.1";
    config.api_port = 0;
    config.db_path = ":memory:";
    config.server_public_key.clear();
    config.server_secret_key.clear();
    return config;
}

EnrollRequest make_valid_enroll_request(const std::string& agent_id)
{
    EnrollRequest enroll;
    enroll.agent_id = agent_id;
    enroll.hostname = "transport-test-host";
    enroll.platform = "windows";
    enroll.curve_public_key_z85 = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    return enroll;
}

// Encodes an EnrollRequest as a wire frame.
std::vector<uint8_t> encode_enrollment(const EnrollRequest& enroll)
{
    Frame frame;
    frame.type = static_cast<uint8_t>(FrameType::ENROLL_REQUEST);
    frame.agent_id = enroll.agent_id;
    frame.timestamp_ms = test_now_ms();
    frame.payload = pack(enroll);
    return encode_frame(frame);
}

// Sends one enrollment request and waits up to timeout_ms for a reply.
// Returns the decoded EnrollResponse, or std::nullopt when no reply arrived.
std::optional<EnrollResponse> send_enrollment(const std::string& endpoint, const std::vector<uint8_t>& encoded,
                                              int timeout_ms = 2000)
{
    zmq::context_t ctx{1};
    zmq::socket_t req{ctx, ZMQ_REQ};
    req.set(zmq::sockopt::linger, 0);
    req.set(zmq::sockopt::rcvtimeo, timeout_ms);
    req.connect(endpoint);
    req.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);

    zmq::message_t reply;
    auto received = req.recv(reply, zmq::recv_flags::none);
    if (!received.has_value())
        return std::nullopt;
    auto reply_frame = decode_frame(reply.data(), reply.size());
    return unpack<EnrollResponse>(reply_frame.payload);
}

} // namespace

SCENARIO("Multipart enrollment messages cannot crash the server")
{
    GIVEN("a running server with an empty store")
    {
        ServerConfig config = make_transport_config();
        Server server(config);
        StartupSignal startup;
        std::exception_ptr server_error;

        std::thread server_thread([&] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                server_error = std::current_exception();
                startup.fail(server_error);
            }
        });

        auto result = startup.wait();
        REQUIRE(result.started);
        REQUIRE(server_error == nullptr);

        WHEN("a DEALER sends a delimited multipart enrollment message")
        {
            zmq::context_t ctx{1};
            zmq::socket_t dealer{ctx, ZMQ_DEALER};
            dealer.set(zmq::sockopt::linger, 0);
            dealer.set(zmq::sockopt::rcvtimeo, 2000);
            dealer.connect(config.enrollment_address);

            // Wire shape [""][payload][extra]: a REQ-compatible envelope whose
            // payload has two parts. REP delivers both parts to the app with
            // rcvmore set — the exact shape that used to crash the server.
            auto encoded = encode_enrollment(make_valid_enroll_request("multipart-agent"));
            dealer.send(zmq::message_t{}, zmq::send_flags::sndmore);
            dealer.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::sndmore);
            dealer.send(zmq::message_t("trailing-frame", 14), zmq::send_flags::none);

            zmq::message_t reply;
            auto received = dealer.recv(reply, zmq::recv_flags::none);
            // REP replies arrive as [empty delimiter][payload]; collect the
            // whole envelope and keep the last non-empty part for decoding.
            std::vector<uint8_t> reply_payload;
            while (received.has_value())
            {
                if (reply.size() > 0)
                    reply_payload.assign(static_cast<const uint8_t*>(reply.data()),
                                         static_cast<const uint8_t*>(reply.data()) + reply.size());
                if (!dealer.get(zmq::sockopt::rcvmore))
                    break;
                received = dealer.recv(reply, zmq::recv_flags::none);
            }

            THEN("the server replies with an error and keeps running")
            {
                REQUIRE(received.has_value());
                REQUIRE_FALSE(reply_payload.empty());
                auto reply_frame = decode_frame(reply_payload.data(), reply_payload.size());
                auto response = unpack<EnrollResponse>(reply_frame.payload);
                REQUIRE(response.approved == false);
                REQUIRE(server_error == nullptr);
            }

            AND_THEN("a subsequent valid enrollment is still answered")
            {
                auto response = send_enrollment(config.enrollment_address,
                                                encode_enrollment(make_valid_enroll_request("valid-agent")));
                REQUIRE(response.has_value());
                REQUIRE(response->approved == false);
                REQUIRE(response->message == "pending approval");
                REQUIRE(server_error == nullptr);
            }
        }

        WHEN("a DEALER sends a multipart message without a leading delimiter")
        {
            zmq::context_t ctx{1};
            zmq::socket_t dealer{ctx, ZMQ_DEALER};
            dealer.set(zmq::sockopt::linger, 0);
            dealer.set(zmq::sockopt::rcvtimeo, 2000);
            dealer.connect(config.enrollment_address);

            // No empty delimiter: REP treats the whole message as a malformed
            // envelope and rolls it back at the socket layer. The server never
            // sees a request — the pin here is that it stays healthy anyway.
            auto encoded = encode_enrollment(make_valid_enroll_request("no-delim-agent"));
            dealer.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::sndmore);
            dealer.send(zmq::message_t("trailing-frame", 14), zmq::send_flags::none);

            zmq::message_t reply;
            auto received = dealer.recv(reply, zmq::recv_flags::none);

            THEN("no reply is delivered and the server keeps running")
            {
                REQUIRE_FALSE(received.has_value());
                REQUIRE(server_error == nullptr);
            }

            AND_THEN("a subsequent valid enrollment is still answered")
            {
                auto response = send_enrollment(config.enrollment_address,
                                                encode_enrollment(make_valid_enroll_request("valid-agent-2")));
                REQUIRE(response.has_value());
                REQUIRE(response->approved == false);
                REQUIRE(response->message == "pending approval");
                REQUIRE(server_error == nullptr);
            }
        }

        server.stop();
        if (server_thread.joinable())
            server_thread.join();
        REQUIRE(server_error == nullptr);
    }
}

SCENARIO("Oversize enrollment messages are rejected without harm")
{
    GIVEN("a running server with an empty store")
    {
        ServerConfig config = make_transport_config();
        Server server(config);
        StartupSignal startup;
        std::exception_ptr server_error;

        std::thread server_thread([&] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                server_error = std::current_exception();
                startup.fail(server_error);
            }
        });

        auto result = startup.wait();
        REQUIRE(result.started);
        REQUIRE(server_error == nullptr);

        WHEN("a client sends an enrollment message far larger than the enrollment size cap")
        {
            // 128 KiB: twice the enrollment socket's maximum message size.
            std::vector<uint8_t> huge(128 * 1024, 'x');
            auto response = send_enrollment(config.enrollment_address, huge);

            THEN("no reply is delivered and the server keeps running")
            {
                REQUIRE_FALSE(response.has_value());
                REQUIRE(server_error == nullptr);
            }

            AND_THEN("a subsequent valid enrollment is still answered")
            {
                auto reply = send_enrollment(config.enrollment_address,
                                             encode_enrollment(make_valid_enroll_request("valid-agent")));
                REQUIRE(reply.has_value());
                REQUIRE(reply->approved == false);
                REQUIRE(server_error == nullptr);
            }
        }

        server.stop();
        if (server_thread.joinable())
            server_thread.join();
        REQUIRE(server_error == nullptr);
    }
}