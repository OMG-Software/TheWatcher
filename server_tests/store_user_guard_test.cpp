// Built-in user guard and disabled-session tests (SEC-13 / SEC-14).
//
// Both backends must behave identically here: the bootstrap admin cannot be
// disabled or deleted (Postgres previously lacked the guard and would
// recreate the account with the public default password on the next
// restart), disable/delete report affected rows so the API can 4xx no-ops,
// and a disabled user's existing sessions must stop authenticating.
#include "../server/store_sqlite.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>

using namespace thewatcher::server;

namespace
{

// Returns the user named `username`, or nullopt when no such user exists.
std::optional<UserRecord> find_user(SqliteStore& store, const std::string& username)
{
    auto users = store.list_users();
    auto it = std::find_if(users.begin(), users.end(), [&](const UserRecord& r) {
        return r.username == username;
    });
    if (it == users.end())
        return std::nullopt;
    return *it;
}

} // namespace

SCENARIO("The built-in admin cannot be disabled or deleted")
{
    GIVEN("an in-memory store with the bootstrapped built-in admin")
    {
        SqliteStore store(":memory:");
        const auto admin = find_user(store, "thewatcher");
        REQUIRE(admin.has_value());
        REQUIRE(admin->built_in);

        WHEN("disable_user is called for the built-in admin")
        {
            const auto affected = store.disable_user(admin->user_id);

            THEN("no rows change and the account stays enabled")
            {
                REQUIRE(affected == 0);
                const auto after = find_user(store, "thewatcher");
                REQUIRE(after.has_value());
                REQUIRE_FALSE(after->disabled);
            }
        }

        WHEN("delete_user is called for the built-in admin")
        {
            const auto affected = store.delete_user(admin->user_id);

            THEN("no rows change and the account still exists")
            {
                REQUIRE(affected == 0);
                REQUIRE(find_user(store, "thewatcher").has_value());
            }
        }
    }
}

SCENARIO("Disable and delete report affected rows for ordinary users")
{
    GIVEN("an in-memory store with a regular user")
    {
        SqliteStore store(":memory:");
        const auto id = store.create_user("ordinary", "hash", "viewer");
        REQUIRE(id > 0);

        WHEN("the user is disabled")
        {
            const auto affected = store.disable_user(id);

            THEN("one row is affected and the flag is set")
            {
                REQUIRE(affected == 1);
                const auto user = find_user(store, "ordinary");
                REQUIRE(user.has_value());
                REQUIRE(user->disabled);
            }
        }

        WHEN("the user is deleted")
        {
            const auto affected = store.delete_user(id);

            THEN("one row is affected and the account is gone")
            {
                REQUIRE(affected == 1);
                REQUIRE_FALSE(find_user(store, "ordinary").has_value());
            }
        }

        WHEN("an unknown user id is disabled or deleted")
        {
            THEN("zero rows are affected for both")
            {
                REQUIRE(store.disable_user(999999) == 0);
                REQUIRE(store.delete_user(999999) == 0);
            }
        }
    }
}

SCENARIO("A disabled user's existing sessions stop authenticating")
{
    GIVEN("an in-memory store with a user holding a live session")
    {
        SqliteStore store(":memory:");
        const auto id = store.create_user("session-user", "hash", "viewer");
        REQUIRE(id > 0);

        SessionRecord session;
        session.token = "guard-test-token";
        session.user_id = id;
        session.username = "session-user";
        session.role = "viewer";
        session.created_at = 1000;
        session.expires_at = 999'999'999'999LL;
        store.create_session(session);

        REQUIRE(store.get_session(session.token, 2000).has_value());

        WHEN("the user is disabled")
        {
            REQUIRE(store.disable_user(id) == 1);

            THEN("the session no longer authenticates")
            {
                REQUIRE_FALSE(store.get_session(session.token, 2000).has_value());
            }

            AND_WHEN("the user is re-enabled")
            {
                store.enable_user(id);

                THEN("the session authenticates again")
                {
                    REQUIRE(store.get_session(session.token, 2000).has_value());
                }
            }
        }
    }
}