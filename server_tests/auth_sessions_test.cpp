// Session revocation, expired-session purging and authentication audit
// logging (issues #145 and #70).
#include "../server/api.hpp"
#include "../server/sqlite_raii.hpp"
#include "../server/store.hpp"
#include "../server/store_sqlite.hpp"
#include "../server/store_synchronized.hpp"
#include "../server/zap_handler.hpp"
#include "common/SingleLog.hpp"
#include "test_support/net_helpers.hpp"
#include "test_support/temp_dir.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <httplib.h>
#include <sodium.h>
#include <sqlite3.h>
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
    // `configure` runs before the listener starts (for test hooks); anything its
    // callbacks capture must outlive the fixture.
    explicit ApiFixture(const std::function<void(ApiFixture&)>& configure = {})
        : ApiFixture(thewatcher::server::make_store("sqlite", ":memory:"), configure)
    {
    }

    // The same, over a store the test built (for example a file-backed one).
    explicit ApiFixture(std::unique_ptr<thewatcher::server::Store> backing,
                        const std::function<void(ApiFixture&)>& configure = {})
        : store(std::move(backing))
        , api(*store, zap, "127.0.0.1", port)
        , client("http://127.0.0.1:" + std::to_string(port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(10, 0);
        if (configure)
            configure(*this);
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

    // Blocks until every record written before this call is in the file. The
    // writer is one FIFO queue, so a marker written now reaches the file after
    // everything queued earlier; once it shows up, absence checks are sound.
    void settle()
    {
        const auto marker = "test-settle-marker-" + std::to_string(++settle_count);
        thewatcher::logging::SingleLog::GetInstance().Notice("settle", marker);
        REQUIRE(eventually_contains(marker));
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
    int settle_count = 0;
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
                           R"({"password":"carol-pass-3","current_password":"carol-pass-1"})") == 200);

            THEN("the current session survives and the user's other sessions are revoked")
            {
                REQUIRE(fx.session_status(carol_a) == 200);
                REQUIRE(fx.session_status(carol_b) == 401);
            }
        }
    }
}

SCENARIO("Changing your own password requires the current password")
{
    GIVEN("an admin and a second user, each with two live sessions")
    {
        CapturedLog log;
        ApiFixture fx;
        const auto admin_a = fx.login("thewatcher", "look_at_me");
        const auto admin_b = fx.login("thewatcher", "look_at_me");
        REQUIRE_FALSE(admin_a.empty());
        REQUIRE_FALSE(admin_b.empty());
        const auto carol_id = fx.create_user(admin_a, "carol", "carol-pass-1");
        const auto carol_a = fx.login("carol", "carol-pass-1");
        const auto carol_b = fx.login("carol", "carol-pass-1");
        REQUIRE_FALSE(carol_a.empty());
        REQUIRE_FALSE(carol_b.empty());
        const auto carol_path = "/api/users/" + std::to_string(carol_id) + "/password";

        // Nothing may have changed: the old password still works and no
        // session of the user was revoked.
        const auto expect_unchanged = [&] {
            REQUIRE(fx.session_status(carol_a) == 200);
            REQUIRE(fx.session_status(carol_b) == 200);
            REQUIRE_FALSE(fx.login("carol", "carol-pass-1").empty());
            REQUIRE(fx.login("carol", "carol-pass-new").empty());
        };

        WHEN("the user omits current_password")
        {
            const auto status = fx.put(carol_a, carol_path, R"({"password":"carol-pass-new"})");

            THEN("the request is rejected with 400 and nothing changes")
            {
                REQUIRE(status == 400);
                expect_unchanged();
            }
        }

        WHEN("the user sends a current_password that is not a string")
        {
            const auto status =
                fx.put(carol_a, carol_path, R"({"password":"carol-pass-new","current_password":12345})");

            THEN("the request is rejected with 400 and nothing changes")
            {
                REQUIRE(status == 400);
                expect_unchanged();
            }
        }

        WHEN("the user sends the wrong current_password")
        {
            auto res = fx.client.Put(carol_path,
                                     {
                                         {"Cookie", carol_a}
            },
                                     R"({"password":"carol-pass-new","current_password":"not-my-password"})",
                                     "application/json");
            REQUIRE(res);

            THEN("the request is refused with 403, nothing changes and no session is revoked")
            {
                REQUIRE(res->status == 403);
                REQUIRE(json::parse(res->body).at("error").get<std::string>() == "current password is incorrect");
                expect_unchanged();
            }

            THEN("the failure is audited without logging either password")
            {
                REQUIRE(log.eventually_contains("event=password_change outcome=failure"));
                const auto text = log.read();
                REQUIRE(text.find("not-my-password") == std::string::npos);
                REQUIRE(text.find("carol-pass-new") == std::string::npos);
            }
        }

        WHEN("the user sends the correct current_password")
        {
            REQUIRE(fx.put(carol_a, carol_path, R"({"password":"carol-pass-new","current_password":"carol-pass-1"})") ==
                    200);

            THEN("the password changes, the current session survives and the other is revoked")
            {
                REQUIRE(fx.session_status(carol_a) == 200);
                REQUIRE(fx.session_status(carol_b) == 401);
                REQUIRE_FALSE(fx.login("carol", "carol-pass-new").empty());
                REQUIRE(fx.login("carol", "carol-pass-1").empty());
            }
        }

        WHEN("a global admin changes their own password without the current one")
        {
            const auto status = fx.put(admin_a, "/api/users/1/password", R"({"password":"admin-pass-new"})");

            THEN("the request is rejected with 400 as for any other role")
            {
                REQUIRE(status == 400);
                REQUIRE(fx.session_status(admin_b) == 200);
                REQUIRE_FALSE(fx.login("thewatcher", "look_at_me").empty());
            }
        }

        WHEN("a global admin changes their own password with a wrong current one")
        {
            const auto status =
                fx.put(admin_a, "/api/users/1/password", R"({"password":"admin-pass-new","current_password":"wrong"})");

            THEN("the request is refused with 403 and nothing changes")
            {
                REQUIRE(status == 403);
                REQUIRE(fx.session_status(admin_b) == 200);
                REQUIRE_FALSE(fx.login("thewatcher", "look_at_me").empty());
            }
        }

        WHEN("a global admin changes their own password with the correct current one")
        {
            REQUIRE(fx.put(admin_a, "/api/users/1/password",
                           R"({"password":"admin-pass-new","current_password":"look_at_me"})") == 200);

            THEN("the password changes and the admin's other session is revoked")
            {
                REQUIRE(fx.session_status(admin_a) == 200);
                REQUIRE(fx.session_status(admin_b) == 401);
                REQUIRE_FALSE(fx.login("thewatcher", "admin-pass-new").empty());
            }
        }

        WHEN("a global admin resets another user's password and the body carries a wrong current_password")
        {
            const auto status =
                fx.put(admin_a, carol_path, R"({"password":"carol-pass-new","current_password":"ignored-for-resets"})");

            THEN("the reset succeeds because resets never need the current password")
            {
                REQUIRE(status == 200);
                REQUIRE_FALSE(fx.login("carol", "carol-pass-new").empty());
            }
        }
    }
}

SCENARIO("Wrong current passwords count against the login throttle")
{
    GIVEN("a user with a live session")
    {
        ApiFixture fx;
        const auto admin = fx.login("thewatcher", "look_at_me");
        const auto carol_id = fx.create_user(admin, "carol", "carol-pass-1");
        const auto carol = fx.login("carol", "carol-pass-1");
        REQUIRE_FALSE(carol.empty());
        const auto path = "/api/users/" + std::to_string(carol_id) + "/password";
        const std::string wrong = R"({"password":"carol-pass-new","current_password":"guess"})";
        // Read before any attempt, so the final check does not depend on login.
        const auto stored_hash_before = fx.store->get_user_by_username("carol")->password_hash;

        WHEN("the session guesses the current password five times")
        {
            for (int i = 0; i < 5; ++i)
                REQUIRE(fx.put(carol, path, wrong) == 403);

            THEN("the next attempt is throttled even with the correct password")
            {
                auto res = fx.client.Put(path,
                                         {
                                             {"Cookie", carol}
                },
                                         R"({"password":"carol-pass-new","current_password":"carol-pass-1"})",
                                         "application/json");
                REQUIRE(res);
                REQUIRE(res->status == 429);
                REQUIRE_FALSE(res->get_header_value("Retry-After").empty());
                REQUIRE(fx.session_status(carol) == 200);
                REQUIRE(fx.store->get_user_by_username("carol")->password_hash == stored_hash_before);
                REQUIRE(fx.login("carol", "carol-pass-new").empty());
            }

            THEN("a login for that user from the same address shares the lockout")
            {
                auto res = fx.client.Post("/api/login",
                                          json{
                                              {"username", "carol"       },
                                              {"password", "carol-pass-1"}
                }
                                              .dump(),
                                          "application/json");
                REQUIRE(res);
                REQUIRE(res->status == 429);
            }
        }

        WHEN("the correct current password follows four wrong guesses")
        {
            for (int i = 0; i < 4; ++i)
                REQUIRE(fx.put(carol, path, wrong) == 403);
            const auto status =
                fx.put(carol, path, R"({"password":"carol-pass-new","current_password":"carol-pass-1"})");

            THEN("it succeeds")
            {
                REQUIRE(status == 200);
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
                log.settle(); // the writer is asynchronous: wait for every earlier line
                const auto text = log.read();
                REQUIRE(text.find("look_at_me") == std::string::npos);
                REQUIRE(text.find("erin-pass") == std::string::npos);
            }
        }
    }
}

SCENARIO("A session can only be created for credentials that are still current")
{
    GIVEN("a store with a user whose password hash was read by a login")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto old_hash = hash_for("old-password");
        const auto user_id = store->create_user("frank", old_hash, "global_viewer");
        const int64_t now = 1'000'000;

        WHEN("the hash is still current")
        {
            const auto created =
                store->create_session_if_credentials_current(make_session("s1", user_id, now + hour_ms), old_hash);

            THEN("the session is created")
            {
                REQUIRE(created);
                REQUIRE(store->get_session("s1", now));
            }
        }

        WHEN("the password changed after the login verified the old hash")
        {
            store->update_user_password(user_id, hash_for("new-password"));
            const auto created =
                store->create_session_if_credentials_current(make_session("s2", user_id, now + hour_ms), old_hash);

            THEN("no session is created")
            {
                REQUIRE_FALSE(created);
                REQUIRE_FALSE(store->get_session("s2", now));
            }
        }

        WHEN("the user was disabled after the login verified the hash")
        {
            store->disable_user(user_id);
            const auto created =
                store->create_session_if_credentials_current(make_session("s3", user_id, now + hour_ms), old_hash);

            THEN("no session is created")
            {
                REQUIRE_FALSE(created);
            }
        }
    }
}

SCENARIO("A password change and the revocation of old sessions are one operation")
{
    GIVEN("a user with two sessions")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto user_id = store->create_user("gina", hash_for("old-password"), "global_viewer");
        const int64_t now = 1'000'000;
        store->create_session(make_session("keep", user_id, now + hour_ms));
        store->create_session(make_session("drop", user_id, now + hour_ms));
        const auto new_hash = hash_for("new-password");

        WHEN("the password is replaced while keeping one session")
        {
            const auto revoked = store->update_user_password_and_revoke_sessions(user_id, new_hash, "keep");

            THEN("the hash is stored and only the other session is revoked")
            {
                REQUIRE(revoked == 1);
                REQUIRE(store->get_user_by_username("gina")->password_hash == new_hash);
                REQUIRE(store->get_session("keep", now));
                REQUIRE_FALSE(store->get_session("drop", now));
            }
        }
    }
}

SCENARIO("Disabling a user and revoking their sessions is one operation")
{
    GIVEN("a user with two sessions and the built-in admin with one")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto user_id = store->create_user("hank", hash_for("pw"), "global_viewer");
        const auto admin_id = store->get_user_by_username("thewatcher")->user_id;
        const int64_t now = 1'000'000;
        store->create_session(make_session("h1", user_id, now + hour_ms));
        store->create_session(make_session("h2", user_id, now + hour_ms));
        store->create_session(make_session("a1", admin_id, now + hour_ms));

        WHEN("the user is disabled with revocation")
        {
            const auto affected = store->disable_user_and_revoke_sessions(user_id);

            THEN("the user is disabled and every session row of theirs is gone")
            {
                REQUIRE(affected == 1);
                REQUIRE(store->get_user_by_username("hank")->disabled);
                // Re-enabling must not revive anything.
                store->enable_user(user_id);
                REQUIRE_FALSE(store->get_session("h1", now));
                REQUIRE_FALSE(store->get_session("h2", now));
            }
        }

        WHEN("the built-in admin is disabled with revocation")
        {
            const auto affected = store->disable_user_and_revoke_sessions(admin_id);

            THEN("nothing changes, so the admin keeps their session")
            {
                REQUIRE(affected == 0);
                REQUIRE(store->get_session("a1", now));
            }
        }

        WHEN("an unknown user is disabled with revocation")
        {
            THEN("zero rows are affected")
            {
                REQUIRE(store->disable_user_and_revoke_sessions(987654) == 0);
            }
        }
    }
}

SCENARIO("Enabling an unknown user is reported as not found")
{
    GIVEN("an admin session on an API server")
    {
        ApiFixture fx;
        const auto admin = fx.login("thewatcher", "look_at_me");
        REQUIRE_FALSE(admin.empty());

        WHEN("an unknown user id is enabled")
        {
            const auto status = fx.put(admin, "/api/users/987654/enable");

            THEN("the API answers 404")
            {
                REQUIRE(status == 404);
            }
        }
    }
}

SCENARIO("Enabling a user reports how many rows changed")
{
    GIVEN("a store with one disabled user")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto user_id = store->create_user("ivy", hash_for("pw"), "global_viewer");
        store->disable_user(user_id);

        WHEN("an unknown id is enabled")
        {
            THEN("no row changes")
            {
                REQUIRE(store->enable_user(987654) == 0);
            }
        }

        WHEN("the disabled user is enabled")
        {
            const auto changed = store->enable_user(user_id);

            THEN("one row changes and the user is enabled")
            {
                REQUIRE(changed == 1);
                REQUIRE_FALSE(store->get_user_by_username("ivy")->disabled);
            }
        }
    }
}

SCENARIO("Session creation racing a password reset never leaves an old-credential session behind")
{
    GIVEN("a file-backed store shared by many threads and a user whose password is about to change")
    {
        thewatcher::test::ScopedTempDir dir{"auth-race"};
        auto store = thewatcher::server::make_store("sqlite", (dir / "race.db").string());
        const auto old_hash = hash_for("old-password");
        const auto user_id = store->create_user("jack", old_hash, "global_viewer");
        const auto new_hash = hash_for("new-password");
        const int64_t expires = now_ms() + hour_ms;

        constexpr int creators = 6;
        constexpr int creator_budget = 400; // sessions each creator may attempt
        // Wall-clock cap as well, so a slow disk bounds the loops by time.
        const auto creator_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::atomic<int> created_count{0};
        std::atomic<int> errors{0};
        std::atomic<bool> stop{false};
        std::vector<std::vector<std::string>> created(creators);

        WHEN("logins keep creating sessions while the password is reset")
        {
            std::vector<std::thread> threads;
            for (int t = 0; t < creators; ++t)
            {
                threads.emplace_back([&, t] {
                    // No Catch2 assertions off the main thread.
                    // Bounded by a fixed budget as well as by `stop`, so a slow disk
                    // cannot make the loop run for minutes.
                    for (int i = 0;
                         i < creator_budget && !stop.load() && std::chrono::steady_clock::now() < creator_deadline; ++i)
                    {
                        const auto token = "tok-" + std::to_string(t) + "-" + std::to_string(i);
                        try
                        {
                            if (store->create_session_if_credentials_current(make_session(token, user_id, expires),
                                                                             old_hash))
                            {
                                created[t].push_back(token);
                                ++created_count;
                            }
                        }
                        catch (const std::exception&)
                        {
                            ++errors;
                        }
                    }
                });
            }
            // Reset once logins are demonstrably in flight.
            thewatcher::test::eventually(
                [&] {
                    return created_count.load() >= 20;
                },
                10s, 5ms);
            // Several resets at once: each is its own transaction, so they must
            // queue behind one another rather than nest on the shared connection.
            std::atomic<int> revoke_errors{0};
            std::vector<std::thread> updaters;
            for (int u = 0; u < 3; ++u)
            {
                updaters.emplace_back([&] {
                    for (int i = 0; i < 20; ++i)
                    {
                        try
                        {
                            store->update_user_password_and_revoke_sessions(user_id, new_hash);
                        }
                        catch (const std::exception&)
                        {
                            ++revoke_errors;
                        }
                    }
                });
            }
            for (auto& th : updaters)
                th.join();
            // Let a few more attempts hit the new hash before stopping.
            std::this_thread::sleep_for(100ms);
            stop = true;
            for (auto& th : threads)
                th.join();

            THEN("no call failed and no session made with the old credentials is still valid")
            {
                REQUIRE(errors.load() == 0);
                REQUIRE(revoke_errors.load() == 0);
                REQUIRE(created_count.load() >= 20);
                const auto now = now_ms();
                for (const auto& tokens : created)
                    for (const auto& token : tokens)
                        REQUIRE_FALSE(store->get_session(token, now));
            }
        }
    }
}

namespace
{

// Owns a second SQLite connection to the same file, used only to plant a
// fault trigger that the store under test does not know about.
struct SideConnection
{
    // The RAII connection owns the handle from the moment it opens (it also
    // installs the busy timeout), so a failed open throws and nothing leaks.
    explicit SideConnection(const std::string& path)
        : conn(path)
    {
    }
    void exec(const std::string& sql)
    {
        char* err = nullptr;
        const auto rc = sqlite3_exec(conn, sql.c_str(), nullptr, nullptr, &err);
        const std::string message = err != nullptr ? err : "";
        sqlite3_free(err);
        REQUIRE(message.empty());
        REQUIRE(rc == SQLITE_OK);
    }
    // Runs a query that returns one integer, for example a row count, without
    // going through the store under test.
    int64_t scalar(const std::string& sql)
    {
        thewatcher::server::sqlite::Stmt st;
        REQUIRE(sqlite3_prepare_v2(conn, sql.c_str(), -1, &st.s, nullptr) == SQLITE_OK);
        REQUIRE(sqlite3_step(st.s) == SQLITE_ROW);
        return sqlite3_column_int64(st.s, 0);
    }
    thewatcher::server::sqlite::Connection conn;
};

// A thread that is always joined on scope exit, so a failed REQUIRE never
// destroys a joinable std::thread (std::terminate). Declare it before anything
// the thread waits on, so that is released first and the join cannot hang.
class ScopedThread
{
public:
    ScopedThread() = default;
    ScopedThread(const ScopedThread&) = delete;
    ScopedThread& operator=(const ScopedThread&) = delete;
    ~ScopedThread()
    {
        join();
    }
    template <typename Fn>
    void start(Fn fn)
    {
        thread_ = std::thread(std::move(fn));
    }
    void join()
    {
        if (thread_.joinable())
            thread_.join();
    }

private:
    std::thread thread_;
};

} // namespace

SCENARIO("make_store wraps every backend in SynchronizedStore")
{
    GIVEN("a store opened through make_store")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");

        THEN("it is a SynchronizedStore, so its calls run one at a time")
        {
            REQUIRE(dynamic_cast<thewatcher::server::SynchronizedStore*>(store.get()) != nullptr);
        }
    }
}

// The invariant here is serialisation. make_store wraps every backend in
// SynchronizedStore (checked above), which runs one call at a time on the one
// connection, so a login that arrives during a reset cannot run inside the
// reset's transaction: it waits until the reset has rolled back and returned.
// Without the wrapper the login's insert would land inside the reset's open
// transaction and the rollback would discard it. The store's lock observer
// makes both halves of the overlap facts the test waits for, rather than
// inferring them from timing: the reset holds the lock (and is stuck behind a
// SQLite writer), and then the login, sent over HTTP, waits for that lock.
SCENARIO("A login during a failing password reset waits for it and keeps its session")
{
    GIVEN("an API server over a synchronized file-backed store where revoking one user's sessions is forced to fail")
    {
        using thewatcher::server::SynchronizedStore;
        using LockEvent = SynchronizedStore::LockEvent;
        constexpr std::string_view reset_method = "update_user_password_and_revoke_sessions";

        thewatcher::test::ScopedTempDir dir{"auth-rollback"};
        const auto path = (dir / "rollback.db").string();
        // Read by the lock observer, so declared before the store that owns it.
        std::atomic<bool> observing{false}; // setup calls before the barrier do not count
        std::atomic<bool> reset_returned{false};
        std::atomic<bool> reset_holds_lock{false};
        std::atomic<bool> reset_waited{false};
        std::atomic<bool> login_waited_for_reset{false};
        auto synchronized =
            std::make_unique<SynchronizedStore>(std::make_unique<thewatcher::server::SqliteStore>(path));
        synchronized->set_lock_observer([&](const char* method, LockEvent event) {
            if (!observing.load())
                return;
            const bool is_reset = method == reset_method;
            if (is_reset && event == LockEvent::Acquired)
                reset_holds_lock = true;
            else if (is_reset && event == LockEvent::Waiting)
                reset_waited = true; // something else held the lock first
            else if (!is_reset && event == LockEvent::Waiting && reset_holds_lock.load() && !reset_returned.load())
                // The reset and the HTTP login are the only callers while the
                // test observes, so this is the login waiting for the reset.
                login_waited_for_reset = true;
        });
        ApiFixture fx(std::move(synchronized));
        auto& store = *fx.store;

        const auto victim_hash = hash_for("victim-password");
        const auto victim = store.create_user("victim", victim_hash, "global_viewer");
        store.create_user("other", hash_for("other-password"), "global_viewer");
        store.create_session(make_session("victim-session", victim, now_ms() + hour_ms));
        {
            // The reset updates the password first, then fails on the revoke
            // and must roll the whole transaction back.
            SideConnection side(path);
            side.exec("CREATE TRIGGER force_revoke_failure BEFORE DELETE ON sessions WHEN OLD.user_id = " +
                      std::to_string(victim) + " BEGIN SELECT RAISE(ABORT, 'forced'); END;");
        }
        const auto new_hash = hash_for("new-victim-password");

        WHEN("another user signs in while the failing reset holds the store")
        {
            std::atomic<bool> reset_threw{false};
            std::atomic<bool> login_returned{false};
            std::atomic<int> login_status{0};
            // Written by the threads before they return; read only after join().
            std::string reset_error;
            std::string login_cookie;

            // Declared before the writer, so on an early failure the writer is
            // closed (releasing the write lock) before the threads are joined.
            ScopedThread reset;
            ScopedThread login;
            // Holds SQLite's write lock. The reset's BEGIN IMMEDIATE waits for it
            // (busy timeout 5 s) inside the store call, holding the store's
            // lock, until the test releases it. No Catch2 assertions run off the
            // main thread.
            SideConnection writer(path);
            writer.exec("BEGIN IMMEDIATE;");
            observing = true;

            reset.start([&] {
                try
                {
                    store.update_user_password_and_revoke_sessions(victim, new_hash);
                }
                catch (const std::exception& e)
                {
                    reset_error = e.what();
                    reset_threw = true;
                }
                reset_returned = true;
            });
            const bool reset_held = thewatcher::test::eventually(
                [&] {
                    return reset_holds_lock.load();
                },
                5s, 1ms);

            // Sent only once the reset holds the lock, so the login cannot get
            // there first.
            login.start([&] {
                auto res = fx.client.Post("/api/login",
                                          json{
                                              {"username", "other"         },
                                              {"password", "other-password"}
                }
                                              .dump(),
                                          "application/json");
                if (res)
                {
                    login_status = res->status;
                    const auto set_cookie = res->get_header_value("Set-Cookie");
                    login_cookie = set_cookie.substr(0, set_cookie.find(';'));
                }
                login_returned = true;
            });
            // The barrier: the login's first store call found the lock held by
            // the reset, which is still inside its call.
            const bool login_waited = thewatcher::test::eventually(
                [&] {
                    return login_waited_for_reset.load();
                },
                5s, 1ms);
            const bool login_still_waiting = !login_returned.load();

            writer.exec("ROLLBACK;");
            const bool both_returned = thewatcher::test::eventually(
                [&] {
                    return reset_returned.load() && login_returned.load();
                },
                10s, 1ms);
            reset.join();
            login.join();
            observing = false;

            THEN("the login waits for the reset to roll back, then gets a session that survives it")
            {
                // Each barrier fails the test on timeout instead of letting it
                // pass without the overlap it is meant to prove.
                REQUIRE(reset_held);
                REQUIRE_FALSE(reset_waited.load());
                REQUIRE(login_waited);
                REQUIRE(login_still_waiting);
                REQUIRE(both_returned);
                REQUIRE(reset_threw.load());
                // The reset failed because of the planted trigger, not for some
                // other reason such as a busy timeout on the writer's lock.
                REQUIRE(reset_error.find("forced") != std::string::npos);
                REQUIRE(login_status.load() == 200);
                REQUIRE_FALSE(login_cookie.empty());
                REQUIRE(fx.session_status(login_cookie) == 200);
                REQUIRE(store.get_user_by_username("victim")->password_hash == victim_hash);
                REQUIRE(store.get_session("victim-session", now_ms()));
            }
        }
    }
}

SCENARIO("Session purges are spaced five minutes from the end of the previous purge")
{
    GIVEN("an API server over a file-backed store whose purge clock the test controls")
    {
        thewatcher::test::ScopedTempDir dir{"auth-purge-spacing"};
        const auto path = (dir / "purge.db").string();
        auto store = thewatcher::server::make_store("sqlite", path);
        zmq::context_t ctx{1};
        thewatcher::server::ZapHandler zap{ctx};
        // Never started: the test calls the purge directly.
        thewatcher::server::ApiServer api(*store, zap, "127.0.0.1", free_http_port());
        std::atomic<int64_t> purge_clock_ms{0};
        api.set_session_purge_clock([&purge_clock_ms] {
            return purge_clock_ms.load();
        });
        const auto admin_id = store->get_user_by_username("thewatcher")->user_id;
        // Reads the row count on a connection of its own, so whether a purge
        // ran is seen without asking the store under test.
        SideConnection observer(path);
        const auto plant_expired_session = [&](const std::string& token) {
            store->create_session(make_session(token, admin_id, 1));
        };
        const auto session_rows = [&] {
            return observer.scalar("SELECT COUNT(*) FROM sessions;");
        };

        WHEN("a purge is held up by another writer and finishes eight seconds after it started")
        {
            plant_expired_session("expired-1");
            std::atomic<bool> purge_returned{false};
            // Declared before the writer, so on an early failure the writer is
            // closed (releasing the write lock) before the thread is joined.
            ScopedThread purge;
            SideConnection writer(path);
            writer.exec("BEGIN IMMEDIATE;");
            purge.start([&] {
                api.purge_expired_sessions_if_due(now_ms());
                purge_returned = true;
            });
            // An unblocked purge returns within milliseconds, so one still
            // running after this pause is waiting for the writer's lock.
            std::this_thread::sleep_for(200ms);
            const bool purge_held = !purge_returned.load();
            purge_clock_ms = 8'000;
            writer.exec("ROLLBACK;");
            purge.join();
            const auto after_slow_purge = session_rows();

            plant_expired_session("expired-2");
            purge_clock_ms = 300'000; // five minutes after the start, not after the end
            api.purge_expired_sessions_if_due(now_ms());
            const auto five_minutes_after_start = session_rows();
            purge_clock_ms = 308'000; // five minutes after the end
            api.purge_expired_sessions_if_due(now_ms());
            const auto five_minutes_after_end = session_rows();

            THEN("the next purge runs only once five minutes have passed since the slow one finished")
            {
                REQUIRE(purge_held);
                REQUIRE(after_slow_purge == 0);
                REQUIRE(five_minutes_after_start == 1);
                REQUIRE(five_minutes_after_end == 0);
            }
        }

        WHEN("the wall clock jumps a day ahead and then an hour behind between purges")
        {
            plant_expired_session("expired-1");
            api.purge_expired_sessions_if_due(now_ms());
            const auto after_first = session_rows();

            plant_expired_session("expired-2");
            purge_clock_ms = 1'000;
            api.purge_expired_sessions_if_due(now_ms() + 24 * hour_ms);
            const auto after_forward_jump = session_rows();
            purge_clock_ms = 301'000;
            api.purge_expired_sessions_if_due(now_ms() - hour_ms);
            const auto after_backward_jump = session_rows();

            THEN("the spacing follows the purge clock, not the wall clock")
            {
                REQUIRE(after_first == 0);
                REQUIRE(after_forward_jump == 1);  // one second on the purge clock: too soon
                REQUIRE(after_backward_jump == 0); // five minutes on the purge clock: due
            }
        }
    }
}

SCENARIO("A password change tells a missing user apart from a user with no sessions")
{
    GIVEN("a store with a user who has no sessions")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto user_id = store->create_user("kate", hash_for("pw"), "global_viewer");

        WHEN("the password of an existing user is replaced")
        {
            const auto result = store->update_user_password_and_revoke_sessions(user_id, hash_for("new-pw"));

            THEN("the change is reported with zero sessions revoked")
            {
                REQUIRE(result.has_value());
                REQUIRE(*result == 0);
            }
        }

        WHEN("the password of a user that does not exist is replaced")
        {
            const auto result = store->update_user_password_and_revoke_sessions(987654, hash_for("new-pw"));

            THEN("the store reports that no user was updated")
            {
                REQUIRE_FALSE(result.has_value());
            }
        }
    }
}

SCENARIO("A username containing a NUL byte is rejected before it reaches the throttle or the store")
{
    GIVEN("an API server whose admin account is thewatcher")
    {
        ApiFixture fx;
        const auto admin = fx.login("thewatcher", "look_at_me");
        REQUIRE_FALSE(admin.empty());

        WHEN("logins name the admin with a NUL byte and a different suffix each time, with the right password")
        {
            std::vector<int> statuses;
            for (int i = 0; i < 12; ++i)
            {
                const auto name = std::string("thewatcher\0", 11) + std::to_string(i);
                auto res = fx.client.Post("/api/login",
                                          json{
                                              {"username", name        },
                                              {"password", "look_at_me"}
                }
                                              .dump(),
                                          "application/json");
                REQUIRE(res);
                statuses.push_back(res->status);
                REQUIRE(res->get_header_value("Set-Cookie").empty());
            }

            THEN("every one is refused as a bad request, never reaching the account or the throttle")
            {
                for (const auto status : statuses)
                    REQUIRE(status == 400);
                REQUIRE_FALSE(fx.login("thewatcher", "look_at_me").empty());
            }
        }

        WHEN("a user is created with a NUL byte in the name")
        {
            auto res = fx.client.Post(
                "/api/users",
                {
                    {"Cookie", admin}
            },
                json{{"username", std::string("evil\0x", 6)}, {"password", "pw-123456"}, {"role", "global_viewer"}}
                    .dump(),
                "application/json");
            REQUIRE(res);

            THEN("it is rejected and no account exists")
            {
                REQUIRE(res->status == 400);
                REQUIRE_FALSE(fx.store->get_user_by_username("evil").has_value());
            }
        }
    }
}

SCENARIO("A self-service change applies only to the password hash that was verified")
{
    GIVEN("a store with a user who has two sessions")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto old_hash = hash_for("old-password");
        const auto user_id = store->create_user("gina", old_hash, "global_viewer");
        const int64_t expires = now_ms() + hour_ms;
        store->create_session(make_session("gina-1", user_id, expires));
        store->create_session(make_session("gina-2", user_id, expires));
        using Status = thewatcher::server::PasswordChangeResult::Status;

        WHEN("the change names the hash that is still stored")
        {
            const auto result =
                store->update_user_password_if_current_and_revoke_sessions(user_id, old_hash, "new-hash", "gina-1");

            THEN("the password changes and the other session is revoked")
            {
                REQUIRE(result.status == Status::Changed);
                REQUIRE(result.revoked == 1);
                REQUIRE(store->get_user_by_username("gina")->password_hash == "new-hash");
                REQUIRE(store->get_session("gina-1", now_ms()));
                REQUIRE_FALSE(store->get_session("gina-2", now_ms()));
            }
        }

        WHEN("the stored hash was replaced after it was verified")
        {
            store->update_user_password(user_id, "reset-by-admin");
            const auto result =
                store->update_user_password_if_current_and_revoke_sessions(user_id, old_hash, "new-hash", "gina-1");

            THEN("nothing changes: the reset stays and no session is revoked")
            {
                REQUIRE(result.status == Status::PasswordChanged);
                REQUIRE(result.revoked == 0);
                REQUIRE(store->get_user_by_username("gina")->password_hash == "reset-by-admin");
                REQUIRE(store->get_session("gina-1", now_ms()));
                REQUIRE(store->get_session("gina-2", now_ms()));
            }
        }

        WHEN("the user was disabled, and their sessions revoked, after the password was verified")
        {
            REQUIRE(store->disable_user_and_revoke_sessions(user_id) == 1);
            const auto result =
                store->update_user_password_if_current_and_revoke_sessions(user_id, old_hash, "new-hash", "gina-1");

            THEN("nothing changes and the store reports that the session ended")
            {
                REQUIRE(result.status == Status::SessionEnded);
                REQUIRE(result.revoked == 0);
                REQUIRE(store->get_user_by_username("gina")->password_hash == old_hash);
            }
        }

        WHEN("the user was disabled and enabled again after the password was verified")
        {
            REQUIRE(store->disable_user_and_revoke_sessions(user_id) == 1);
            REQUIRE(store->enable_user_and_revoke_sessions(user_id).has_value());
            const auto result =
                store->update_user_password_if_current_and_revoke_sessions(user_id, old_hash, "new-hash", "gina-1");

            THEN("nothing changes: the session that verified the password is gone")
            {
                REQUIRE(result.status == Status::SessionEnded);
                REQUIRE(result.revoked == 0);
                REQUIRE(store->get_user_by_username("gina")->password_hash == old_hash);
            }
        }

        WHEN("the user was disabled without their sessions being revoked")
        {
            REQUIRE(store->disable_user(user_id) == 1);
            const auto result =
                store->update_user_password_if_current_and_revoke_sessions(user_id, old_hash, "new-hash", "gina-1");

            THEN("nothing changes or is revoked while the user is disabled")
            {
                REQUIRE(result.status == Status::SessionEnded);
                REQUIRE(result.revoked == 0);
                REQUIRE(store->get_user_by_username("gina")->password_hash == old_hash);
                // Re-enabled without a revoke, both sessions are still there.
                REQUIRE(store->enable_user(user_id) == 1);
                REQUIRE(store->get_session("gina-1", now_ms()));
                REQUIRE(store->get_session("gina-2", now_ms()));
            }
        }

        WHEN("the caller's session ended after the password was verified")
        {
            store->delete_session("gina-1");
            const auto result =
                store->update_user_password_if_current_and_revoke_sessions(user_id, old_hash, "new-hash", "gina-1");

            THEN("nothing changes and the user's other session is kept")
            {
                REQUIRE(result.status == Status::SessionEnded);
                REQUIRE(result.revoked == 0);
                REQUIRE(store->get_user_by_username("gina")->password_hash == old_hash);
                REQUIRE(store->get_session("gina-2", now_ms()));
            }
        }

        WHEN("the user does not exist")
        {
            const auto result =
                store->update_user_password_if_current_and_revoke_sessions(987654, old_hash, "x", "gina-1");

            THEN("the store reports it")
            {
                REQUIRE(result.status == Status::UserNotFound);
            }
        }
    }
}

SCENARIO("An admin reset that lands between verification and update wins over a self-service change")
{
    GIVEN("a user who sends the correct current password while an admin reset lands mid-request")
    {
        CapturedLog log;
        std::atomic<bool> armed{false};
        std::string reset_hash;
        int64_t carol_id = 0;
        ApiFixture fx([&](ApiFixture& f) {
            // The same store call the admin reset route makes: new hash, and
            // every session of the user revoked, in one transaction.
            f.api.set_password_change_test_hook([&armed, &reset_hash, &carol_id, fixture = &f](int64_t user_id) {
                if (armed.load() && user_id == carol_id)
                    fixture->store->update_user_password_and_revoke_sessions(user_id, reset_hash, "");
            });
        });
        const auto admin = fx.login("thewatcher", "look_at_me");
        carol_id = fx.create_user(admin, "carol", "carol-pass-1");
        const auto carol_a = fx.login("carol", "carol-pass-1");
        const auto carol_b = fx.login("carol", "carol-pass-1");
        reset_hash = hash_for("admin-chosen");
        armed = true;

        WHEN("the change is submitted")
        {
            auto res =
                fx.client.Put("/api/users/" + std::to_string(carol_id) + "/password",
                              {
                                  {"Cookie", carol_a}
            },
                              R"({"password":"carol-pass-new","current_password":"carol-pass-1"})", "application/json");
            REQUIRE(res);

            THEN("it is refused with 409, the admin hash stays and the reset's revocation stands")
            {
                // 409, not 401: the hash changed, which says more than the
                // ended session (the reset revoked it too).
                REQUIRE(res->status == 409);
                REQUIRE(fx.store->get_user_by_username("carol")->password_hash == reset_hash);
                REQUIRE(fx.session_status(carol_a) == 401);
                REQUIRE(fx.session_status(carol_b) == 401);
            }

            THEN("no success is audited")
            {
                log.settle();
                REQUIRE(log.read().find("event=password_change outcome=success") == std::string::npos);
            }
        }
    }
}

SCENARIO("A disable that lands between verification and update stops a self-service change")
{
    GIVEN("a user who sends the correct current password while an admin acts on the account mid-request")
    {
        CapturedLog log;
        // What the hook does in the window: 0 nothing, 1 disable, 2 disable and
        // enable again. Each is the store call the matching admin route makes.
        std::atomic<int> interference{0};
        int64_t hank_id = 0;
        ApiFixture fx([&](ApiFixture& f) {
            f.api.set_password_change_test_hook([&interference, &hank_id, fixture = &f](int64_t user_id) {
                if (user_id != hank_id)
                    return;
                const auto mode = interference.load();
                if (mode >= 1)
                    fixture->store->disable_user_and_revoke_sessions(user_id);
                if (mode == 2)
                    fixture->store->enable_user_and_revoke_sessions(user_id);
            });
        });
        const auto admin = fx.login("thewatcher", "look_at_me");
        hank_id = fx.create_user(admin, "hank", "hank-pass-1");
        const auto hank = fx.login("hank", "hank-pass-1");
        REQUIRE_FALSE(hank.empty());
        const auto old_hash = fx.store->get_user_by_username("hank")->password_hash;
        const auto change_password = [&] {
            auto res =
                fx.client.Put("/api/users/" + std::to_string(hank_id) + "/password",
                              {
                                  {"Cookie", hank}
            },
                              R"({"password":"hank-pass-new","current_password":"hank-pass-1"})", "application/json");
            REQUIRE(res);
            return res->status;
        };

        WHEN("the account is disabled in that window")
        {
            interference = 1;
            const auto status = change_password();

            THEN("the change is refused with 401 and the stored hash is unchanged")
            {
                REQUIRE(status == 401);
                REQUIRE(fx.store->get_user_by_username("hank")->password_hash == old_hash);
            }

            THEN("the refusal is audited and no success is")
            {
                REQUIRE(log.eventually_contains("event=password_change outcome=session_ended"));
                log.settle();
                REQUIRE(log.read().find("event=password_change outcome=success") == std::string::npos);
            }
        }

        WHEN("the account is disabled and enabled again in that window")
        {
            interference = 2;
            const auto status = change_password();

            THEN("the change is refused with 401: the old password still signs in and the new one does not")
            {
                REQUIRE(status == 401);
                REQUIRE(fx.store->get_user_by_username("hank")->password_hash == old_hash);
                REQUIRE(fx.session_status(hank) == 401);
                REQUIRE_FALSE(fx.login("hank", "hank-pass-1").empty());
                REQUIRE(fx.login("hank", "hank-pass-new").empty());
            }
        }

        WHEN("nothing happens to the account in that window")
        {
            const auto status = change_password();

            THEN("the change succeeds and the session that made it is kept")
            {
                REQUIRE(status == 200);
                REQUIRE(fx.session_status(hank) == 200);
                REQUIRE_FALSE(fx.login("hank", "hank-pass-new").empty());
            }
        }
    }
}

SCENARIO("Enabling a user revokes leftover sessions only when the user was disabled")
{
    GIVEN("a store with a disabled user holding a leftover session and an enabled user holding a session")
    {
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        const auto disabled = store->create_user("dora", hash_for("pw"), "global_viewer");
        const auto enabled = store->create_user("eli", hash_for("pw"), "global_viewer");
        const int64_t expires = now_ms() + hour_ms;
        store->disable_user(disabled);
        store->create_session(make_session("dora-old", disabled, expires));
        store->create_session(make_session("eli-1", enabled, expires));

        WHEN("the disabled user is enabled")
        {
            const auto revoked = store->enable_user_and_revoke_sessions(disabled);

            THEN("the user is enabled and the leftover session is gone")
            {
                REQUIRE(revoked.has_value());
                REQUIRE(*revoked == 1);
                REQUIRE_FALSE(store->get_user_by_username("dora")->disabled);
                REQUIRE_FALSE(store->get_session("dora-old", now_ms()));
            }

            AND_WHEN("a session is created and the enable is retried")
            {
                store->create_session(make_session("dora-new", disabled, expires));
                const auto retry = store->enable_user_and_revoke_sessions(disabled);

                THEN("the retry succeeds and the new session survives")
                {
                    REQUIRE(retry.has_value());
                    REQUIRE(*retry == 0);
                    REQUIRE(store->get_session("dora-new", now_ms()));
                }
            }
        }

        WHEN("an already enabled user is enabled")
        {
            const auto revoked = store->enable_user_and_revoke_sessions(enabled);

            THEN("it succeeds without revoking anything")
            {
                REQUIRE(revoked.has_value());
                REQUIRE(*revoked == 0);
                REQUIRE(store->get_session("eli-1", now_ms()));
            }
        }

        WHEN("an unknown user is enabled")
        {
            THEN("the store reports no such user")
            {
                REQUIRE_FALSE(store->enable_user_and_revoke_sessions(987654).has_value());
            }
        }
    }
}

SCENARIO("A retried enable over HTTP does not sign the user out again")
{
    GIVEN("a disabled user the admin enables, who then signs in")
    {
        ApiFixture fx;
        const auto admin = fx.login("thewatcher", "look_at_me");
        const auto user_id = fx.create_user(admin, "carol", "carol-pass-1");
        const auto path = "/api/users/" + std::to_string(user_id);
        REQUIRE(fx.put(admin, path + "/disable") == 200);
        REQUIRE(fx.put(admin, path + "/enable") == 200);
        const auto carol = fx.login("carol", "carol-pass-1");
        REQUIRE_FALSE(carol.empty());

        WHEN("the admin repeats the enable")
        {
            const auto status = fx.put(admin, path + "/enable");

            THEN("it still succeeds and the session created after the first enable survives")
            {
                REQUIRE(status == 200);
                REQUIRE(fx.session_status(carol) == 200);
            }
        }
    }
}

SCENARIO("A wrong current password is audited once, as a failed password change")
{
    GIVEN("a user and a captured audit log")
    {
        CapturedLog log;
        ApiFixture fx;
        const auto admin = fx.login("thewatcher", "look_at_me");
        const auto carol_id = fx.create_user(admin, "carol", "carol-pass-1");
        const auto carol = fx.login("carol", "carol-pass-1");

        WHEN("the user sends a wrong current password")
        {
            REQUIRE(fx.put(carol, "/api/users/" + std::to_string(carol_id) + "/password",
                           R"({"password":"carol-pass-new","current_password":"wrong"})") == 403);
            log.settle();

            THEN("only the password_change failure line is written, not a permission denial")
            {
                const auto text = log.read();
                REQUIRE(text.find("event=password_change outcome=failure") != std::string::npos);
                REQUIRE(text.find("event=permission_denied") == std::string::npos);
            }
        }

        WHEN("the user tries to change the admin password")
        {
            REQUIRE(fx.put(carol, "/api/users/1/password", R"({"password":"x-new-pass","current_password":"x"})") ==
                    403);
            log.settle();

            THEN("the real authorization denial is still audited")
            {
                REQUIRE(log.read().find("event=permission_denied") != std::string::npos);
            }
        }
    }
}
