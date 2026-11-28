#include "test_support/net_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

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
        // A fixture picks its ROUTER, enrollment and REST ports up front and
        // the server binds them later, so a probe cannot see an earlier pick.
        // With independent random draws from the 12768-port range, 200 draws
        // repeat a port about four times in five runs (400 draws repeated 4
        // ports in the run that confirmed the bug).
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
