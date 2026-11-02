// Transport-level hardening tests for the server's ZMQ sockets.
//
// Covers SEC-01 (unauthenticated remote crash via multipart enrollment
// message) and the enrollment side of SEC-02 (ZMQ_MAXMSGSIZE on the
// enrollment REP socket).
#include "test_support/enrollment_client.hpp"
#include "test_support/server_runner.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <zmq.hpp>

using namespace thewatcher::proto;
using namespace thewatcher::test;

namespace
{

EnrollRequest make_valid_enroll_request(const std::string& agent_id)
{
    return make_enroll_request(agent_id, "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "transport-test-host");
}

zmq::socket_t connect_dealer(zmq::context_t& ctx, const std::string& endpoint)
{
    zmq::socket_t dealer{ctx, ZMQ_DEALER};
    dealer.set(zmq::sockopt::linger, 0);
    dealer.set(zmq::sockopt::rcvtimeo, 2000);
    dealer.connect(endpoint);
    return dealer;
}

// Asserts the server still answers a well-formed enrollment with "pending approval".
void require_valid_enrollment_answered(const ServerRunner& runner, const std::string& agent_id)
{
    auto response =
        send_enrollment(runner.config.enrollment_address, encode_enrollment(make_valid_enroll_request(agent_id)));
    REQUIRE(response.has_value());
    REQUIRE(response->approved == false);
    REQUIRE(response->message == "pending approval");
    REQUIRE(runner.error == nullptr);
}

} // namespace

SCENARIO("Multipart enrollment messages cannot crash the server")
{
    GIVEN("a running server with an empty store")
    {
        ServerRunner runner(make_in_memory_server_config());
        const auto& config = runner.config;
        REQUIRE(runner.startup.wait().started);
        REQUIRE(runner.error == nullptr);

        WHEN("a DEALER sends a delimited multipart enrollment message")
        {
            zmq::context_t ctx{1};
            auto dealer = connect_dealer(ctx, config.enrollment_address);

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
                REQUIRE(runner.error == nullptr);
            }

            AND_THEN("a subsequent valid enrollment is still answered")
            {
                require_valid_enrollment_answered(runner, "valid-agent");
            }
        }

        WHEN("a DEALER sends a multipart message without a leading delimiter")
        {
            zmq::context_t ctx{1};
            auto dealer = connect_dealer(ctx, config.enrollment_address);

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
                REQUIRE(runner.error == nullptr);
            }

            AND_THEN("a subsequent valid enrollment is still answered")
            {
                require_valid_enrollment_answered(runner, "valid-agent-2");
            }
        }

        runner.stop();
        REQUIRE(runner.error == nullptr);
    }
}

SCENARIO("Oversize enrollment messages are rejected without harm")
{
    GIVEN("a running server with an empty store")
    {
        ServerRunner runner(make_in_memory_server_config());
        const auto& config = runner.config;
        REQUIRE(runner.startup.wait().started);
        REQUIRE(runner.error == nullptr);

        WHEN("a client sends an enrollment message far larger than the enrollment size cap")
        {
            // 128 KiB: twice the enrollment socket's maximum message size.
            std::vector<uint8_t> huge(128 * 1024, 'x');
            auto response = send_enrollment(config.enrollment_address, huge);

            THEN("no reply is delivered and the server keeps running")
            {
                REQUIRE_FALSE(response.has_value());
                REQUIRE(runner.error == nullptr);
            }

            AND_THEN("a subsequent valid enrollment is still answered")
            {
                auto reply = send_enrollment(config.enrollment_address,
                                             encode_enrollment(make_valid_enroll_request("valid-agent")));
                REQUIRE(reply.has_value());
                REQUIRE(reply->approved == false);
                REQUIRE(runner.error == nullptr);
            }
        }

        runner.stop();
        REQUIRE(runner.error == nullptr);
    }
}
SCENARIO("A short data-plane message cannot stall the server loop")
{
    GIVEN("a running server with an empty store")
    {
        ServerRunner runner(make_in_memory_server_config());
        REQUIRE(runner.startup.wait().started);
        REQUIRE(runner.error == nullptr);

        WHEN("a DEALER sends a single-frame message to the ROUTER (no delimiter, no payload frame)")
        {
            zmq::context_t ctx{1};
            auto dealer = connect_dealer(ctx, runner.config.bind_address);
            // ROUTER delivers [identity][frame]: one part fewer than the
            // [identity][empty][payload] the server expects. A blocking recv
            // for the missing part would wait for the next message instead.
            dealer.send(zmq::message_t("short", 5), zmq::send_flags::none);

            THEN("the server still answers enrollment requests promptly")
            {
                require_valid_enrollment_answered(runner, "after-short-frame");
            }
        }

        runner.stop();
        REQUIRE(runner.error == nullptr);
    }
}
