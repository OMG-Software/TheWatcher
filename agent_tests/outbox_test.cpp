#include "../agent/outbox.hpp"

#include <catch2/catch_test_macros.hpp>

using thewatcher::agent::FramePriority;
using thewatcher::agent::Outbox;

namespace
{
Outbox::Bytes frame(std::uint8_t tag)
{
    return Outbox::Bytes{tag};
}
} // namespace

SCENARIO("The outbox sends urgent frames before bulk frames")
{
    GIVEN("an outbox holding metrics queued before a heartbeat")
    {
        Outbox outbox;
        outbox.push(frame(1), FramePriority::Bulk);
        outbox.push(frame(2), FramePriority::Bulk);
        outbox.push(frame(9), FramePriority::Urgent);

        WHEN("frames are popped")
        {
            const auto first = outbox.pop();
            const auto second = outbox.pop();
            const auto third = outbox.pop();

            THEN("the urgent frame comes first and each class stays FIFO")
            {
                REQUIRE(first.has_value());
                REQUIRE((*first)[0] == 9);
                REQUIRE((*second)[0] == 1);
                REQUIRE((*third)[0] == 2);
                REQUIRE_FALSE(outbox.pop().has_value());
            }
        }
    }
}

SCENARIO("The outbox stays bounded while nothing is sent")
{
    GIVEN("a small outbox whose peer never drains it")
    {
        Outbox outbox(/*max_urgent=*/2, /*max_bulk=*/3);

        WHEN("far more frames than the limits are queued")
        {
            for (std::uint8_t i = 0; i < 100; ++i)
                outbox.push(frame(i), FramePriority::Bulk);
            outbox.push(frame(200), FramePriority::Urgent);
            outbox.push(frame(201), FramePriority::Urgent);
            outbox.push(frame(202), FramePriority::Urgent);

            THEN("only the newest frames of each class are kept and heartbeats are never starved by metrics")
            {
                REQUIRE(outbox.size() == 5);
                REQUIRE(outbox.dropped() == 97 + 1);
                REQUIRE((*outbox.pop())[0] == 201);
                REQUIRE((*outbox.pop())[0] == 202);
                REQUIRE((*outbox.pop())[0] == 97);
            }
        }
    }
}
