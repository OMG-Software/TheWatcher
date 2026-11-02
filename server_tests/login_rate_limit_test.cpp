// Login rate limiter tests (SEC-12).
//
// The original limiter never reset its window: after the first 15-minute
// lockout expired, the failure count stayed above the threshold but the
// window start was never refreshed, so the lock could never re-arm —
// unlimited password guessing after the first window. These tests pin the
// fixed-window reset and the per-source-IP throttle, using shortened
// windows so expiry is observable in test time.
#include "../server/api.hpp"
#include "../server/store.hpp"
#include "../server/zap_handler.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using json = nlohmann::json;

namespace
{

// An isolated API server with 300 ms rate-limit windows (defaults are
// 15 min / 60 s) so lockout expiry can be observed quickly.
struct ApiFixture
{
    ApiFixture()
        : store(thewatcher::server::make_store("sqlite", ":memory:"))
        , api(*store, zap, "127.0.0.1", port, /*login_lockout_ms=*/300, /*login_ip_window_ms=*/300)
        , client("http://127.0.0.1:" + std::to_string(port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
        api.start();
    }

    ~ApiFixture()
    {
        api.stop();
    }

    static constexpr int port = 28790;

    zmq::context_t ctx{1};
    std::unique_ptr<thewatcher::server::Store> store;
    thewatcher::server::ZapHandler zap{ctx};
    thewatcher::server::ApiServer api;
    httplib::Client client;
};

int attempt_login(httplib::Client& client, const std::string& username, const std::string& password)
{
    auto res = client.Post("/api/login",
                           json{
                               {"username", username},
                               {"password", password}
    }
                               .dump(),
                           "application/json");
    REQUIRE(res);
    return res->status;
}

} // namespace

SCENARIO("The username lockout re-arms after the window expires")
{
    GIVEN("an API server with a short lockout window and the default admin")
    {
        ApiFixture fx;

        WHEN("five bad passwords arrive")
        {
            for (int i = 0; i < 5; ++i)
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 401);

            THEN("the sixth attempt is locked out")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 429);

                AND_WHEN("the lockout window expires and five more bad passwords arrive")
                {
                    std::this_thread::sleep_for(400ms);
                    for (int i = 0; i < 5; ++i)
                        REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 401);

                    THEN("the lockout engages again — the window resets")
                    {
                        REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 429);
                    }
                }
            }
        }
    }
}

SCENARIO("The per-IP throttle spans usernames")
{
    GIVEN("an API server with a short IP throttle window and the default admin")
    {
        ApiFixture fx;

        WHEN("ten bad logins arrive across ten different usernames")
        {
            // One failure per username: no username lockout can fire, so a
            // 429 on the next attempt can only come from the IP throttle.
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login(fx.client, "spray-user-" + std::to_string(i), "wrong-password") == 401);

            THEN("the next attempt is throttled regardless of username")
            {
                REQUIRE(attempt_login(fx.client, "spray-user-11", "wrong-password") == 429);

                AND_THEN("even the correct admin password is throttled from this address")
                {
                    REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 429);
                }
            }

            AND_WHEN("the IP window expires")
            {
                std::this_thread::sleep_for(400ms);

                THEN("logins work again")
                {
                    REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 200);
                }
            }
        }
    }
}