#include "test_support/net_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include <zmq.hpp>

namespace
{

int port_of(const std::string& endpoint)
{
    return std::stoi(endpoint.substr(endpoint.rfind(':') + 1));
}

} // namespace

SCENARIO("The free-port helpers never hand out the same port twice in one process")
{
    GIVEN("a test that asks for many endpoints and HTTP ports before binding any of them")
    {
        // These helpers probe and release a port, so a probe cannot see an
        // earlier pick that nothing has bound yet, and the OS may hand the
        // same port back after the release. The helpers remember what they
        // handed out instead. Uniqueness is per process only, and the
        // probe-release window remains: new tests should use ServerFixture or
        // an OS-assigned port instead.
        constexpr int draws = 200;

        WHEN("the ports are drawn")
        {
            std::set<int> seen;
            int repeats = 0;
            for (int i = 0; i < draws; ++i)
            {
                const int port =
                    (i % 3 == 2) ? thewatcher::test::free_http_port() : port_of(thewatcher::test::free_zmq_endpoint());
                if (!seen.insert(port).second)
                    ++repeats;
            }

            THEN("every port is different")
            {
                REQUIRE(repeats == 0);
                REQUIRE(seen.size() == static_cast<std::size_t>(draws));
            }
        }
    }
}

SCENARIO("A port from the free-port helpers can be bound by the code under test")
{
    GIVEN("a free endpoint and a free HTTP port")
    {
        const auto endpoint = thewatcher::test::free_zmq_endpoint();
        const auto http_port = thewatcher::test::free_http_port();

        WHEN("sockets bind them")
        {
            zmq::context_t ctx{1};
            zmq::socket_t zmq_server{ctx, ZMQ_REP};
            zmq_server.set(zmq::sockopt::linger, 0);
            zmq::socket_t http_server{ctx, ZMQ_REP};
            http_server.set(zmq::sockopt::linger, 0);

            THEN("both binds succeed on 127.0.0.1 with a non-zero port")
            {
                REQUIRE(endpoint.rfind("tcp://127.0.0.1:", 0) == 0);
                REQUIRE(port_of(endpoint) > 0);
                REQUIRE(http_port > 0);
                REQUIRE_NOTHROW(zmq_server.bind(endpoint));
                REQUIRE_NOTHROW(http_server.bind("tcp://127.0.0.1:" + std::to_string(http_port)));
            }
        }
    }
}
