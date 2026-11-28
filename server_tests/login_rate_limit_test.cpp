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
#include "test_support/server_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
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
    // `configure` runs before the listener starts (for test hooks). Anything
    // its callbacks capture must outlive the fixture.
    explicit ApiFixture(int64_t window_ms = 3000, std::vector<std::string> trusted_proxies = {},
                        const std::function<void(thewatcher::server::ApiServer&)>& configure = {})
        : store(thewatcher::server::make_store("sqlite", ":memory:"))
        , api(*store, zap, "127.0.0.1", port, window_ms, window_ms)
        , client("http://127.0.0.1:" + std::to_string(port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
        api.set_trusted_proxies(std::move(trusted_proxies));
        if (configure)
            configure(api);
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
                REQUIRE(attempt_login_via_proxy(fx.client, "192.0.2.66, 203.0.113.10", "thewatcher",
                                                "wrong-password") == 429);
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

SCENARIO("A trusted proxy that forwards a malformed address cannot lock out other clients")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("an attacker sends garbage in X-Forwarded-For until their own bucket is throttled")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "zzz", "spray-user-" + std::to_string(i),
                                                "wrong-password") == 401);

            THEN("the malformed requests themselves are throttled")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "zzz", "thewatcher", "look_at_me") == 429);
            }

            THEN("a legitimate client behind the same proxy can still log in")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.30", "thewatcher", "look_at_me") == 200);
            }

            THEN("requests the proxy forwards without any client header are not throttled")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 200);
            }
        }
    }
}

SCENARIO("X-Real-IP set by a trusted proxy takes precedence over X-Forwarded-For")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("five failures carry the same X-Real-IP but a different client-supplied X-Forwarded-For each time")
        {
            for (int i = 0; i < 5; ++i)
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password",
                                      {
                                          {"X-Real-IP",       "203.0.113.40"                       },
                                          {"X-Forwarded-For", "198.51.100." + std::to_string(i + 1)}
                }) == 401);

            THEN("the real client is locked out despite the rotating X-Forwarded-For")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password",
                                      {
                                          {"X-Real-IP",       "203.0.113.40" },
                                          {"X-Forwarded-For", "198.51.100.77"}
                }) == 429);
                // The same peer with a different X-Real-IP is a different
                // client, so the bucket is the forwarded address, not the peer.
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me",
                                      {
                                          {"X-Real-IP",       "203.0.113.41" },
                                          {"X-Forwarded-For", "198.51.100.77"}
                }) == 200);
            }
        }
    }
}

SCENARIO("Every X-Forwarded-For header line is considered, the proxy's line last")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("the client sends its own X-Forwarded-For line and the proxy adds a second one")
        {
            // HAProxy's `option forwardfor` adds a separate header line.
            for (int i = 0; i < 5; ++i)
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password",
                                      {
                                          {"X-Forwarded-For", "198.51.100." + std::to_string(i + 1)},
                                          {"X-Forwarded-For", "203.0.113.50"                       }
                }) == 401);

            THEN("the address on the proxy's line is the one that is locked out")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password",
                                      {
                                          {"X-Forwarded-For", "198.51.100.99"},
                                          {"X-Forwarded-For", "203.0.113.50" }
                }) ==
                        429);
                // A different address on the proxy's line is a different
                // client, so the bucket is the forwarded address, not the peer.
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me",
                                      {
                                          {"X-Forwarded-For", "198.51.100.99"},
                                          {"X-Forwarded-For", "203.0.113.51" }
                }) ==
                        200);
            }
        }
    }
}

SCENARIO("Trusted proxies may be given as CIDR ranges")
{
    GIVEN("an API server that trusts the 127.0.0.0/8 range")
    {
        ApiFixture fx(60'000, {"127.0.0.0/8"});

        WHEN("five failures arrive via the proxy for one forwarded client")
        {
            for (int i = 0; i < 5; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.60", "thewatcher", "wrong-password") == 401);

            THEN("that forwarded client is locked out, so the range was honoured")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.60", "thewatcher", "wrong-password") == 429);
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.61", "thewatcher", "look_at_me") == 200);
            }
        }
    }

    GIVEN("an API server whose trusted entries are invalid or do not contain the peer")
    {
        ApiFixture fx(60'000, {"192.0.2.0/24", "not-an-address", "192.0.2.0/99"});

        WHEN("ten failures each claim a different forwarded client")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "198.51.100." + std::to_string(i + 1),
                                                "spray-user-" + std::to_string(i), "wrong-password") == 401);

            THEN("the headers are ignored and the real peer is throttled")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "198.51.100.200", "thewatcher", "look_at_me") == 429);
            }
        }
    }
}

SCENARIO("Unknown and disabled usernames cost as much as a wrong password")
{
    GIVEN("an API server with the default admin and a disabled user")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});
        REQUIRE(thewatcher::test::login_as_default_admin(fx.client));
        auto created =
            fx.client.Post("/api/users",
                           json{
                               {"username", "sleeper"      },
                               {"password", "sleeper-pass" },
                               {"role",     "global_viewer"}
        }
                               .dump(),
                           "application/json");
        REQUIRE(created);
        REQUIRE(created->status == 200);
        const auto user_id = json::parse(created->body).at("user_id").get<int64_t>();
        auto disabled = fx.client.Put("/api/users/" + std::to_string(user_id) + "/disable", "", "application/json");
        REQUIRE(disabled);
        REQUIRE(disabled->status == 200);
        httplib::Client anon("http://127.0.0.1:" + std::to_string(fx.port));
        anon.set_read_timeout(10, 0);

        // Median login time of three failures, each from its own forwarded
        // address so the throttle stays out of the measurement.
        const auto median_ms = [&](const std::string& user, const std::string& pass, const std::string& prefix) {
            std::vector<double> samples;
            for (int i = 0; i < 3; ++i)
            {
                const auto start = std::chrono::steady_clock::now();
                REQUIRE(attempt_login(anon, user, pass,
                                      {
                                          {"X-Forwarded-For", prefix + std::to_string(i + 1)}
                }) == 401);
                samples.push_back(
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            std::sort(samples.begin(), samples.end());
            return samples[1];
        };

        WHEN("a login is attempted for an existing user with the wrong password")
        {
            const auto known = median_ms("thewatcher", "wrong-password", "198.51.100.");

            THEN("an unknown username takes a comparable time")
            {
                REQUIRE(median_ms("no-such-user", "wrong-password", "198.51.101.") > known * 0.5);
            }

            THEN("a disabled username takes a comparable time")
            {
                REQUIRE(median_ms("sleeper", "sleeper-pass", "198.51.102.") > known * 0.5);
            }
        }
    }
}

SCENARIO("Concurrent login attempts cannot exceed the failure limit")
{
    GIVEN("an API server that holds every admitted login before it verifies the password")
    {
        constexpr int attempts = 16;
        constexpr int limit = 5;
        // Declared before the fixture so they outlive the server's threads.
        std::atomic<int> admitted{0};
        std::promise<void> gate;
        auto gate_open = gate.get_future().share();
        ApiFixture fx(60'000, {}, [&](thewatcher::server::ApiServer& api) {
            api.set_login_test_hooks({}, [&](const std::string&) {
                ++admitted;
                gate_open.wait_for(10s);
            });
        });
        std::atomic<int> unauthorized{0};
        std::atomic<int> throttled{0};
        std::vector<std::thread> threads;

        WHEN("sixteen bad logins for one account from one address are in flight at once")
        {
            for (int i = 0; i < attempts; ++i)
            {
                threads.emplace_back([&] {
                    httplib::Client client("http://127.0.0.1:" + std::to_string(fx.port));
                    client.set_read_timeout(30, 0);
                    auto res = client.Post("/api/login",
                                           json{
                                               {"username", "thewatcher"    },
                                               {"password", "wrong-password"}
                    }
                                               .dump(),
                                           "application/json");
                    if (res && res->status == 401)
                        ++unauthorized;
                    else if (res && res->status == 429)
                        ++throttled;
                });
            }

            // The admitted attempts stay parked, so the rest are judged while
            // the admitted ones are still unverified: only reservations made
            // up front can already have turned them away.
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (throttled.load() < attempts - limit && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            const int admitted_while_parked = admitted.load();
            const int throttled_while_parked = throttled.load();
            gate.set_value();
            for (auto& t : threads)
                t.join();

            THEN("only the per-account limit of five were admitted and the rest were refused at once")
            {
                REQUIRE(admitted_while_parked == limit);
                REQUIRE(throttled_while_parked == attempts - limit);
                REQUIRE(unauthorized.load() == limit);
                REQUIRE(throttled.load() == attempts - limit);
            }
        }
    }
}

SCENARIO("A trusted proxy that forwards an empty address list cannot lock out other clients")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("ten failures arrive with an X-Forwarded-For that holds no address")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, ",,", "spray-user-" + std::to_string(i), "wrong-password") ==
                        401);

            THEN("only the empty-header bucket is throttled, not the proxy address")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, ",,", "thewatcher", "look_at_me") == 429);
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 200);
            }
        }

        WHEN("the forwarded list contains only trusted proxies")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "127.0.0.1", "spray-user-" + std::to_string(i),
                                                "wrong-password") == 401);

            THEN("the request is attributed to the proxy address itself")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 429);
            }
        }
    }
}

SCENARIO("A trusted IPv6 range does not trust IPv4 peers")
{
    GIVEN("an API server that trusts every IPv6 address but no IPv4 address")
    {
        ApiFixture fx(60'000, {"::/0"});

        WHEN("ten failures from an IPv4 peer each claim a different forwarded client")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "198.51.100." + std::to_string(i + 1),
                                                "spray-user-" + std::to_string(i), "wrong-password") == 401);

            THEN("the headers are ignored and the real peer is throttled")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "198.51.100.200", "thewatcher", "look_at_me") == 429);
            }
        }
    }
}

SCENARIO("A successful login only releases its own throttle reservation")
{
    GIVEN("an API server and four completed failures for the admin account")
    {
        ApiFixture fx(60'000);
        for (int i = 0; i < 4; ++i)
            REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 401);

        WHEN("a correct login succeeds and one more failure follows")
        {
            REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 200);
            REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 401);

            THEN("the earlier failures still count, so the account is now locked out")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 429);
            }
        }
    }
}

SCENARIO("A forwarded chain with an empty entry is malformed even next to a trusted hop")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("ten failures send a trusted hop followed by an empty entry")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "127.0.0.1,", "spray-user-" + std::to_string(i),
                                                "wrong-password") == 401);

            THEN("they are throttled on their own and the proxy address stays usable")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "127.0.0.1,", "thewatcher", "look_at_me") == 429);
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 200);
            }
        }
    }

    GIVEN("a second API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("ten failures send an empty entry followed by a trusted hop")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, ",127.0.0.1", "spray-user-" + std::to_string(i),
                                                "wrong-password") == 401);

            THEN("they are throttled on their own and the proxy address stays usable")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, ",127.0.0.1", "thewatcher", "look_at_me") == 429);
                REQUIRE(attempt_login(fx.client, "thewatcher", "look_at_me") == 200);
            }
        }
    }
}

SCENARIO("A leading empty X-Forwarded-For line makes the header malformed")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("ten failures send an empty header line followed by a line with an address")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login(fx.client, "spray-user-" + std::to_string(i), "wrong-password",
                                      {
                                          {"X-Forwarded-For", ""           },
                                          {"X-Forwarded-For", "203.0.113.7"}
                }) == 401);

            THEN("they are throttled as unknown-client, not as the address on the second line")
            {
                REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password",
                                      {
                                          {"X-Forwarded-For", ""           },
                                          {"X-Forwarded-For", "203.0.113.7"}
                }) == 429);
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.7", "thewatcher", "look_at_me") == 200);
            }
        }
    }
}

namespace
{

// Opens the gate and joins the worker however the scope is left, so a failed
// assertion cannot leave a joinable thread (which would terminate the process).
struct GateGuard
{
    std::thread& worker;
    std::promise<void>& gate;
    std::atomic<bool>& opened;

    ~GateGuard()
    {
        if (!opened.exchange(true))
            gate.set_value();
        if (worker.joinable())
            worker.join();
    }
};

} // namespace

SCENARIO("A login that outlives its throttle window cannot release a newer window's slot")
{
    GIVEN("an API server with a controllable clock and a 3 second throttle window")
    {
        // Declared before the fixture so the server's threads never outlive them.
        std::atomic<int64_t> clock_ms{1'000'000};
        std::promise<void> in_flight;
        auto in_flight_future = in_flight.get_future();
        std::promise<void> proceed;
        auto proceed_future = proceed.get_future().share();
        std::atomic<bool> hold_next{true};
        std::atomic<bool> proceed_set{false};
        ApiFixture fx(3000, {}, [&](thewatcher::server::ApiServer& api) {
            api.set_login_test_hooks(
                [&] {
                    return clock_ms.load();
                },
                [&](const std::string&) {
                    // Park only the first attempt, after its slot was counted.
                    if (hold_next.exchange(false))
                    {
                        in_flight.set_value();
                        proceed_future.wait_for(15s);
                    }
                });
        });

        WHEN("a correct login is still verifying while the window expires and a new failure arrives")
        {
            int slow_status = 0;
            std::thread slow([&] {
                httplib::Client client("http://127.0.0.1:" + std::to_string(fx.port));
                client.set_read_timeout(30, 0);
                // No Catch2 assertions off the main thread.
                auto res = client.Post("/api/login",
                                       json{
                                           {"username", "thewatcher"},
                                           {"password", "look_at_me"}
                }
                                           .dump(),
                                       "application/json");
                slow_status = res ? res->status : 0;
            });
            GateGuard guard{slow, proceed, proceed_set};
            REQUIRE(in_flight_future.wait_for(10s) == std::future_status::ready);
            clock_ms += 10'000; // the first attempt's window has long expired
            REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 401);
            if (!proceed_set.exchange(true))
                proceed.set_value();
            slow.join();
            REQUIRE(slow_status == 200);

            AND_WHEN("four more failures follow in the new window")
            {
                for (int i = 0; i < 4; ++i)
                    REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 401);

                THEN("all five failures count, so the account is locked out")
                {
                    REQUIRE(attempt_login(fx.client, "thewatcher", "wrong-password") == 429);
                }
            }
        }
    }
}

SCENARIO("An invalid entry left of the client address makes the forwarded chain malformed")
{
    GIVEN("an API server that trusts 127.0.0.1 as a reverse proxy")
    {
        ApiFixture fx(60'000, {"127.0.0.1"});

        WHEN("ten failures send junk before a valid client address")
        {
            for (int i = 0; i < 10; ++i)
                REQUIRE(attempt_login_via_proxy(fx.client, "junk, 203.0.113.9", "spray-user-" + std::to_string(i),
                                                "wrong-password") == 401);

            THEN("they are throttled as unknown-client, not as the address after the junk")
            {
                REQUIRE(attempt_login_via_proxy(fx.client, "junk, 203.0.113.9", "thewatcher", "look_at_me") == 429);
                REQUIRE(attempt_login_via_proxy(fx.client, "203.0.113.9", "thewatcher", "look_at_me") == 200);
            }
        }
    }
}
