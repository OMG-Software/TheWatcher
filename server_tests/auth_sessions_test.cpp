// Session revocation, expired-session purging and authentication audit
// logging (issues #145 and #70).
#include "../server/api.hpp"
#include "../server/store.hpp"
#include "../server/zap_handler.hpp"
#include "common/SingleLog.hpp"
#include "test_support/net_helpers.hpp"
#include "test_support/temp_dir.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include <httplib.h>
#include <sodium.h>
#include <zmq.hpp>

using namespace std::chrono_literals;
using json = nlohmann::json;
using thewatcher::server::SessionRecord;
using thewatcher::test::free_http_port;

namespace
{

constexpr int64_t hour_ms = 60LL * 60LL * 1000LL;

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string hash_for(const std::string& password)
{
    REQUIRE(sodium_init() >= 0);
    char hash[crypto_pwhash_STRBYTES] = {};
    REQUIRE(crypto_pwhash_str(hash, password.c_str(), password.size(), crypto_pwhash_OPSLIMIT_MIN,
                              crypto_pwhash_MEMLIMIT_MIN) == 0);
    return hash;
}

SessionRecord make_session(const std::string& token, int64_t user_id, int64_t expires_at)
{
    SessionRecord s;
    s.token = token;
    s.user_id = user_id;
    s.created_at = 0;
    s.expires_at = expires_at;
    return s;
}

// An isolated API server over an in-memory store.
struct ApiFixture
{
    ApiFixture()
        : store(thewatcher::server::make_store("sqlite", ":memory:"))
        , api(*store, zap, "127.0.0.1", port)
        , client("http://127.0.0.1:" + std::to_string(port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);
        api.start();
    }

    ~ApiFixture()
    {
        api.stop();
    }

    // Logs in and returns the "tw_session=<token>" cookie, or "" on failure.
    std::string login(const std::string& username, const std::string& password)
    {
        auto res = client.Post("/api/login",
                               json{
                                   {"username", username},
                                   {"password", password}
        }
                                   .dump(),
                               "application/json");
        if (!res || res->status != 200)
            return {};
        const auto set_cookie = res->get_header_value("Set-Cookie");
        return set_cookie.substr(0, set_cookie.find(';'));
    }

    // Status of GET /api/session with the given cookie.
    int session_status(const std::string& cookie)
    {
        const httplib::Headers headers{
            {"Cookie", cookie}
        };
        auto res = client.Get("/api/session", headers);
        REQUIRE(res);
        return res->status;
    }

    int64_t create_user(const std::string& cookie, const std::string& username, const std::string& password,
                        const std::string& role = "global_viewer")
    {
        auto res = client.Post("/api/users",
                               {
                                   {"Cookie", cookie}
        },
                               json{{"username", username}, {"password", password}, {"role", role}}.dump(),
                               "application/json");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        return json::parse(res->body).at("user_id").get<int64_t>();
    }

    int put(const std::string& cookie, const std::string& path, const std::string& body = "{}")
    {
        auto res = client.Put(path,
                              {
                                  {"Cookie", cookie}
        },
                              body, "application/json");
        REQUIRE(res);
        return res->status;
    }

    int del(const std::string& cookie, const std::string& path)
    {
        const httplib::Headers headers{
            {"Cookie", cookie}
        };
        auto res = client.Delete(path, headers);
        REQUIRE(res);
        return res->status;
    }

    int port = free_http_port();
    zmq::context_t ctx{1};
    std::unique_ptr<thewatcher::server::Store> store;
    thewatcher::server::ZapHandler zap{ctx};
    thewatcher::server::ApiServer api;
    httplib::Client client;
};

// Redirects the process-wide log file to a scratch file for the lifetime of
// the object so audit lines can be read back.
struct CapturedLog
{
    CapturedLog()
        : path(dir / "audit.log")
    {
        auto& log = thewatcher::logging::SingleLog::GetInstance();
        log.SetFileLogLevel(thewatcher::logging::LogLevel::L_NOTICE);
        log.SetLogFilePath(path.string());
    }

    std::string read() const
    {
        std::ifstream in(path);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    // True once the log contains `needle` (the writer thread is asynchronous).
    bool eventually_contains(const std::string& needle) const
    {
        return thewatcher::test::eventually(
            [&] {
                return read().find(needle) != std::string::npos;
            },
            5000ms, 50ms);
    }

    // `dir` is declared first so it exists when `path` is initialised.
    thewatcher::test::ScopedTempDir dir{"auth-audit"};
    std::filesystem::path path;
};

} // namespace

SCENARIO("The store revokes and purges sessions")
{
    GIVEN("a store with two users and several sessions")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto alice = store->create_user("alice", hash_for("pw"), "global_viewer");
        const auto bob = store->create_user("bob", hash_for("pw"), "global_viewer");
        const int64_t now = 1'000'000;
        store->create_session(make_session("alice-1", alice, now + hour_ms));
        store->create_session(make_session("alice-2", alice, now + hour_ms));
        store->create_session(make_session("bob-1", bob, now + hour_ms));
        store->create_session(make_session("alice-old", alice, now - 1));
        store->create_session(make_session("bob-old", bob, now - hour_ms));

        WHEN("all of one user's sessions are revoked")
        {
            const auto removed = store->delete_sessions_for_user(alice);

            THEN("only that user's sessions are gone")
            {
                REQUIRE(removed == 3);
                REQUIRE_FALSE(store->get_session("alice-1", now));
                REQUIRE_FALSE(store->get_session("alice-2", now));
                REQUIRE(store->get_session("bob-1", now));
            }
        }

        WHEN("a user's sessions are revoked except one token")
        {
            const auto removed = store->delete_sessions_for_user(alice, "alice-1");

            THEN("the kept session stays valid and the others are removed")
            {
                REQUIRE(removed == 2);
                REQUIRE(store->get_session("alice-1", now));
                REQUIRE_FALSE(store->get_session("alice-2", now));
            }
        }

        WHEN("expired sessions are purged")
        {
            const auto removed = store->delete_expired_sessions(now);

            THEN("only expired rows are removed and live sessions survive")
            {
                REQUIRE(removed == 2);
                REQUIRE(store->get_session("alice-1", now));
                REQUIRE(store->get_session("bob-1", now));
                REQUIRE(store->delete_expired_sessions(now) == 0);
            }
        }
    }
}

SCENARIO("Sessions are revoked when a password changes")
{
    GIVEN("an admin and a second user, each with a live session")
    {
        ApiFixture fx;
        const auto admin = fx.login("thewatcher", "look_at_me");
        REQUIRE_FALSE(admin.empty());
        const auto user_id = fx.create_user(admin, "carol", "carol-pass-1");
        const auto carol_a = fx.login("carol", "carol-pass-1");
        const auto carol_b = fx.login("carol", "carol-pass-1");
        REQUIRE_FALSE(carol_a.empty());
        REQUIRE_FALSE(carol_b.empty());

        WHEN("the admin resets that user's password")
        {
            REQUIRE(fx.put(admin, "/api/users/" + std::to_string(user_id) + "/password",
                           R"({"password":"carol-pass-2"})") == 200);

            THEN("every session of that user is revoked and the admin keeps theirs")
            {
                REQUIRE(fx.session_status(carol_a) == 401);
                REQUIRE(fx.session_status(carol_b) == 401);
                REQUIRE(fx.session_status(admin) == 200);

                AND_THEN("the new password logs in")
                {
                    REQUIRE_FALSE(fx.login("carol", "carol-pass-2").empty());
                }
            }
        }

        WHEN("the user changes their own password from one session")
        {
            REQUIRE(fx.put(carol_a, "/api/users/" + std::to_string(user_id) + "/password",
                           R"({"password":"carol-pass-3"})") == 200);

            THEN("the current session survives and the user's other sessions are revoked")
            {
                REQUIRE(fx.session_status(carol_a) == 200);
                REQUIRE(fx.session_status(carol_b) == 401);
            }
        }
    }
}

SCENARIO("Sessions are revoked when a user is disabled or deleted")
{
    GIVEN("a user with a live session")
    {
        ApiFixture fx;
        const auto admin = fx.login("thewatcher", "look_at_me");
        const auto user_id = fx.create_user(admin, "dave", "dave-pass");
        const auto dave = fx.login("dave", "dave-pass");
        REQUIRE_FALSE(dave.empty());

        WHEN("the user is disabled and later re-enabled")
        {
            REQUIRE(fx.put(admin, "/api/users/" + std::to_string(user_id) + "/disable") == 200);
            REQUIRE(fx.session_status(dave) == 401);
            REQUIRE(fx.put(admin, "/api/users/" + std::to_string(user_id) + "/enable") == 200);

            THEN("the old session does not come back")
            {
                REQUIRE(fx.session_status(dave) == 401);

                AND_THEN("a fresh login works")
                {
                    REQUIRE_FALSE(fx.login("dave", "dave-pass").empty());
                }
            }
        }

        WHEN("the user is deleted")
        {
            REQUIRE(fx.del(admin, "/api/users/" + std::to_string(user_id)) == 200);

            THEN("the session is no longer valid")
            {
                REQUIRE(fx.session_status(dave) == 401);
            }
        }
    }
}

SCENARIO("Expired sessions are purged opportunistically")
{
    GIVEN("an API server whose store holds an already expired session")
    {
        ApiFixture fx;
        const auto admin_id = [&] {
            for (const auto& u : fx.store->list_users())
                if (u.username == "thewatcher")
                    return u.user_id;
            return int64_t{0};
        }();
        REQUIRE(admin_id != 0);
        fx.store->create_session(make_session("long-expired", admin_id, 1));

        WHEN("someone logs in")
        {
            REQUIRE_FALSE(fx.login("thewatcher", "look_at_me").empty());

            THEN("the expired row has been removed from the store")
            {
                // Nothing left to purge means the login already did it.
                REQUIRE(fx.store->delete_expired_sessions(now_ms()) == 0);
            }
        }
    }
}

SCENARIO("Authentication events are audit logged")
{
    GIVEN("an API server and a captured log")
    {
        CapturedLog log;
        ApiFixture fx;

        WHEN("a login fails for a username containing control characters")
        {
            auto res = fx.client.Post(
                "/api/login",
                json{
                    {"username", "evil\nAUDIT event=login outcome=success user=\"root\""},
                    {"password", "x"                                                    }
            }
                    .dump(),
                "application/json");
            REQUIRE(res);
            REQUIRE(res->status == 401);

            THEN("one failure line is written and the username cannot forge another line")
            {
                REQUIRE(log.eventually_contains("event=login outcome=failure"));
                const auto text = log.read();
                REQUIRE(text.find("evil\nAUDIT") == std::string::npos);
                // The forged text stays inside the quoted user field of the
                // single failure line: no line reports a success.
                std::istringstream lines(text);
                for (std::string line; std::getline(lines, line);)
                    if (line.find("outcome=success") != std::string::npos)
                        REQUIRE(line.find("outcome=failure") != std::string::npos);
            }
        }

        WHEN("the admin logs in, logs out and is denied a privileged action")
        {
            const auto admin = fx.login("thewatcher", "look_at_me");
            fx.create_user(admin, "erin", "erin-pass");
            const auto erin = fx.login("erin", "erin-pass");
            REQUIRE(fx.put(erin, "/api/users/1/disable") == 403);
            auto logout = fx.client.Post("/api/logout",
                                         {
                                             {"Cookie", admin}
            },
                                         "", "application/json");
            REQUIRE(logout);

            THEN("success, denial, user creation and logout are all recorded with the acting user")
            {
                REQUIRE(log.eventually_contains("event=logout"));
                const auto text = log.read();
                REQUIRE(text.find("event=login outcome=success user=\"thewatcher\"") != std::string::npos);
                REQUIRE(text.find("event=permission_denied") != std::string::npos);
                REQUIRE(text.find("event=user_create") != std::string::npos);
                REQUIRE(text.find("target=\"erin\"") != std::string::npos);
            }

            THEN("no password is ever logged")
            {
                const auto text = log.read();
                REQUIRE(text.find("look_at_me") == std::string::npos);
                REQUIRE(text.find("erin-pass") == std::string::npos);
            }
        }
    }
}
