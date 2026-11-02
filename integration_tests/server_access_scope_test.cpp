// Cross-group access-scope tests (SEC-11).
//
// Per-agent endpoints, runbooks and views must all respect group scoping;
// previously GET /api/agents/:id/history skipped the agent-access check,
// GET /api/runbooks returned every runbook to every viewer, and view
// agent_ids were never validated.
#include "test_support/server_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <httplib.h>

using namespace std::chrono_literals;
using namespace thewatcher::test;
using json = nlohmann::json;

namespace
{

struct ScopeFixture : ServerFixture
{
    // Creates a group through the API. Returns its id, or nullopt when the request
    // fails or does not return 200.
    std::optional<int64_t> create_group(const std::string& name)
    {
        auto res = client.Post("/api/groups",
                               json{
                                   {"name", name}
        }
                                   .dump(),
                               "application/json");
        if (!res || res->status != 200)
            return std::nullopt;
        return json::parse(res->body).value("group_id", int64_t{0});
    }

    // Creates a user with the given role and group memberships through the API.
    // Returns true only when the server answers 200.
    bool create_user(const std::string& username, const std::string& role, std::vector<int64_t> group_ids)
    {
        auto res =
            client.Post("/api/users",
                        json{
                            {"username",  username  },
                            {"password",  "sc0pepw!"},
                            {"role",      role      },
                            {"group_ids", group_ids }
        }
                            .dump(),
                        "application/json");
        return res && res->status == 200;
    }

    // Creates groups "<prefix>-group-a" and "<prefix>-group-b", a user in
    // group B, and one approved agent "<prefix>-agent-a/b" in each group.
    // Returns group B's id.
    int64_t seed_two_groups(const std::string& prefix, const std::string& username, const std::string& role)
    {
        const auto group_a = create_group(prefix + "-group-a");
        const auto group_b = create_group(prefix + "-group-b");
        REQUIRE(group_a.has_value());
        REQUIRE(group_b.has_value());
        REQUIRE(create_user(username, role, {*group_b}));
        REQUIRE(enroll_and_approve(prefix + "-agent-a", {*group_a}));
        std::this_thread::sleep_for(1100ms); // per-source enrollment rate limit
        REQUIRE(enroll_and_approve(prefix + "-agent-b", {*group_b}));
        return *group_b;
    }

    // Enrolls an agent and approves it into the given groups.
    bool enroll_and_approve(const std::string& agent_id, const std::vector<int64_t>& group_ids)
    {
        return ServerFixture::enroll_and_approve(agent_id, "scope-test-host",
                                                 json{
                                                     {"group_ids", group_ids}
        }
                                                     .dump());
    }
};

} // namespace

SCENARIO("Per-agent history enforces group scoping")
{
    GIVEN("a server with group A and group B, an agent in each, and a viewer in group B")
    {
        ScopeFixture fx;
        fx.start_and_login();

        fx.seed_two_groups("scope", "scope_viewer", "group_viewer");

        REQUIRE(fx.login("scope_viewer", "sc0pepw!"));

        WHEN("the viewer requests the history of group A's agent")
        {
            auto res = fx.client.Get("/api/agents/scope-agent-a/history");

            THEN("the request is denied")
            {
                REQUIRE(res);
                REQUIRE(res->status == 403);
            }
        }

        WHEN("the viewer requests the history of group B's agent")
        {
            auto res = fx.client.Get("/api/agents/scope-agent-b/history");

            THEN("the request is allowed")
            {
                REQUIRE(res);
                REQUIRE(res->status == 200);
            }
        }
    }
}

SCENARIO("Runbooks are scoped to the sessions that can see their agent")
{
    GIVEN("a server with group A and group B, an agent in each, and a viewer in group B")
    {
        ScopeFixture fx;
        fx.start_and_login();

        fx.seed_two_groups("rb", "rb_viewer", "group_viewer");

        // The admin creates a generic runbook and an agent-scoped runbook for
        // group A's agent.
        auto generic =
            fx.client.Post("/api/runbooks",
                           json{
                               {"indicator", "cpu"                             },
                               {"status",    "red"                             },
                               {"url",       "https://wiki.example.com/generic"}
        }
                               .dump(),
                           "application/json");
        REQUIRE(generic);
        REQUIRE(generic->status == 201);
        auto scoped = fx.client.Post("/api/runbooks",
                                     json{
                                         {"agent_id",  "rb-agent-a"                     },
                                         {"indicator", "cpu"                            },
                                         {"status",    "red"                            },
                                         {"url",       "https://wiki.example.com/secret"}
        }
                                         .dump(),
                                     "application/json");
        REQUIRE(scoped);
        REQUIRE(scoped->status == 201);

        WHEN("the group B viewer lists runbooks")
        {
            REQUIRE(fx.login("rb_viewer", "sc0pepw!"));
            auto res = fx.client.Get("/api/runbooks");

            THEN("the generic runbook is visible and the group A runbook is not")
            {
                REQUIRE(res);
                REQUIRE(res->status == 200);
                const auto runbooks = json::parse(res->body);
                bool saw_generic = false;
                bool saw_scoped = false;
                for (const auto& rb : runbooks)
                {
                    if (rb.value("url", "") == "https://wiki.example.com/generic")
                        saw_generic = true;
                    if (rb.value("url", "") == "https://wiki.example.com/secret")
                        saw_scoped = true;
                }
                REQUIRE(saw_generic);
                REQUIRE_FALSE(saw_scoped);
            }
        }

        WHEN("the admin lists runbooks")
        {
            auto res = fx.client.Get("/api/runbooks");

            THEN("both runbooks are visible")
            {
                REQUIRE(res);
                REQUIRE(res->status == 200);
                const auto runbooks = json::parse(res->body);
                REQUIRE(runbooks.size() == 2);
            }
        }
    }
}

SCENARIO("Views cannot reference agents outside the creator's scope")
{
    GIVEN("a server with group A and group B, an agent in each, and an operator in group B")
    {
        ScopeFixture fx;
        fx.start_and_login();

        const auto group_b = fx.seed_two_groups("view", "view_operator", "group_operator");

        REQUIRE(fx.login("view_operator", "sc0pepw!"));

        WHEN("the operator creates a view referencing group A's agent")
        {
            auto res = fx.client.Post("/api/views",
                                      json{
                                          {"name",      "Cross-group leak"           },
                                          {"is_public", true                         },
                                          {"group_id",  group_b                      },
                                          {"agent_ids", json::array({"view-agent-a"})}
            }
                                          .dump(),
                                      "application/json");

            THEN("the request is rejected with 400")
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
            }
        }

        WHEN("the operator creates a view referencing only group B's agent")
        {
            auto res = fx.client.Post("/api/views",
                                      json{
                                          {"name",      "Own-group view"             },
                                          {"is_public", true                         },
                                          {"group_id",  group_b                      },
                                          {"agent_ids", json::array({"view-agent-b"})}
            }
                                          .dump(),
                                      "application/json");

            THEN("the request is accepted")
            {
                REQUIRE(res);
                REQUIRE(res->status == 201);
            }
        }
    }
}