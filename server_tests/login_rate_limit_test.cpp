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
#include "test_support/net_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <zmq.hpp>

using namespace std::chrono_literals;
using json = nlohmann::json;
using thewatcher::test::free_http_port;

namespace
{

// An isolated API server with shortened rate-limit windows (defaults are
// 15 min / 60 s) so window expiry can be observed in test time. The default
// 3 s is deliberately generous: five Argon2 verifications must fit inside
// one window even on a loaded host.
struct ApiFixture
{
    explicit ApiFixture(int64_t window_ms = 3000, std::vector<std::string> trusted_proxies = {})
        : store(thewatcher::server::make_store("sqlite", ":memory:"))
        , api(*store, zap, "127.0.0.1", port, window_ms, window_ms)
        , client("http://127.0.0.1:" + std::to_string(port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
        api.set_trusted_proxies(std::move(trusted_proxies));
        api.start();
    }

    ~ApiFixture()
    {
        api.stop();
    }

    // Declared first so it is initialised before `api` uses it.
    int port = free_http_port();
    zmq::context_t ctx{1};
    std::unique_ptr<thewatcher::server::Store> store;
    thewatcher::server::ZapHandler zap{ctx};
    thewatcher::server::ApiServer api;
    httplib::Client client;
};

int attempt_login(httplib::Client& client, const std::string& username, const std::string& password,
                  const httplib::Headers& headers = {})
{
    auto res = client.Post("/api/login", headers,
                           json{
                               {"username", username},
                               {"password", password}
    }
                               .dump(),
                           "application/json");
    REQUIRE(res);
    return res->status;
}

// Login as seen through a trusted reverse proxy that reports `client_ip`.
int attempt_login_via_proxy(httplib::Client& client, const std::string& client_ip, const std::string& username,
                            const std::string& password)
{
    return attempt_login(client, username, password,
                         {
                             {"X-Forwarded-For", client_ip}
    });
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
                    std::this_thread::sleep_for(3300ms);
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
                std::this_thread::sleep_for(3300ms);

                THEN("logins work again")
                {
                    REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 200);
                }
            }
        }
    }
}
SCENARIO("Behind a trusted proxy, throttling is per client address")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("one client address fails five logins for the admin account")
        {
            for (int i = 0; i < 5; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.10", "thewatcher", "wrong-password") == 401);

            THEN("that client is locked out")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.10", "thewatcher", "wrong-password") == 429);
            }

            THEN("a spoofed leftmost X-Forwarded-For entry does not evade the lockout")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "192.0.2.66, 203.0.113.10", "thewatcher", "wrong-password") ==
                        429);
            }

            THEN("a different client address can still log in as the same user")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.11", "thewatcher", "look_at_me") == 200);
            }
        }

        WHEN("one client address exhausts the per-IP throttle")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.20", "spray-user-" + std::to_string(i),
                                                "wrong-password") == 401);

            THEN("that client is throttled but another client is not")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.20", "thewatcher", "look_at_me") == 429);
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.21", "thewatcher", "look_at_me") == 200);
            }
        }

        WHEN("failures arrive with no forwarding headers")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login(fx.client, "spray-user-" + std::to_string(i), "wrong-password") == 401);

            THEN("the proxy's own address is throttled")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 429);
            }
        }
    }
}

SCENARIO("Without trusted proxies, forwarding headers are ignored")
{
    GIVEN("an API server with no trusted proxies")
    {
        ApiFixture fx(60'000);

        WHEN("ten bad logins each claim a different client address")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "198.51.100." + std::to_string(i),
                                                "spray-user-" + std::to_string(i), "wrong-password") == 401);

            THEN("the per-IP throttle still applies to the real peer address")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "198.51.100.200", "thewatcher", "look_at_me") == 429);
            }
        }

        WHEN("five bad logins for one account each claim a different client address")
        {
            for (int i = 0; i < 5; ++i)
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password",
                                      {
                                          {"X-Forwarded-For", "198.51.100." + std::to_string(i)},
                                          {"X-Real-IP",       "198.51.100." + std::to_string(i)}
                }) == 401);

            THEN("the account is locked out for the real peer address")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password",
                                      {
                                          {"X-Forwarded-For", "198.51.100.99"}
                }) == 429);
            }
        }
    }
}

SCENARIO("A username is limited account-wide across many client addresses")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("fifty different clients each fail once for the same username")
        {
            // One failure per client address: neither the per-(IP, user)
            // lockout nor the per-IP throttle can fire. The username does
            // not exist, so no password hashing slows this down.
            for (int i = 1; i <= 50; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "192.0.2." + std::to_string(i), "ghost", "wrong-password") ==
                        401);

            THEN("a fifty-first client is refused for that username")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "192.0.2.200", "ghost", "wrong-password") == 429);

                AND_THEN("other usernames are unaffected")
                {
                    REQUIRE(attempt_login_via_proxy(fx.client, "192.0.2.200", "thewatcher", "look_at_me") == 200);
                }
            }
        }
    }
}
