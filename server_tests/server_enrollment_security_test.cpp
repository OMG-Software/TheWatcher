// Enrollment hardening tests (SEC-05 / SEC-06).
//
// The enrollment socket is the one deliberately unauthenticated surface:
// any host that can reach it can send requests. These tests pin the
// validation, token gate, pending-queue immutability and last_seen rules
// that keep that surface from being used to flood storage, hijack pending
// records or suppress dead-agent alerts.
#include "../server/server.hpp"
#include "../server/startup_signal.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <zmq.hpp>

using namespace std::chrono_literals;
using namespace thewatcher::server;
using namespace thewatcher::proto;
using json = nlohmann::json;

namespace
{

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string free_zmq_endpoint()
{
    zmq::context_t ctx{1};
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    return endpoint;
}

int free_http_port()
{
    // OS-assigned free port: clock-derived ports collide when meson runs
    // test executables in parallel.
    zmq::context_t ctx{1};
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    const auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    const auto colon = endpoint.rfind(':');
    return std::stoi(endpoint.substr(colon + 1));
}

// Sends one enrollment request and returns the decoded response (waits up
// to 2 s; nullopt when no reply arrives).
std::optional<EnrollResponse> enroll(const std::string& endpoint, const EnrollRequest& request)
{
    zmq::context_t ctx{1};
    zmq::socket_t req{ctx, ZMQ_REQ};
    req.set(zmq::sockopt::linger, 0);
    req.set(zmq::sockopt::rcvtimeo, 2000);
    req.connect(endpoint);

    Frame frame;
    frame.type = static_cast<uint8_t>(FrameType::ENROLL_REQUEST);
    frame.agent_id = request.agent_id;
    frame.timestamp_ms = now_ms();
    frame.payload = pack(request);

    const auto encoded = encode_frame(frame);
    req.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);

    zmq::message_t reply;
    auto received = req.recv(reply, zmq::recv_flags::none);
    if (!received.has_value())
        return std::nullopt;
    auto reply_frame = decode_frame(reply.data(), reply.size());
    return unpack<EnrollResponse>(reply_frame.payload);
}

EnrollRequest valid_request(const std::string& agent_id, const std::string& z85_key)
{
    EnrollRequest request;
    request.agent_id = agent_id;
    request.hostname = "enroll-test-host";
    request.platform = "windows";
    request.curve_public_key_z85 = z85_key;
    return request;
}

// A running server plus an admin HTTP client.
struct ServerFixture
{
    ServerConfig config;
    Server server;
    StartupSignal startup;
    std::exception_ptr error;
    std::thread thread;
    httplib::Client client;

    explicit ServerFixture(std::string enrollment_token = {})
        : server(make_config(std::move(enrollment_token)))
        , client("http://127.0.0.1:" + std::to_string(config.api_port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
        thread = std::thread([this] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                error = std::current_exception();
                startup.fail(error);
            }
        });
    }

    ~ServerFixture()
    {
        server.stop();
        if (thread.joinable())
            thread.join();
        std::error_code ec;
        std::filesystem::remove(config.db_path, ec);
        std::filesystem::remove(config.db_path + "-shm", ec);
        std::filesystem::remove(config.db_path + "-wal", ec);
    }

    bool login()
    {
        auto res =
            client.Post("/api/login", R"({"username":"thewatcher","password":"look_at_me"})", "application/json");
        if (!res || res->status != 200)
            return false;
        const auto set_cookie = res->get_header_value("Set-Cookie");
        const auto end = set_cookie.find(';');
        if (set_cookie.empty() || end == std::string::npos)
            return false;
        client.set_default_headers({
            {"Cookie", set_cookie.substr(0, end)}
        });
        return true;
    }

    bool approve(const std::string& agent_id)
    {
        auto res = client.Post("/api/agents/" + agent_id + "/approve", "", "application/json");
        return res && res->status == 200;
    }

    std::optional<int64_t> agent_last_seen(const std::string& agent_id)
    {
        auto res = client.Get("/api/agents");
        if (!res || res->status != 200)
            return std::nullopt;
        for (const auto& a : json::parse(res->body))
            if (a.value("agent_id", "") == agent_id)
                return a.value("last_seen", int64_t{0});
        return std::nullopt;
    }

    bool pending_contains(const std::string& agent_id)
    {
        auto res = client.Get("/api/pending-enrollments");
        if (!res || res->status != 200)
            return false;
        for (const auto& a : json::parse(res->body))
            if (a.value("agent_id", "") == agent_id)
                return true;
        return false;
    }

private:
    ServerConfig make_config(std::string enrollment_token)
    {
        thewatcher::crypto::init();
        const auto ts = std::to_string(now_ms());
        const auto base = std::filesystem::temp_directory_path() / ("thewatcher-enrollsec-test-" + ts);

        ServerConfig cfg;
        cfg.bind_address = free_zmq_endpoint();
        cfg.enrollment_address = free_zmq_endpoint();
        cfg.api_host = "127.0.0.1";
        cfg.api_port = free_http_port();
        cfg.db_path = base.string() + ".db";
        cfg.server_public_key.clear();
        cfg.server_secret_key.clear();
        cfg.enrollment_token = std::move(enrollment_token);
        config = cfg;
        return cfg;
    }
};

} // namespace

SCENARIO("Enrollment validates attacker-controlled fields")
{
    GIVEN("a running server with no enrollment token configured")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login());

        thewatcher::crypto::init();
        const auto keys = thewatcher::crypto::generate_curve_keypair();

        WHEN("an agent_id with invalid characters is submitted")
        {
            auto request = valid_request("bad\nagent id", keys.public_key_z85);
            auto response = enroll(fx.config.enrollment_address, request);

            THEN("the request is rejected and no record is created")
            {
                REQUIRE(response.has_value());
                REQUIRE_FALSE(response->approved);
                REQUIRE(response->message == "invalid agent_id");
                REQUIRE_FALSE(fx.pending_contains("bad\nagent id"));
            }
        }

        WHEN("an over-long agent_id is submitted")
        {
            auto request = valid_request(std::string(65, 'a'), keys.public_key_z85);
            auto response = enroll(fx.config.enrollment_address, request);

            THEN("the request is rejected")
            {
                REQUIRE(response.has_value());
                REQUIRE(response->message == "invalid agent_id");
            }
        }

        WHEN("an over-long hostname is submitted")
        {
            auto request = valid_request("hostname-test", keys.public_key_z85);
            request.hostname = std::string(256, 'h');
            auto response = enroll(fx.config.enrollment_address, request);

            THEN("the request is rejected")
            {
                REQUIRE(response.has_value());
                REQUIRE(response->message == "invalid hostname");
            }
        }

        WHEN("a validly-formatted request is submitted")
        {
            auto request = valid_request("Agent._-ok1", keys.public_key_z85);
            auto response = enroll(fx.config.enrollment_address, request);

            THEN("it lands in the pending queue")
            {
                REQUIRE(response.has_value());
                REQUIRE_FALSE(response->approved);
                REQUIRE(response->message == "pending approval");
                REQUIRE(fx.pending_contains("Agent._-ok1"));
            }
        }
    }
}

SCENARIO("A configured enrollment token is required on every request")
{
    GIVEN("a running server configured with an enrollment token")
    {
        ServerFixture fx{"test-enrollment-token"};
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login());

        thewatcher::crypto::init();
        const auto keys = thewatcher::crypto::generate_curve_keypair();

        WHEN("a request arrives without a token")
        {
            auto request = valid_request("tokenless-agent", keys.public_key_z85);
            auto response = enroll(fx.config.enrollment_address, request);

            THEN("the request is rejected and no record is created")
            {
                REQUIRE(response.has_value());
                REQUIRE(response->message == "invalid enrollment token");
                REQUIRE_FALSE(fx.pending_contains("tokenless-agent"));
            }
        }

        WHEN("a request arrives with the wrong token")
        {
            auto request = valid_request("wrong-token-agent", keys.public_key_z85);
            request.enrollment_token = "not-the-token";
            auto response = enroll(fx.config.enrollment_address, request);

            THEN("the request is rejected")
            {
                REQUIRE(response.has_value());
                REQUIRE(response->message == "invalid enrollment token");
                REQUIRE_FALSE(fx.pending_contains("wrong-token-agent"));
            }
        }

        WHEN("a request arrives with the correct token")
        {
            auto request = valid_request("token-agent", keys.public_key_z85);
            request.enrollment_token = "test-enrollment-token";
            auto response = enroll(fx.config.enrollment_address, request);

            THEN("the request is accepted into the pending queue")
            {
                REQUIRE(response.has_value());
                REQUIRE(response->message == "pending approval");
                REQUIRE(fx.pending_contains("token-agent"));
            }
        }
    }
}

SCENARIO("A pending record's public key is immutable")
{
    GIVEN("a running server with a pending agent enrolled under key K1")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login());

        thewatcher::crypto::init();
        const auto keys1 = thewatcher::crypto::generate_curve_keypair();
        const auto keys2 = thewatcher::crypto::generate_curve_keypair();

        auto first = enroll(fx.config.enrollment_address, valid_request("hijack-target", keys1.public_key_z85));
        REQUIRE(first.has_value());
        REQUIRE(first->message == "pending approval");

        WHEN("another enrollment for the same agent_id presents a different key")
        {
            // Clear the per-agent rate limit window so the attack enrollment
            // reaches the key check rather than being rate-limited.
            std::this_thread::sleep_for(11s);
            auto attack = enroll(fx.config.enrollment_address, valid_request("hijack-target", keys2.public_key_z85));

            THEN("the request is rejected with a key-mismatch error")
            {
                REQUIRE(attack.has_value());
                REQUIRE(attack->approved == false);
                REQUIRE(attack->message.find("public key mismatch") != std::string::npos);
            }

            AND_THEN("the original key still authenticates the pending record")
            {
                // Clear the rate limit window again; the original key must be
                // accepted, proving the stored key was never overwritten.
                std::this_thread::sleep_for(11s);
                auto retry = enroll(fx.config.enrollment_address, valid_request("hijack-target", keys1.public_key_z85));
                REQUIRE(retry.has_value());
                REQUIRE(retry->message == "pending approval");
            }
        }
    }
}

SCENARIO("Re-enrolling an approved agent does not refresh last_seen")
{
    GIVEN("a running server with an approved agent")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login());

        thewatcher::crypto::init();
        const auto keys = thewatcher::crypto::generate_curve_keypair();

        auto first = enroll(fx.config.enrollment_address, valid_request("approved-agent", keys.public_key_z85));
        REQUIRE(first.has_value());
        REQUIRE(fx.approve("approved-agent"));

        const auto before = fx.agent_last_seen("approved-agent");
        REQUIRE(before.has_value());

        WHEN("the approved agent re-enrolls")
        {
            // Rate limit: same agent_id, so wait out the 10 s window to make
            // this a genuine re-enrollment rather than a rate-limited one.
            std::this_thread::sleep_for(11s);
            auto again = enroll(fx.config.enrollment_address, valid_request("approved-agent", keys.public_key_z85));

            THEN("the reply is approved but last_seen is unchanged")
            {
                REQUIRE(again.has_value());
                REQUIRE(again->approved == true);
                REQUIRE(fx.agent_last_seen("approved-agent") == before);
                REQUIRE(fx.error == nullptr);
            }
        }
    }
}