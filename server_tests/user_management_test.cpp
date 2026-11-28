// User management tests (issue #64).
//
// - Role and group membership can be edited (PUT /api/users/:id).
// - Group admins manage the users of their own groups and nothing beyond.
// - The last enabled global admin cannot be disabled, deleted or demoted, and
//   the check lives in the store so concurrent requests cannot both pass it.
// - Role and group changes apply to live sessions immediately: a session
//   stores only the user id and the role is read on every request.
#include "../server/api.hpp"
#include "../server/store.hpp"
#include "../server/zap_handler.hpp"
#include "test_support/net_helpers.hpp"
#include "test_support/server_fixture.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>
#include <sqlite3.h>
#include <zmq.hpp>

using namespace thewatcher::server;
using thewatcher::test::free_http_port;
using thewatcher::test::login_as;
using thewatcher::test::login_as_default_admin;
using json = nlohmann::json;

namespace
{

// Clears the built-in flag on the bootstrap admin, so it behaves like any
// other global admin. Databases created before the built-in guard existed
// look like this, and it is the only way to reach "last global admin" through
// the store, because the built-in admin otherwise always counts as one.
void clear_built_in_flag(const std::string& db_path)
{
    { // Create the schema and bootstrap admin, then close the store again.
        auto store = make_store("sqlite", db_path);
    }
    sqlite3* raw = nullptr;
    REQUIRE(sqlite3_open(db_path.c_str(), &raw) == SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
    REQUIRE(sqlite3_exec(db.get(), "UPDATE users SET built_in=0;", nullptr, nullptr, nullptr) == SQLITE_OK);
}

std::optional<UserRecord> find_user(Store& store, const std::string& username)
{
    return store.get_user_by_username(username);
}

// An ApiServer on a store, started on a free port.
struct ApiFixture
{
    explicit ApiFixture(const std::string& db_path = ":memory:")
        : store(make_store("sqlite", db_path))
        , api(*store, zap, "127.0.0.1", port)
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

    // A client signed in as `username`.
    std::unique_ptr<httplib::Client> sign_in(const std::string& username, const std::string& password)
    {
        auto c = std::make_unique<httplib::Client>("http://127.0.0.1:" + std::to_string(port));
        c->set_connection_timeout(5, 0);
        c->set_read_timeout(5, 0);
        REQUIRE(login_as(*c, username, password));
        return c;
    }

    // Declared first so it is initialised before `api` uses it.
    int port = free_http_port();
    zmq::context_t ctx{1};
    std::unique_ptr<Store> store;
    ZapHandler zap{ctx};
    ApiServer api;
    httplib::Client client;
};

int64_t create_group(httplib::Client& admin, const std::string& name)
{
    auto res = admin.Post("/api/groups",
                          json{
                              {"name", name}
    }
                              .dump(),
                          "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    return json::parse(res->body).at("group_id").get<int64_t>();
}

int64_t create_user(httplib::Client& admin, const std::string& name, const std::string& role,
                    const std::vector<int64_t>& groups)
{
    auto res = admin.Post("/api/users",
                          json{
                              {"username",  name        },
                              {"password",  "pw-" + name},
                              {"role",      role        },
                              {"group_ids", groups      }
    }
                              .dump(),
                          "application/json");
    REQUIRE(res);
    REQUIRE(res->status == 200);
    return json::parse(res->body).at("user_id").get<int64_t>();
}

int put_user(httplib::Client& client, int64_t user_id, const json& body)
{
    auto res = client.Put("/api/users/" + std::to_string(user_id), body.dump(), "application/json");
    REQUIRE(res);
    return res->status;
}

int put_action(httplib::Client& client, int64_t user_id, const std::string& action)
{
    auto res = client.Put("/api/users/" + std::to_string(user_id) + "/" + action, "", "application/json");
    REQUIRE(res);
    return res->status;
}

int delete_user(httplib::Client& client, int64_t user_id)
{
    auto res = client.Delete("/api/users/" + std::to_string(user_id));
    REQUIRE(res);
    return res->status;
}

// A JSON array of group ids (a bare braced list would be read as null or an object).
json ids(const std::vector<int64_t>& values)
{
    return json(values);
}

std::vector<int64_t> groups_of(Store& store, int64_t user_id)
{
    return store.get_user_groups(user_id);
}

} // namespace

// ── Store: access edits and the last-admin guard ─────────────────────────────

SCENARIO("A user's role and groups can be edited in one step")
{
    GIVEN("a store with a group viewer in one group")
    {
        auto store = make_store("sqlite", ":memory:");
        const auto group_a = store->create_group("A");
        const auto group_b = store->create_group("B");
        const auto id = store->create_user("alice", "hash", "group_viewer");
        store->set_user_groups(id, {group_a});

        WHEN("role and groups are updated together")
        {
            const auto changed =
                store->update_user_access(id, std::string("group_operator"), std::vector<int64_t>{group_b});

            THEN("both change and one row is reported")
            {
                REQUIRE(changed == 1);
                REQUIRE(find_user(*store, "alice")->role == "group_operator");
                REQUIRE(groups_of(*store, id) == std::vector<int64_t>{group_b});
            }
        }

        WHEN("only the groups are updated")
        {
            REQUIRE(store->update_user_access(id, std::nullopt, std::vector<int64_t>{group_a, group_b}) == 1);

            THEN("the role is untouched")
            {
                REQUIRE(find_user(*store, "alice")->role == "group_viewer");
                REQUIRE(groups_of(*store, id) == std::vector<int64_t>{group_a, group_b});
            }
        }

        WHEN("only the role is updated")
        {
            REQUIRE(store->update_user_access(id, std::string("group_admin"), std::nullopt) == 1);

            THEN("the groups are untouched")
            {
                REQUIRE(find_user(*store, "alice")->role == "group_admin");
                REQUIRE(groups_of(*store, id) == std::vector<int64_t>{group_a});
            }
        }

        WHEN("the groups include one that does not exist")
        {
            THEN("the whole update is rolled back")
            {
                REQUIRE_THROWS(
                    store->update_user_access(id, std::string("group_operator"), std::vector<int64_t>{999999}));
                REQUIRE(find_user(*store, "alice")->role == "group_viewer");
                REQUIRE(groups_of(*store, id) == std::vector<int64_t>{group_a});
            }
        }

        WHEN("an unknown user is updated")
        {
            THEN("no rows change")
            {
                REQUIRE(store->update_user_access(424242, std::string("group_viewer"), std::nullopt) == 0);
            }
        }
    }
}

SCENARIO("The built-in admin's access cannot be edited")
{
    GIVEN("a store with the bootstrapped built-in admin")
    {
        auto store = make_store("sqlite", ":memory:");
        const auto admin = find_user(*store, "thewatcher");
        REQUIRE(admin);
        REQUIRE(admin->built_in);

        WHEN("its role is changed")
        {
            const auto changed = store->update_user_access(admin->user_id, std::string("group_viewer"), std::nullopt);

            THEN("nothing changes")
            {
                REQUIRE(changed == 0);
                REQUIRE(find_user(*store, "thewatcher")->role == "global_admin");
            }
        }
    }
}

SCENARIO("A role change is visible to the user's existing session")
{
    GIVEN("a global operator holding a live session")
    {
        auto store = make_store("sqlite", ":memory:");
        const auto id = store->create_user("bob", "hash", "global_operator");
        SessionRecord session;
        session.token = "bob-token";
        session.user_id = id;
        session.created_at = 1000;
        session.expires_at = 999'999'999'999LL;
        store->create_session(session);

        WHEN("an admin demotes the user")
        {
            REQUIRE(store->update_user_access(id, std::string("group_viewer"), std::nullopt) == 1);

            THEN("the same session now carries the new role, so no revocation is needed")
            {
                const auto after = store->get_session("bob-token", 2000);
                REQUIRE(after);
                REQUIRE(after->role == "group_viewer");
            }
        }
    }
}

SCENARIO("The last enabled global admin cannot be removed or demoted")
{
    GIVEN("a database whose only global admin is an ordinary (non built-in) account")
    {
        thewatcher::test::ScopedTempDir dir("thewatcher-users");
        const auto db_path = (dir.path() / "users.db").string();
        clear_built_in_flag(db_path);
        auto store = make_store("sqlite", db_path);
        const auto sole = find_user(*store, "thewatcher");
        REQUIRE(sole);
        REQUIRE_FALSE(sole->built_in);

        WHEN("it is disabled")
        {
            THEN("the change is refused and the account stays enabled")
            {
                REQUIRE(store->disable_user(sole->user_id) == kLastGlobalAdmin);
                REQUIRE_FALSE(find_user(*store, "thewatcher")->disabled);
            }
        }

        WHEN("it is deleted")
        {
            THEN("the change is refused and the account still exists")
            {
                REQUIRE(store->delete_user(sole->user_id) == kLastGlobalAdmin);
                REQUIRE(find_user(*store, "thewatcher"));
            }
        }

        WHEN("it is demoted to a group role")
        {
            THEN("the change is refused and the role is kept")
            {
                REQUIRE(store->update_user_access(sole->user_id, std::string("group_admin"), std::nullopt) ==
                        kLastGlobalAdmin);
                REQUIRE(find_user(*store, "thewatcher")->role == "global_admin");
            }
        }

        WHEN("only its groups are edited")
        {
            THEN("the edit is allowed because the role is unchanged")
            {
                REQUIRE(store->update_user_access(sole->user_id, std::nullopt, std::vector<int64_t>{}) == 1);
            }
        }

        WHEN("a second global admin exists")
        {
            const auto second = store->create_user("second", "hash", "global_admin");

            THEN("either admin can be removed, but never both")
            {
                REQUIRE(store->delete_user(sole->user_id) == 1);
                REQUIRE(store->disable_user(second) == kLastGlobalAdmin);
                REQUIRE(store->delete_user(second) == kLastGlobalAdmin);
            }
        }

        WHEN("the other global admin is already disabled")
        {
            const auto second = store->create_user("second", "hash", "global_admin");
            REQUIRE(store->disable_user(second) == 1);

            THEN("a disabled admin does not count, so the enabled one is still the last")
            {
                REQUIRE(store->delete_user(sole->user_id) == kLastGlobalAdmin);
            }
        }

        WHEN("the other admin uses the legacy role name")
        {
            store->create_user("legacy", "hash", "admin");

            THEN("it counts as a global admin")
            {
                REQUIRE(store->delete_user(sole->user_id) == 1);
            }
        }

        WHEN("a non-admin account is removed")
        {
            const auto viewer = store->create_user("viewer", "hash", "global_viewer");

            THEN("the guard does not interfere")
            {
                REQUIRE(store->disable_user(viewer) == 1);
                REQUIRE(store->delete_user(viewer) == 1);
            }
        }
    }
}

SCENARIO("Concurrent removal of the last two global admins leaves one")
{
    GIVEN("exactly two enabled global admins")
    {
        thewatcher::test::ScopedTempDir dir("thewatcher-users");
        const auto db_path = (dir.path() / "users.db").string();
        clear_built_in_flag(db_path);
        auto store = make_store("sqlite", db_path);
        const auto first = find_user(*store, "thewatcher")->user_id;
        const auto second = store->create_user("second", "hash", "global_admin");

        WHEN("two threads, each with its own connection, delete one admin each at the same moment")
        {
            // One connection per thread: a SqliteStore connection itself is not
            // shared between threads in this suite. Locked-database errors from
            // the competing writer are retried.
            std::atomic<bool> go{false};
            std::atomic<int> removed{0};
            std::atomic<int> refused{0};
            // Open both connections before the race: opening runs the bootstrap writes.
            auto store_1 = make_store("sqlite", db_path);
            auto store_2 = make_store("sqlite", db_path);
            auto remove = [&](Store& own, int64_t id) {
                while (!go)
                    std::this_thread::yield();
                int result = 0;
                for (int attempt = 0; attempt < 200; ++attempt)
                {
                    try
                    {
                        result = own.delete_user(id);
                        break;
                    }
                    catch (const std::runtime_error&)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                }
                if (result == 1)
                    ++removed;
                else if (result == kLastGlobalAdmin)
                    ++refused;
            };
            std::thread t1(remove, std::ref(*store_1), first);
            std::thread t2(remove, std::ref(*store_2), second);
            go = true;
            t1.join();
            t2.join();

            THEN("exactly one deletion succeeds and one global admin remains")
            {
                REQUIRE(removed == 1);
                REQUIRE(refused == 1);
                const auto users = store->list_users();
                REQUIRE(std::count_if(users.begin(), users.end(), [](const UserRecord& u) {
                            return u.role == "global_admin";
                        }) == 1);
            }
        }
    }
}

// ── API: editing users ───────────────────────────────────────────────────────

SCENARIO("A global admin edits a user's role and groups")
{
    GIVEN("a global admin and a group viewer")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));
        const auto group_a = create_group(fx.client, "A");
        const auto group_b = create_group(fx.client, "B");
        const auto user = create_user(fx.client, "carol", "group_viewer", {group_a});

        WHEN("the admin changes the role and groups")
        {
            const auto status = put_user(fx.client, user,
                                         {
                                             {"role",      "group_operator"},
                                             {"group_ids", ids({group_b})  }
            });

            THEN("the user list shows the new values")
            {
                REQUIRE(status == 200);
                auto list = fx.client.Get("/api/users");
                REQUIRE(list);
                const auto users = json::parse(list->body);
                const auto it = std::find_if(users.begin(), users.end(), [](const json& u) {
                    return u.at("username") == "carol";
                });
                REQUIRE(it != users.end());
                REQUIRE(it->at("role") == "group_operator");
                REQUIRE(it->at("group_ids") == json::array({group_b}));
            }
        }

        WHEN("the admin gives the user a global role")
        {
            THEN("it is accepted")
            {
                REQUIRE(put_user(fx.client, user,
                                 {
                                     {"role", "global_viewer"}
                }) == 200);
                REQUIRE(find_user(*fx.store, "carol")->role == "global_viewer");
            }
        }

        WHEN("the legacy role name is used")
        {
            THEN("it is stored in its normalised form")
            {
                REQUIRE(put_user(fx.client, user,
                                 {
                                     {"role", "operator"}
                }) == 200);
                REQUIRE(find_user(*fx.store, "carol")->role == "global_operator");
            }
        }

        WHEN("the request is malformed")
        {
            THEN("each case is a 400 and nothing changes")
            {
                REQUIRE(put_user(fx.client, user, json::object()) == 400);
                REQUIRE(put_user(fx.client, user,
                                 {
                                     {"role", "emperor"}
                }) == 400);
                REQUIRE(put_user(fx.client, user,
                                 {
                                     {"group_ids", ids({999999})}
                }) == 400);
                REQUIRE(put_user(fx.client, user,
                                 {
                                     {"group_ids", "A"}
                }) == 400);
                REQUIRE(find_user(*fx.store, "carol")->role == "group_viewer");
                REQUIRE(groups_of(*fx.store, user) == std::vector<int64_t>{group_a});
            }
        }

        WHEN("the user does not exist")
        {
            THEN("the request is a 404")
            {
                REQUIRE(put_user(fx.client, 424242,
                                 {
                                     {"role", "group_viewer"}
                }) == 404);
            }
        }

        WHEN("the target is the built-in admin")
        {
            const auto built_in = find_user(*fx.store, "thewatcher")->user_id;

            THEN("the request is a 409 and the role is kept")
            {
                REQUIRE(put_user(fx.client, built_in,
                                 {
                                     {"role", "group_viewer"}
                }) == 409);
                REQUIRE(find_user(*fx.store, "thewatcher")->role == "global_admin");
            }
        }
    }
}

SCENARIO("A role change applies to the user's live session")
{
    GIVEN("a global operator who is signed in")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));
        const auto user = create_user(fx.client, "dave", "global_operator", {});
        auto dave = fx.sign_in("dave", "pw-dave");
        REQUIRE(json::parse(dave->Get("/api/session")->body).at("role") == "global_operator");

        WHEN("the admin demotes the user")
        {
            REQUIRE(put_user(fx.client, user,
                             {
                                 {"role", "group_viewer"}
            }) == 200);

            THEN("the next request on the same session sees the new role")
            {
                REQUIRE(json::parse(dave->Get("/api/session")->body).at("role") == "group_viewer");
            }
        }

        WHEN("the admin disables the user")
        {
            REQUIRE(put_action(fx.client, user, "disable") == 200);

            THEN("the existing session stops working")
            {
                REQUIRE(dave->Get("/api/session")->status == 401);
            }
        }
    }
}

SCENARIO("A group admin manages only the users of their own groups")
{
    GIVEN("two groups, a group admin of A, and users in and outside A")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));
        const auto group_a = create_group(fx.client, "A");
        const auto group_b = create_group(fx.client, "B");
        create_user(fx.client, "boss-a", "group_admin", {group_a});
        const auto in_a = create_user(fx.client, "in-a", "group_viewer", {group_a});
        const auto in_b = create_user(fx.client, "in-b", "group_viewer", {group_b});
        const auto in_both = create_user(fx.client, "in-both", "group_viewer", {group_a, group_b});
        const auto global = create_user(fx.client, "global-op", "global_operator", {group_a});
        auto boss = fx.sign_in("boss-a", "pw-boss-a");

        WHEN("they edit the role of a user who is only in their group")
        {
            THEN("a group-scoped role is accepted")
            {
                REQUIRE(put_user(*boss, in_a,
                                 {
                                     {"role", "group_operator"}
                }) == 200);
                REQUIRE(find_user(*fx.store, "in-a")->role == "group_operator");
            }

            THEN("a global role is refused")
            {
                REQUIRE(put_user(*boss, in_a,
                                 {
                                     {"role", "global_admin"}
                }) == 403);
                REQUIRE(find_user(*fx.store, "in-a")->role == "group_viewer");
            }
        }

        WHEN("they edit a user outside their groups")
        {
            THEN("the request is refused")
            {
                REQUIRE(put_user(*boss, in_b,
                                 {
                                     {"role", "group_operator"}
                }) == 403);
                REQUIRE(put_user(*boss, in_b,
                                 {
                                     {"group_ids", ids({group_a})}
                }) == 403);
                REQUIRE(groups_of(*fx.store, in_b) == std::vector<int64_t>{group_b});
            }
        }

        WHEN("they edit a user holding a global role")
        {
            THEN("the request is refused even though the user is in their group")
            {
                REQUIRE(put_user(*boss, global,
                                 {
                                     {"group_ids", ids({})}
                }) == 403);
                REQUIRE(put_user(*boss, global,
                                 {
                                     {"role", "group_viewer"}
                }) == 403);
                REQUIRE(find_user(*fx.store, "global-op")->role == "global_operator");
            }
        }

        WHEN("they assign a user to a group they do not belong to")
        {
            THEN("the request is refused")
            {
                REQUIRE(put_user(*boss, in_a,
                                 {
                                     {"group_ids", ids({group_a, group_b})}
                }) == 403);
                REQUIRE(groups_of(*fx.store, in_a) == std::vector<int64_t>{group_a});
            }
        }

        WHEN("they remove a user from their group while the user is also in another")
        {
            REQUIRE(put_user(*boss, in_both,
                             {
                                 {"group_ids", ids({})}
            }) == 200);

            THEN("only their own group is touched")
            {
                REQUIRE(groups_of(*fx.store, in_both) == std::vector<int64_t>{group_b});
            }
        }

        WHEN("they change the role of a user who is also in another group")
        {
            THEN("the request is refused because it would reach beyond their groups")
            {
                REQUIRE(put_user(*boss, in_both,
                                 {
                                     {"role", "group_operator"}
                }) == 403);
            }
        }

        WHEN("they disable, enable and delete a user who is only in their group")
        {
            THEN("each succeeds")
            {
                REQUIRE(put_action(*boss, in_a, "disable") == 200);
                REQUIRE(find_user(*fx.store, "in-a")->disabled);
                REQUIRE(put_action(*boss, in_a, "enable") == 200);
                REQUIRE_FALSE(find_user(*fx.store, "in-a")->disabled);
                REQUIRE(delete_user(*boss, in_a) == 200);
                REQUIRE_FALSE(find_user(*fx.store, "in-a"));
            }
        }

        WHEN("they disable or delete users beyond their groups")
        {
            THEN("users outside, shared with another group, and global users are all refused")
            {
                for (const auto target : {in_b, in_both, global})
                {
                    REQUIRE(put_action(*boss, target, "disable") == 403);
                    REQUIRE(put_action(*boss, target, "enable") == 403);
                    REQUIRE(delete_user(*boss, target) == 403);
                }
                REQUIRE_FALSE(find_user(*fx.store, "in-b")->disabled);
                REQUIRE(find_user(*fx.store, "global-op"));
            }
        }

        WHEN("a user who is not an admin tries to edit anyone")
        {
            auto viewer = fx.sign_in("in-both", "pw-in-both");

            THEN("every request is refused")
            {
                REQUIRE(put_user(*viewer, in_a,
                                 {
                                     {"role", "group_operator"}
                }) == 403);
                REQUIRE(put_action(*viewer, in_a, "disable") == 403);
                REQUIRE(delete_user(*viewer, in_a) == 403);
            }
        }
    }
}

SCENARIO("The last global admin is protected through the API")
{
    GIVEN("a server whose only global admin is an ordinary account")
    {
        thewatcher::test::ScopedTempDir dir("thewatcher-users");
        const auto db_path = (dir.path() / "users.db").string();
        clear_built_in_flag(db_path);
        ApiFixture fx(db_path);
        REQUIRE(login_as_default_admin(fx.client));
        const auto self = find_user(*fx.store, "thewatcher")->user_id;

        WHEN("the admin deletes, disables or demotes themselves")
        {
            THEN("each request is a 409 and the account is unchanged")
            {
                REQUIRE(delete_user(fx.client, self) == 409);
                REQUIRE(put_action(fx.client, self, "disable") == 409);
                REQUIRE(put_user(fx.client, self,
                                 {
                                     {"role", "global_viewer"}
                }) == 409);
                const auto after = find_user(*fx.store, "thewatcher");
                REQUIRE(after);
                REQUIRE_FALSE(after->disabled);
                REQUIRE(after->role == "global_admin");
            }
        }

        WHEN("a second global admin exists")
        {
            const auto second = create_user(fx.client, "second", "global_admin", {});

            THEN("the first can leave, and the survivor is then protected")
            {
                REQUIRE(put_user(fx.client, self,
                                 {
                                     {"role", "global_viewer"}
                }) == 200);
                auto survivor = fx.sign_in("second", "pw-second");
                REQUIRE(delete_user(*survivor, second) == 409);
            }
        }
    }
}

// ── Store: a group admin's scope is enforced inside the write ───────────────

SCENARIO("A group admin's scope is re-checked by the store at write time")
{
    GIVEN("groups A and B, a scope limited to A, and users in and outside A")
    {
        auto store = make_store("sqlite", ":memory:");
        const auto group_a = store->create_group("A");
        const auto group_b = store->create_group("B");
        const auto group_c = store->create_group("C");
        const UserScope scope{true, {group_a}};
        const auto inside = store->create_user("inside", "hash", "group_viewer");
        store->set_user_groups(inside, {group_a});
        const auto shared = store->create_user("shared", "hash", "group_viewer");
        store->set_user_groups(shared, {group_a, group_b});
        const auto outside = store->create_user("outside", "hash", "group_viewer");
        store->set_user_groups(outside, {group_b});
        const auto global = store->create_user("global", "hash", "global_operator");
        store->set_user_groups(global, {group_a});
        const auto nobody = store->create_user("nobody", "hash", "group_viewer");

        WHEN("the account actions target a user wholly inside the scope")
        {
            THEN("disable, enable and delete succeed")
            {
                REQUIRE(store->disable_user(inside, scope) == 1);
                REQUIRE(find_user(*store, "inside")->disabled);
                REQUIRE(store->enable_user(inside, scope) == 1);
                REQUIRE_FALSE(find_user(*store, "inside")->disabled);
                REQUIRE(store->delete_user(inside, scope) == 1);
            }
        }

        WHEN("the account actions target users beyond the scope")
        {
            THEN("each is refused as out of scope and nothing changes")
            {
                for (const auto target : {shared, outside, global, nobody})
                {
                    REQUIRE(store->disable_user(target, scope) == kOutOfScope);
                    REQUIRE(store->enable_user(target, scope) == kOutOfScope);
                    REQUIRE(store->delete_user(target, scope) == kOutOfScope);
                }
                REQUIRE_FALSE(find_user(*store, "shared")->disabled);
                REQUIRE(find_user(*store, "global"));
            }
        }

        WHEN("an unknown user is targeted")
        {
            THEN("the result is 0, not out of scope")
            {
                REQUIRE(store->disable_user(424242, scope) == 0);
                REQUIRE(store->update_user_access(424242, std::nullopt, std::vector<int64_t>{}, scope) == 0);
            }
        }

        WHEN("a role is changed")
        {
            THEN("a group role is allowed inside the scope and a global role is not")
            {
                REQUIRE(store->update_user_access(inside, std::string("global_admin"), std::nullopt, scope) ==
                        kOutOfScope);
                REQUIRE(store->update_user_access(inside, std::string("admin"), std::nullopt, scope) == kOutOfScope);
                REQUIRE(store->update_user_access(inside, std::string("group_operator"), std::nullopt, scope) == 1);
                REQUIRE(find_user(*store, "inside")->role == "group_operator");
            }

            THEN("a user who is also in another group, or holds a global role, is out of scope")
            {
                REQUIRE(store->update_user_access(shared, std::string("group_operator"), std::nullopt, scope) ==
                        kOutOfScope);
                REQUIRE(store->update_user_access(global, std::string("group_viewer"), std::nullopt, scope) ==
                        kOutOfScope);
                REQUIRE(find_user(*store, "shared")->role == "group_viewer");
            }
        }

        WHEN("memberships are changed")
        {
            THEN("only the scope's own groups are replaced and the others are kept")
            {
                REQUIRE(store->update_user_access(shared, std::nullopt, std::vector<int64_t>{}, scope) == 1);
                REQUIRE(groups_of(*store, shared) == std::vector<int64_t>{group_b});
            }

            THEN("assigning a group outside the scope, or editing a foreign user, is refused")
            {
                REQUIRE(store->update_user_access(inside, std::nullopt, std::vector<int64_t>{group_a, group_b},
                                                  scope) == kOutOfScope);
                REQUIRE(store->update_user_access(outside, std::nullopt, std::vector<int64_t>{group_a}, scope) ==
                        kOutOfScope);
                REQUIRE(store->update_user_access(global, std::nullopt, std::vector<int64_t>{}, scope) == kOutOfScope);
                REQUIRE(groups_of(*store, inside) == std::vector<int64_t>{group_a});
                REQUIRE(groups_of(*store, outside) == std::vector<int64_t>{group_b});
            }
        }

        WHEN("the target is moved out of the scope after the caller computed it")
        {
            store->set_user_groups(inside, {group_b});

            THEN("every scoped write is refused against the fresh state")
            {
                REQUIRE(store->update_user_access(inside, std::string("group_operator"), std::nullopt, scope) ==
                        kOutOfScope);
                REQUIRE(store->update_user_access(inside, std::nullopt, std::vector<int64_t>{group_a}, scope) ==
                        kOutOfScope);
                REQUIRE(store->disable_user(inside, scope) == kOutOfScope);
                REQUIRE(store->delete_user(inside, scope) == kOutOfScope);
                REQUIRE(find_user(*store, "inside")->role == "group_viewer");
                REQUIRE(groups_of(*store, inside) == std::vector<int64_t>{group_b});
            }
        }

        WHEN("the target gains another group after the caller read it")
        {
            store->set_user_groups(shared, {group_a, group_b, group_c});

            THEN("the merge is computed from the fresh memberships, so the new group survives")
            {
                REQUIRE(store->update_user_access(shared, std::nullopt, std::vector<int64_t>{}, scope) == 1);
                REQUIRE(groups_of(*store, shared) == std::vector<int64_t>{group_b, group_c});
            }
        }
    }
}
