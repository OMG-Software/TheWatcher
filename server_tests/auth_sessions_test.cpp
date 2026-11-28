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

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
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
                    for (int i = 0; !stop.load(); ++i)
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
    explicit SideConnection(const std::string& path)
    {
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        sqlite3_busy_timeout(db, 5000);
    }
    SideConnection(const SideConnection&) = delete;
    SideConnection& operator=(const SideConnection&) = delete;
    ~SideConnection()
    {
        sqlite3_close(db);
    }
    void exec(const std::string& sql)
    {
        char* err = nullptr;
        const auto rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
        const std::string message = err != nullptr ? err : "";
        sqlite3_free(err);
        REQUIRE(message.empty());
        REQUIRE(rc == SQLITE_OK);
    }
    sqlite3* db = nullptr;
};

} // namespace

SCENARIO("A rolled-back password reset never takes other threads' sessions with it")
{
    GIVEN("a file-backed store where revoking one user's sessions is forced to fail")
    {
        thewatcher::test::ScopedTempDir dir{"auth-rollback"};
        const auto path = (dir / "rollback.db").string();
        auto store = thewatcher::server::make_store("sqlite", path);
        const auto victim_hash = hash_for("victim-password");
        const auto other_hash = hash_for("other-password");
        const auto victim = store->create_user("victim", victim_hash, "global_viewer");
        const auto other = store->create_user("other", other_hash, "global_viewer");
        const int64_t expires = now_ms() + hour_ms;
        store->create_session(make_session("victim-session", victim, expires));
        {
            // The reset updates the password first, then fails on the revoke
            // and must roll the whole transaction back.
            SideConnection side(path);
            side.exec("CREATE TRIGGER force_revoke_failure BEFORE DELETE ON sessions WHEN OLD.user_id = " +
                      std::to_string(victim) + " BEGIN SELECT RAISE(ABORT, 'forced'); END;");
        }
        const auto new_hash = hash_for("new-victim-password");

        constexpr int creators = 4;
        constexpr int resetters = 3;
        constexpr int resets_each = 40;
        std::atomic<int> reset_throws{0};
        std::atomic<int> create_errors{0};
        std::atomic<int> resets_done{0};
        std::atomic<int> acknowledged{0};
        std::vector<std::vector<std::string>> created(creators);

        WHEN("failing resets run while other users keep logging in")
        {
            std::vector<std::thread> threads;
            // Creators start first, and the resets are only released once a
            // login was acknowledged, so the creators cannot be starved.
            for (int c = 0; c < creators; ++c)
            {
                threads.emplace_back([&, c] {
                    for (int i = 0; resets_done.load() < resetters; ++i)
                    {
                        const auto token = "other-" + std::to_string(c) + "-" + std::to_string(i);
                        try
                        {
                            if (store->create_session_if_credentials_current(make_session(token, other, expires),
                                                                             other_hash))
                            {
                                created[c].push_back(token);
                                ++acknowledged;
                            }
                        }
                        catch (const std::exception&)
                        {
                            ++create_errors;
                        }
                    }
                });
            }
            const bool logins_running = thewatcher::test::eventually(
                [&] {
                    return acknowledged.load() >= 1;
                },
                10s, 5ms);
            for (int r = 0; r < resetters; ++r)
            {
                threads.emplace_back([&] {
                    // No Catch2 assertions off the main thread.
                    for (int i = 0; i < resets_each; ++i)
                    {
                        try
                        {
                            store->update_user_password_and_revoke_sessions(victim, new_hash);
                        }
                        catch (const std::exception&)
                        {
                            ++reset_throws;
                        }
                    }
                    ++resets_done;
                });
            }
            for (auto& t : threads)
                t.join();

            THEN("every reset failed, every acknowledged login survives and the password is unchanged")
            {
                REQUIRE(logins_running);
                REQUIRE(reset_throws.load() == resetters * resets_each);
                REQUIRE(create_errors.load() == 0);
                const auto now = now_ms();
                std::size_t total = 0;
                for (const auto& tokens : created)
                {
                    total += tokens.size();
                    for (const auto& token : tokens)
                        REQUIRE(store->get_session(token, now));
                }
                REQUIRE(total > 0);
                REQUIRE(store->get_user_by_username("victim")->password_hash == victim_hash);
                REQUIRE(store->get_session("victim-session", now));
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
