// send_to_peer: a ROUTER send never blocks the data plane and reports a
// command that was not accepted as undelivered (issue #54).
#include "../server/router_send.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <zmq.hpp>

using thewatcher::server::send_to_peer;

SCENARIO("A command for a connected peer is accepted")
{
    GIVEN("a ROUTER and a DEALER with a known routing id")
    {
        zmq::context_t ctx{1};
        zmq::socket_t router{ctx, ZMQ_ROUTER};
        router.set(zmq::sockopt::linger, 0);
        router.set(zmq::sockopt::router_mandatory, 1);
        router.bind("inproc://router-send-ok");
        zmq::socket_t dealer{ctx, ZMQ_DEALER};
        dealer.set(zmq::sockopt::linger, 0);
        dealer.set(zmq::sockopt::routing_id, "peer-1");
        dealer.set(zmq::sockopt::rcvtimeo, 2000);
        dealer.connect("inproc://router-send-ok");

        WHEN("a payload is sent to that routing id")
        {
            const std::vector<uint8_t> payload{1, 2, 3};
            const bool accepted = send_to_peer(router, "peer-1", payload.data(), payload.size());

            THEN("it is accepted and the peer receives [empty][payload]")
            {
                REQUIRE(accepted);
                zmq::message_t delimiter, body;
                REQUIRE(dealer.recv(delimiter, zmq::recv_flags::none));
                REQUIRE(delimiter.size() == 0);
                REQUIRE(dealer.recv(body, zmq::recv_flags::none));
                REQUIRE(body.size() == 3);
            }
        }
    }
}

SCENARIO("A peer that has gone away is reported, not silently dropped")
{
    GIVEN("a ROUTER with router_mandatory and no peer")
    {
        zmq::context_t ctx{1};
        zmq::socket_t router{ctx, ZMQ_ROUTER};
        router.set(zmq::sockopt::linger, 0);
        router.set(zmq::sockopt::router_mandatory, 1);
        router.bind("inproc://router-send-gone");

        WHEN("a payload is sent to an unknown routing id")
        {
            const std::vector<uint8_t> payload{1};

            THEN("the send throws EHOSTUNREACH")
            {
                REQUIRE_THROWS_AS(send_to_peer(router, "nobody", payload.data(), payload.size()), zmq::error_t);
            }
        }
    }
}

SCENARIO("A full send queue makes the send fail instead of blocking")
{
    GIVEN("a ROUTER with a send high-water mark of 1 and a peer that never reads")
    {
        zmq::context_t ctx{1};
        zmq::socket_t router{ctx, ZMQ_ROUTER};
        router.set(zmq::sockopt::linger, 0);
        router.set(zmq::sockopt::router_mandatory, 1);
        router.set(zmq::sockopt::sndhwm, 1);
        router.bind("inproc://router-send-full");
        zmq::socket_t dealer{ctx, ZMQ_DEALER};
        dealer.set(zmq::sockopt::linger, 0);
        dealer.set(zmq::sockopt::routing_id, "peer-2");
        dealer.set(zmq::sockopt::rcvhwm, 1);
        dealer.connect("inproc://router-send-full");

        WHEN("many payloads are sent")
        {
            const std::vector<uint8_t> payload(1024, 7);
            bool refused = false;
            for (int i = 0; i < 100 && !refused; ++i)
                refused = !send_to_peer(router, "peer-2", payload.data(), payload.size());

            THEN("a send is refused without blocking")
            {
                REQUIRE(refused);
            }
        }
    }
}
