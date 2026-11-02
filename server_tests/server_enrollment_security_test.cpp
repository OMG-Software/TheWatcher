// Enrollment hardening tests (SEC-05 / SEC-06).
//
// The enrollment socket is the one deliberately unauthenticated surface:
// any host that can reach it can send requests. These tests pin the
// validation, token gate, pending-queue immutability and last_seen rules
// that keep that surface from being used to flood storage, hijack pending
// records or suppress dead-agent alerts.
#include "common/crypto.hpp"
#include "common/protocol.hpp"
#include "test_support/enrollment_client.hpp"
#include "test_support/server_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using namespace thewatcher::proto;
using namespace thewatcher::test;
using json = nlohmann::json;

namespace
{

EnrollRequest valid_request(const std::string& agent_id, const std::string& z85_key)
{
    return make_enroll_request(agent_id, z85_key);
}

// A running server plus an admin HTTP client.
struct EnrollmentFixture : ServerFixture
{
    explicit EnrollmentFixture(std::string enrollment_token = {})
        : ServerFixture(ServerFixtureOptions::with_enrollment_token(std::move(enrollment_token)))
    {
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
};

} // namespace

SCENARIO("Enrollment validates attacker-controlled fields")
{
    GIVEN("a running server with no enrollment token configured")
    {
        EnrollmentFixture fx;
        fx.start_and_login();

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
        EnrollmentFixture fx{"test-enrollment-token"};
        fx.start_and_login();

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
        EnrollmentFixture fx;
        fx.start_and_login();

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
        EnrollmentFixture fx;
        fx.start_and_login();

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