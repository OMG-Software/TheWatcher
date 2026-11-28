// Behavioural tests for retry_on_eintr: EINTR is retried, everything else is
// reported, and values come back unchanged.
#include "common/zmq_retry.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <stdexcept>
#include <string>

using thewatcher::zmqutil::retry_on_eintr;

namespace
{

// Throws a zmq::error_t carrying the given errno value.
[[noreturn]] void throw_zmq_error(int code)
{
    errno = code;
    throw zmq::error_t();
}

} // namespace

SCENARIO("retry_on_eintr repeats a ZeroMQ call interrupted by a signal")
{
    GIVEN("an operation that is interrupted twice before it succeeds")
    {
        int calls = 0;
        auto operation = [&] {
            if (++calls <= 2)
                throw_zmq_error(EINTR);
            return std::string("received");
        };

        WHEN("it is run through retry_on_eintr")
        {
            const auto result = retry_on_eintr(operation);

            THEN("the eventual result is returned after three attempts")
            {
                REQUIRE(result == "received");
                REQUIRE(calls == 3);
            }
        }
    }

    GIVEN("an operation that returns void and is interrupted once")
    {
        int calls = 0;

        WHEN("it is run through retry_on_eintr")
        {
            retry_on_eintr([&] {
                if (++calls == 1)
                    throw_zmq_error(EINTR);
            });

            THEN("it is attempted again and completes")
            {
                REQUIRE(calls == 2);
            }
        }
    }
}

SCENARIO("retry_on_eintr does not hide real ZeroMQ failures")
{
    GIVEN("an operation that fails with a different error")
    {
        int calls = 0;

        WHEN("it is run through retry_on_eintr")
        {
            THEN("the error propagates after a single attempt")
            {
                REQUIRE_THROWS_AS(retry_on_eintr([&] {
                                      ++calls;
                                      throw_zmq_error(ETERM);
                                  }),
                                  zmq::error_t);
                REQUIRE(calls == 1);
            }
        }
    }

    GIVEN("an operation that throws something other than a ZeroMQ error")
    {
        THEN("it propagates untouched")
        {
            REQUIRE_THROWS_AS(retry_on_eintr([] {
                                  throw std::runtime_error("boom");
                              }),
                              std::runtime_error);
        }
    }
}
