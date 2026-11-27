// Agent runtime behaviour against stub ZeroMQ endpoints: shutdown latency,
// DISCONNECT, heartbeat independence, config validation and collector command
// safety. Waits are bounded polls (test_support/timing.hpp), never fixed sleeps
// standing in for an event.
#include "../agent/agent.hpp"
#include "common/commands.hpp"
#include "common/protocol.hpp"
#include "test_support/net_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

using namespace std::chrono_literals;
using thewatcher::test::eventually;
namespace proto = thewatcher::proto;

namespace
{

// Approves every enrollment request without a server key (insecure mode).
class EnrollmentStub
{
public:
    EnrollmentStub()
    {
        rep_.set(zmq::sockopt::linger, 0);
        rep_.bind("tcp://127.0.0.1:*");
        endpoint_ = rep_.get(zmq::sockopt::last_endpoint);
        thread_ = std::jthread([this](std::stop_token st) {
            while (!st.stop_requested())
            {
                zmq::pollitem_t item{static_cast<void*>(rep_), 0, ZMQ_POLLIN, 0};
                zmq::poll(&item, 1, 50ms);
                if (!(item.revents & ZMQ_POLLIN))
                    continue;
                zmq::message_t request;
                if (!rep_.recv(request, zmq::recv_flags::none))
                    continue;
                proto::EnrollResponse response;
                response.approved = true;
                response.message = "approved";
                proto::Frame f;
                f.type = static_cast<uint8_t>(proto::FrameType::ENROLL_RESPONSE);
                f.payload = proto::pack(response);
                const auto encoded = proto::encode_frame(f);
                rep_.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
            }
        });
    }
    const std::string& endpoint() const
    {
        return endpoint_;
    }

private:
    zmq::context_t ctx_{1};
    zmq::socket_t rep_{ctx_, ZMQ_REP};
    std::string endpoint_;
    std::jthread thread_; // declared last: joined before the socket closes
};

// Enrollment endpoint that accepts connections but never answers.
class SilentEnrollmentStub
{
public:
    SilentEnrollmentStub()
    {
        rep_.set(zmq::sockopt::linger, 0);
        rep_.bind("tcp://127.0.0.1:*");
        endpoint_ = rep_.get(zmq::sockopt::last_endpoint);
    }
    const std::string& endpoint() const
    {
        return endpoint_;
    }

private:
    zmq::context_t ctx_{1};
    zmq::socket_t rep_{ctx_, ZMQ_REP};
    std::string endpoint_;
};

// Unauthenticated ROUTER standing in for the server's data plane. Used from
// the test thread only.
class DataStub
{
public:
    DataStub()
    {
        router_.set(zmq::sockopt::linger, 0);
        router_.bind("tcp://127.0.0.1:*");
        endpoint_ = router_.get(zmq::sockopt::last_endpoint);
    }
    const std::string& endpoint() const
    {
        return endpoint_;
    }

    // Reads whatever arrives within `wait`.
    void pump(std::chrono::milliseconds wait)
    {
        zmq::pollitem_t item{static_cast<void*>(router_), 0, ZMQ_POLLIN, 0};
        zmq::poll(&item, 1, wait);
        while (item.revents & ZMQ_POLLIN)
        {
            zmq::message_t identity, delim, payload;
            if (!router_.recv(identity, zmq::recv_flags::dontwait))
                break;
            (void)router_.recv(delim, zmq::recv_flags::none);
            (void)router_.recv(payload, zmq::recv_flags::none);
            identity_.assign(static_cast<const char*>(identity.data()), identity.size());
            frames_.push_back(proto::decode_frame(payload.data(), payload.size()));
            zmq::poll(&item, 1, 0ms);
        }
    }

    bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (predicate())
                return true;
            pump(20ms);
        }
        return predicate();
    }

    std::size_t count(proto::FrameType type) const
    {
        std::size_t n = 0;
        for (const auto& f : frames_)
            n += f.type == static_cast<uint8_t>(type);
        return n;
    }

    std::size_t acks(bool ok) const
    {
        std::size_t n = 0;
        for (const auto& f : frames_)
            if (f.type == static_cast<uint8_t>(proto::FrameType::COMMAND_ACK))
                n += proto::unpack<thewatcher::AckMessage>(f.payload).success == ok;
        return n;
    }

    void send(proto::FrameType type, std::vector<uint8_t> payload)
    {
        proto::Frame f;
        f.type = static_cast<uint8_t>(type);
        f.payload = std::move(payload);
        const auto encoded = proto::encode_frame(f);
        router_.send(zmq::message_t(identity_.data(), identity_.size()), zmq::send_flags::sndmore);
        router_.send(zmq::message_t{}, zmq::send_flags::sndmore);
        router_.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
    }

    void send_command(thewatcher::CommandType type, std::vector<uint8_t> args = {})
    {
        thewatcher::CommandMessage cmd{static_cast<uint8_t>(type), "cmd-" + std::to_string(++commands_),
                                       std::move(args)};
        send(proto::FrameType::COMMAND, proto::pack(cmd));
    }

    void send_config(int interval, int heartbeat, int process_limit)
    {
        thewatcher::ConfigUpdate update;
        update.interval_seconds = interval;
        update.heartbeat_interval_seconds = heartbeat;
        update.process_limit = process_limit;
        send(proto::FrameType::CONFIG_UPDATE, proto::pack(update));
    }

private:
    zmq::context_t ctx_{1};
    zmq::socket_t router_{ctx_, ZMQ_ROUTER};
    std::string endpoint_;
    std::string identity_;
    std::vector<proto::Frame> frames_;
    int commands_ = 0;
};

thewatcher::agent::AgentConfig make_config(const std::string& enrollment, const std::string& server)
{
    thewatcher::agent::AgentConfig cfg;
    cfg.agent_id = "runtime-agent";
    cfg.enrollment_address = enrollment;
    cfg.server_address = server;
    cfg.allow_insecure_cli = true; // stubs speak plain ZeroMQ
    cfg.collection_interval = 300;
    cfg.heartbeat_interval = 1;
    cfg.process_limit = 5;
    return cfg;
}

// Starts the agent and waits until the stub has seen its first heartbeat.
bool start_and_connect(thewatcher::agent::Agent& agent, DataStub& server)
{
    agent.start();
    return server.wait_for(
        [&] {
            return server.count(proto::FrameType::HEARTBEAT) >= 1;
        },
        10s);
}

} // namespace

SCENARIO("A stop request is never lost and never blocks on an unreachable server")
{
    GIVEN("an agent pointed at endpoints where nothing listens")
    {
        auto cfg = make_config(thewatcher::test::free_zmq_endpoint(), thewatcher::test::free_zmq_endpoint());
        auto agent = std::make_unique<thewatcher::agent::Agent>(cfg);

        WHEN("stop is requested before start (the signal arrives early)")
        {
            agent->stop();
            const auto began = std::chrono::steady_clock::now();
            agent->start();

            THEN("start returns promptly instead of waiting for enrollment, and the stop sticks")
            {
                REQUIRE(std::chrono::steady_clock::now() - began < 1s);
                REQUIRE(agent->stop_requested());
            }
        }
    }
}

SCENARIO("Stopping during enrollment returns within a second")
{
    GIVEN("an enrollment server that never answers")
    {
        SilentEnrollmentStub silent;
        auto cfg = make_config(silent.endpoint(), thewatcher::test::free_zmq_endpoint());
        thewatcher::agent::Agent agent(cfg);
        std::atomic<bool> returned{false};
        std::jthread starter([&] {
            agent.start();
            returned.store(true);
        });

        WHEN("stop is requested while the agent waits for approval")
        {
            std::this_thread::sleep_for(300ms); // let the first request go out
            const auto requested = std::chrono::steady_clock::now();
            agent.stop();

            THEN("start returns within 1 s")
            {
                REQUIRE(eventually(
                    [&] {
                        return returned.load();
                    },
                    1s, 10ms));
                REQUIRE(std::chrono::steady_clock::now() - requested < 1500ms);
            }
        }
    }
}

SCENARIO("An agent whose server is unreachable shuts down within two seconds")
{
    GIVEN("an enrolled agent whose data endpoint has no listener")
    {
        EnrollmentStub enrollment;
        auto cfg = make_config(enrollment.endpoint(), thewatcher::test::free_zmq_endpoint());
        cfg.collection_interval = 1;
        auto agent = std::make_unique<thewatcher::agent::Agent>(cfg);
        agent->start();
        std::this_thread::sleep_for(1500ms); // let it queue frames toward nowhere

        WHEN("the agent is stopped and destroyed")
        {
            REQUIRE_FALSE(agent->connected());
            const auto began = std::chrono::steady_clock::now();
            agent->stop();
            agent.reset();

            THEN("it completes within 2 s")
            {
                REQUIRE(std::chrono::steady_clock::now() - began < 2s);
            }
        }
    }
}

SCENARIO("A DISCONNECT command makes the agent request its own exit")
{
    GIVEN("a running agent connected to a stub server")
    {
        EnrollmentStub enrollment;
        DataStub server;
        thewatcher::agent::Agent agent(make_config(enrollment.endpoint(), server.endpoint()));
        REQUIRE(start_and_connect(agent, server));
        REQUIRE(agent.connected());

        WHEN("the server sends DISCONNECT")
        {
            server.send_command(thewatcher::CommandType::DISCONNECT);

            THEN("the agent acknowledges it and signals that the process should exit")
            {
                REQUIRE(eventually(
                    [&] {
                        server.pump(20ms);
                        return agent.stop_requested();
                    },
                    5s, 10ms));
                server.pump(200ms);
                REQUIRE(server.acks(true) == 1);
            }
        }
    }
}

SCENARIO("Heartbeats are independent of the collection interval and honour changes quickly")
{
    GIVEN("an agent with a 300 s collection interval and a 30 s heartbeat")
    {
        EnrollmentStub enrollment;
        DataStub server;
        auto cfg = make_config(enrollment.endpoint(), server.endpoint());
        cfg.heartbeat_interval = 30;
        thewatcher::agent::Agent agent(cfg);
        REQUIRE(start_and_connect(agent, server));

        WHEN("the server lowers the heartbeat interval to 1 s")
        {
            server.send_config(300, 1, 5);

            THEN("further heartbeats arrive within a few seconds, not after the old 30 s interval")
            {
                REQUIRE(server.wait_for(
                    [&] {
                        return server.count(proto::FrameType::HEARTBEAT) >= 3;
                    },
                    6s));
            }
        }
    }
}

SCENARIO("A CONFIG_UPDATE with an invalid value is rejected and the previous value kept")
{
    GIVEN("an agent heartbeating every second")
    {
        EnrollmentStub enrollment;
        DataStub server;
        thewatcher::agent::Agent agent(make_config(enrollment.endpoint(), server.endpoint()));
        REQUIRE(start_and_connect(agent, server));

        WHEN("an update with heartbeat 0 and one with a negative interval arrive")
        {
            const auto before = server.count(proto::FrameType::HEARTBEAT);
            server.send_config(300, 0, 5);
            server.send_config(-1, 1, 5);
            server.wait_for(
                [] {
                    return false;
                },
                2500ms);

            THEN("heartbeats keep their 1 s cadence instead of flooding")
            {
                const auto during = server.count(proto::FrameType::HEARTBEAT) - before;
                REQUIRE(during >= 1);
                REQUIRE(during <= 5);
            }
        }
    }
}

SCENARIO("A SET_INTERVAL command outside the valid range is refused")
{
    GIVEN("a connected agent")
    {
        EnrollmentStub enrollment;
        DataStub server;
        thewatcher::agent::Agent agent(make_config(enrollment.endpoint(), server.endpoint()));
        REQUIRE(start_and_connect(agent, server));

        WHEN("the interval is set to 0")
        {
            server.send_command(thewatcher::CommandType::SET_INTERVAL, proto::pack(thewatcher::SetIntervalArgs{0}));

            THEN("the command is acknowledged as failed")
            {
                REQUIRE(server.wait_for(
                    [&] {
                        return server.acks(false) == 1;
                    },
                    5s));
            }
        }
    }
}

SCENARIO("A paused agent still asks the server for its configuration")
{
    GIVEN("a paused agent that polls for configuration every 200 ms")
    {
        EnrollmentStub enrollment;
        DataStub server;
        thewatcher::agent::Agent agent(make_config(enrollment.endpoint(), server.endpoint()),
                                       thewatcher::agent::AgentTimings{200ms});
        REQUIRE(start_and_connect(agent, server));
        server.send_command(thewatcher::CommandType::PAUSE);
        REQUIRE(server.wait_for(
            [&] {
                return server.acks(true) == 1;
            },
            5s));

        WHEN("time passes while paused")
        {
            const auto before = server.count(proto::FrameType::CONFIG_REQUEST);

            THEN("more config requests keep arriving")
            {
                REQUIRE(server.wait_for(
                    [&] {
                        return server.count(proto::FrameType::CONFIG_REQUEST) >= before + 3;
                    },
                    5s));
            }
        }
    }
}

SCENARIO("Restart and status commands race safely with the collection thread")
{
    GIVEN("an agent collecting every second")
    {
        EnrollmentStub enrollment;
        DataStub server;
        auto cfg = make_config(enrollment.endpoint(), server.endpoint());
        cfg.collection_interval = 1;
        thewatcher::agent::Agent agent(cfg);
        REQUIRE(start_and_connect(agent, server));

        WHEN("RESTART_COLLECTORS and GET_STATUS are sent repeatedly")
        {
            constexpr int rounds = 40;
            for (int i = 0; i < rounds; ++i)
            {
                server.send_command(thewatcher::CommandType::RESTART_COLLECTORS);
                server.send_command(thewatcher::CommandType::GET_STATUS);
                server.pump(5ms);
            }

            THEN("the agent survives and acknowledges every command")
            {
                REQUIRE(server.wait_for(
                    [&] {
                        return server.acks(true) == 2 * rounds;
                    },
                    60s));
                REQUIRE_FALSE(agent.stop_requested());
            }
        }
    }
}
