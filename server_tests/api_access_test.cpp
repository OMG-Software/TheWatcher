// Access rules for views, agent groups, agent approval, pending enrollments,
// silences, maintenance windows and the Configure modal save (issues #60, #61,
// #62, #63, #66, #112 and #163). The full role x endpoint table lives in
// api_authorization_matrix_test.cpp.
#include "common/collector_config.hpp"
#include "common/crypto.hpp"
#include "test_support/api_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using nlohmann::json;
using thewatcher::server::ViewRecord;
using thewatcher::test::ApiFixture;

namespace
{

// A world with two groups, one user per interesting role and a few agents.
struct AccessWorld : ApiFixture
{
    AccessWorld()
        : group_a(add_group("Group A"))
        , group_b(add_group("Group B"))
    {
        add_user("admin", "global_admin");
        add_user("global_viewer", "global_viewer");
        add_user("g_op", "global_operator");
        add_user("a_admin", "group_admin", {group_a});
        add_user("a_op", "group_operator", {group_a});
        add_user("a_viewer", "group_viewer", {group_a});
        add_user("b_op", "group_operator", {group_b});
        add_agent("agent-a", true, {group_a});
        add_agent("agent-b", true, {group_b});
    }

    // Stores a view straight in the database, bypassing the API.
    int64_t add_view(const std::string& name, const std::string& owner, int64_t group_id, bool is_public,
                     std::vector<std::string> agent_ids = {})
    {
        ViewRecord rec;
        rec.name = name;
        rec.owner_user_id = store->get_user_by_username(owner)->user_id;
        rec.group_id = group_id;
        rec.is_public = is_public;
        rec.agent_ids = std::move(agent_ids);
        rec.created_at = now_ms();
        return store->create_view(rec);
    }

    static std::vector<std::string> names(const json& views)
    {
        std::vector<std::string> out;
        for (const auto& v : views)
            out.push_back(v.at("name").get<std::string>());
        std::sort(out.begin(), out.end());
        return out;
    }

    int64_t group_a;
    int64_t group_b;
};

bool contains(const std::vector<std::string>& haystack, const std::string& needle)
{
    return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

json collector_body(int interval, const json& extra = json::object())
{
    json body = {
        {"collection_interval", interval                           },
        {"process_limit",       10                                 },
        {"collector_config",    json(thewatcher::CollectorConfig{})}
    };
    body.update(extra);
    return body;
}

} // namespace

// ── #60 views with no group ────────────────────────────────────────────────────

SCENARIO("Group-scoped users see public and own ungrouped views (issue #60)")
{
    GIVEN("a public ungrouped view, a private ungrouped view, a view in each group and an own legacy view")
    {
        AccessWorld w;
        w.add_view("public-no-group", "admin", 0, true);
        w.add_view("private-no-group", "admin", 0, false);
        w.add_view("group-a-view", "admin", w.group_a, false);
        w.add_view("group-b-view", "admin", w.group_b, false);
        w.add_view("a-op-own-no-group", "a_op", 0, false);

        WHEN("a group A viewer lists the views")
        {
            const auto res = w.request("GET", "/api/views", "a_viewer");

            THEN("they get the public view and their group's view, and nothing else")
            {
                REQUIRE(res.status == 200);
                REQUIRE(AccessWorld::names(res.body) == std::vector<std::string>{"group-a-view", "public-no-group"});
            }
        }

        WHEN("a group A operator lists the views")
        {
            const auto res = w.request("GET", "/api/views", "a_op");

            THEN("their own ungrouped view is listed as well")
            {
                REQUIRE(AccessWorld::names(res.body) ==
                        std::vector<std::string>{"a-op-own-no-group", "group-a-view", "public-no-group"});
            }
        }

        WHEN("a group A viewer opens each view by id")
        {
            const auto pub = w.request("GET", "/api/views/1", "a_viewer");
            const auto priv = w.request("GET", "/api/views/2", "a_viewer");
            const auto other = w.request("GET", "/api/views/4", "a_viewer");

            THEN("the public view opens and the others are refused")
            {
                REQUIRE(pub.status == 200);
                REQUIRE(priv.status == 403);
                REQUIRE(other.status == 403);
            }
        }

        WHEN("a global viewer lists the views")
        {
            const auto res = w.request("GET", "/api/views", "global_viewer");

            THEN("every view is listed")
            {
                REQUIRE(res.body.size() == 5);
            }
        }
    }
}

// ── #61 group admins and operators manage their group's views ─────────────────

SCENARIO("Group admins and operators edit and delete views of their own group (issue #61)")
{
    GIVEN("a view in group A created by a global admin")
    {
        AccessWorld w;
        const auto view = w.add_view("team-view", "admin", w.group_a, false);
        const auto path = "/api/views/" + std::to_string(view);

        WHEN("a group A operator renames it")
        {
            const auto res = w.request("PUT", path, "a_op",
                                       {
                                           {"name", "renamed"}
            });

            THEN("the rename is applied")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_view(view)->name == "renamed");
            }
        }

        WHEN("a group A admin deletes it")
        {
            const auto res = w.request("DELETE", path, "a_admin");

            THEN("the view is gone")
            {
                REQUIRE(res.status == 200);
                REQUIRE_FALSE(w.store->get_view(view).has_value());
            }
        }

        WHEN("a group A viewer tries to rename or delete it")
        {
            const auto put = w.request("PUT", path, "a_viewer",
                                       {
                                           {"name", "x"}
            });
            const auto del = w.request("DELETE", path, "a_viewer");

            THEN("both are refused and the view is unchanged")
            {
                REQUIRE(put.status == 403);
                REQUIRE(del.status == 403);
                REQUIRE(w.store->get_view(view)->name == "team-view");
            }
        }

        WHEN("an operator of another group tries to rename or delete it")
        {
            const auto put = w.request("PUT", path, "b_op",
                                       {
                                           {"name", "x"}
            });
            const auto del = w.request("DELETE", path, "b_op");

            THEN("both are refused and the view is unchanged")
            {
                REQUIRE(put.status == 403);
                REQUIRE(del.status == 403);
                REQUIRE(w.store->get_view(view)->name == "team-view");
            }
        }

        WHEN("a group A operator moves it into a group they do not belong to")
        {
            const auto res = w.request("PUT", path, "a_op",
                                       {
                                           {"group_id", w.group_b}
            });

            THEN("the move is rejected and the group is unchanged")
            {
                REQUIRE(res.status == 400);
                REQUIRE(w.store->get_view(view)->group_id == w.group_a);
            }
        }
    }

    GIVEN("a public ungrouped view owned by a global admin")
    {
        AccessWorld w;
        const auto view = w.add_view("everyone", "admin", 0, true);

        WHEN("a group A admin tries to delete it")
        {
            const auto res = w.request("DELETE", "/api/views/" + std::to_string(view), "a_admin");

            THEN("it is refused: public ungrouped views belong to their owner and global operators")
            {
                REQUIRE(res.status == 403);
                REQUIRE(w.store->get_view(view).has_value());
            }
        }
    }

    GIVEN("a group operator who owns a private ungrouped view")
    {
        AccessWorld w;
        const auto view = w.add_view("mine", "a_op", 0, false);

        WHEN("they rename it without touching its group")
        {
            const auto res = w.request("PUT", "/api/views/" + std::to_string(view), "a_op",
                                       {
                                           {"name", "still-mine"}
            });

            THEN("the rename succeeds")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_view(view)->name == "still-mine");
            }
        }
    }
}

SCENARIO("A view group must exist (issue #62)")
{
    GIVEN("a global admin")
    {
        AccessWorld w;

        WHEN("they create a view in a group that does not exist")
        {
            const auto res = w.request("POST", "/api/views", "admin",
                                       {
                                           {"name",     "ghost"},
                                           {"group_id", 999    }
            });

            THEN("it is rejected")
            {
                REQUIRE(res.status == 400);
                REQUIRE(w.store->list_views(0).empty());
            }
        }
    }
}

// ── #163 views never reveal agents the caller cannot see ──────────────────────

SCENARIO("Views only list the agents the caller may see (issue #163)")
{
    GIVEN("a group A view that references an agent of group A and an agent of group B")
    {
        AccessWorld w;
        const auto view = w.add_view("mixed", "admin", w.group_a, false, {"agent-a", "agent-b"});
        const auto path = "/api/views/" + std::to_string(view);

        WHEN("a group A viewer reads the list and the single view")
        {
            const auto list = w.request("GET", "/api/views", "a_viewer");
            const auto one = w.request("GET", path, "a_viewer");

            THEN("agent-b is not disclosed")
            {
                REQUIRE(list.body.at(0).at("agent_ids") == json::array({"agent-a"}));
                REQUIRE(one.body.at("agent_ids") == json::array({"agent-a"}));
            }
        }

        WHEN("a global admin reads the view")
        {
            const auto one = w.request("GET", path, "admin");

            THEN("both agents are listed")
            {
                REQUIRE(one.body.at("agent_ids") == json::array({"agent-a", "agent-b"}));
            }
        }

        WHEN("a group A operator renames the view")
        {
            const auto res = w.request("PUT", path, "a_op",
                                       {
                                           {"name",      "renamed"  },
                                           {"agent_ids", {"agent-a"}}
            });

            THEN("the response hides agent-b but the stored view keeps it")
            {
                REQUIRE(res.status == 200);
                REQUIRE(res.body.at("agent_ids") == json::array({"agent-a"}));
                const auto stored = w.store->get_view(view)->agent_ids;
                REQUIRE(stored.size() == 2);
                REQUIRE(contains(stored, "agent-b"));
            }
        }

        WHEN("a group A operator adds an agent of another group")
        {
            const auto res = w.request("PUT", path, "a_op",
                                       {
                                           {"agent_ids", {"agent-a", "agent-b"}}
            });

            THEN("the edit is rejected")
            {
                REQUIRE(res.status == 400);
            }
        }
    }
}

// ── #62 the Configure save and agent group changes ────────────────────────────

SCENARIO("Group changes through the Configure save are explicit and validated (issue #62)")
{
    GIVEN("an agent in group A")
    {
        AccessWorld w;
        const std::string path = "/api/agents/agent-a/collector_config";

        WHEN("a global admin moves it to group B")
        {
            const auto res = w.request("POST", path, "admin",
                                       collector_body(45, {
                                                              {"group_ids", {w.group_b}}
            }));

            THEN("the agent is only in group B and the rest of the config is saved")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_agent_groups("agent-a") == std::vector<int64_t>{w.group_b});
                REQUIRE(w.store->get_agent("agent-a")->collection_interval == 45);
            }
        }

        WHEN("a global admin saves a group id that does not exist")
        {
            const auto res = w.request("POST", path, "admin",
                                       collector_body(45, {
                                                              {"group_ids", {999}}
            }));

            THEN("the save is rejected as a whole")
            {
                REQUIRE(res.status == 400);
                REQUIRE(w.store->get_agent_groups("agent-a") == std::vector<int64_t>{w.group_a});
                REQUIRE(w.store->get_agent("agent-a")->collection_interval == 30);
            }
        }

        WHEN("a group operator tries to move it to another group")
        {
            const auto res = w.request("POST", path, "a_op",
                                       collector_body(45, {
                                                              {"group_ids", {w.group_b}}
            }));

            THEN("they get an explicit 403 and nothing is saved")
            {
                REQUIRE(res.status == 403);
                REQUIRE(res.body.at("error").get<std::string>().find("group") != std::string::npos);
                REQUIRE(w.store->get_agent_groups("agent-a") == std::vector<int64_t>{w.group_a});
                REQUIRE(w.store->get_agent("agent-a")->collection_interval == 30);
            }
        }

        WHEN("a group operator saves the unchanged group list with other edits")
        {
            const auto res = w.request("POST", path, "a_op",
                                       collector_body(45, {
                                                              {"group_ids", {w.group_a}}
            }));

            THEN("the save succeeds")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_agent("agent-a")->collection_interval == 45);
            }
        }

        WHEN("group_ids is not an array of integers")
        {
            const auto res = w.request("POST", path, "admin",
                                       collector_body(45, {
                                                              {"group_ids", "a"}
            }));

            THEN("it is a 400")
            {
                REQUIRE(res.status == 400);
            }
        }

        WHEN("the dedicated groups endpoint is given unknown groups or an unknown agent")
        {
            const auto bad_group = w.request("POST", "/api/agents/agent-a/groups", "admin",
                                             {
                                                 {"group_ids", {999}}
            });
            const auto bad_agent = w.request("POST", "/api/agents/nobody/groups", "admin",
                                             {
                                                 {"group_ids", {w.group_a}}
            });

            THEN("they are 400 and 404 and the groups are unchanged")
            {
                REQUIRE(bad_group.status == 400);
                REQUIRE(bad_agent.status == 404);
                REQUIRE(w.store->get_agent_groups("agent-a") == std::vector<int64_t>{w.group_a});
            }
        }
    }
}

// ── #112 the Configure save does not overwrite concurrent edits ───────────────

SCENARIO("A stale Configure save is refused instead of overwriting a concurrent edit (issue #112)")
{
    GIVEN("two operators who both opened the same agent's Configure modal")
    {
        AccessWorld w;
        const auto listed = w.request("GET", "/api/agents", "admin");
        const auto revision = [&] {
            for (const auto& a : listed.body)
                if (a.at("agent_id") == "agent-a")
                    return a.at("config_revision").get<std::string>();
            return std::string();
        }();
        REQUIRE_FALSE(revision.empty());
        const std::string path = "/api/agents/agent-a/collector_config";

        AND_GIVEN("operator B saved a new collection interval first")
        {
            const auto first =
                w.request("POST", path, "a_op",
                          collector_body(99, {
                                                 {"config_revision", revision   },
                                                 {"group_ids",       {w.group_a}}
            }));
            REQUIRE(first.status == 200);

            WHEN("operator A saves a runbook from the older snapshot")
            {
                const auto res = w.request("POST", path, "a_admin",
                                           collector_body(30, {
                                                                  {"config_revision",  revision     },
                                                                  {"runbook_markdown", "A's runbook"},
                                                                  {"group_ids",        {w.group_a}  }
                }));

                THEN("A gets a 409 and B's interval and A's absent runbook are what is stored")
                {
                    REQUIRE(res.status == 409);
                    const auto stored = w.store->get_agent("agent-a");
                    REQUIRE(stored->collection_interval == 99);
                    REQUIRE(stored->runbook_markdown.empty());
                }
            }

            WHEN("operator A reloads the agent and saves again")
            {
                const auto relisted = w.request("GET", "/api/agents", "admin");
                std::string fresh;
                for (const auto& a : relisted.body)
                    if (a.at("agent_id") == "agent-a")
                        fresh = a.at("config_revision").get<std::string>();
                const auto res = w.request("POST", path, "a_admin",
                                           collector_body(99, {
                                                                  {"config_revision",  fresh        },
                                                                  {"runbook_markdown", "A's runbook"},
                                                                  {"group_ids",        {w.group_a}  }
                }));

                THEN("the save is accepted and both edits are present")
                {
                    REQUIRE(fresh != revision);
                    REQUIRE(res.status == 200);
                    const auto stored = w.store->get_agent("agent-a");
                    REQUIRE(stored->collection_interval == 99);
                    REQUIRE(stored->runbook_markdown == "A's runbook");
                }
            }
        }
    }

    GIVEN("an agent that belongs to two groups")
    {
        AccessWorld w;
        w.store->set_agent_groups("agent-a", {w.group_a, w.group_b});

        WHEN("the modal saves the full membership list back")
        {
            const auto res = w.request("POST", "/api/agents/agent-a/collector_config", "admin",
                                       collector_body(30, {
                                                              {"group_ids", {w.group_a, w.group_b}}
            }));

            THEN("both memberships survive the edit")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_agent_groups("agent-a") == std::vector<int64_t>{w.group_a, w.group_b});
            }
        }
    }

    GIVEN("a client that sends no revision")
    {
        AccessWorld w;

        WHEN("it saves the config")
        {
            const auto res = w.request("POST", "/api/agents/agent-a/collector_config", "admin", collector_body(77));

            THEN("the save is applied unconditionally")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_agent("agent-a")->collection_interval == 77);
            }
        }
    }
}

// ── #63 approval names its groups and is atomic ───────────────────────────────

SCENARIO("Approving an agent names its groups and changes nothing on a bad request (issue #63)")
{
    GIVEN("groups Prod and Dev and a pending agent")
    {
        thewatcher::crypto::init();
        AccessWorld w;
        const auto prod = w.add_group("Prod");
        const auto dev = w.add_group("Dev");
        const auto keys = thewatcher::crypto::generate_curve_keypair();
        thewatcher::server::AgentRecord rec;
        rec.agent_id = "new-host";
        rec.curve_public_key_z85 = keys.public_key_z85;
        w.store->upsert_agent(rec);
        const std::string path = "/api/agents/new-host/approve";

        WHEN("the approval has no body")
        {
            const auto res = w.request("POST", path, "admin");

            THEN("it is a 400 and the agent is still pending")
            {
                REQUIRE(res.status == 400);
                REQUIRE_FALSE(w.store->get_agent("new-host")->approved);
            }
        }

        WHEN("the body has a malformed group list")
        {
            const auto not_array = w.request("POST", path, "admin",
                                             {
                                                 {"group_ids", "Dev"}
            });
            const auto missing = w.request("POST", path, "admin", json::object());

            THEN("both are 400 and the agent is still pending with no groups and no ZAP key")
            {
                REQUIRE(not_array.status == 400);
                REQUIRE(missing.status == 400);
                REQUIRE_FALSE(w.store->get_agent("new-host")->approved);
                REQUIRE(w.store->get_agent_groups("new-host").empty());
                REQUIRE_FALSE(w.zap.user_id_for_key(keys.public_key_z85).has_value());
            }
        }

        WHEN("the body names a group that does not exist")
        {
            const auto res = w.request("POST", path, "admin",
                                       {
                                           {"group_ids", {dev, 999}}
            });

            THEN("it is a 400 and the agent is still pending with no groups")
            {
                REQUIRE(res.status == 400);
                REQUIRE_FALSE(w.store->get_agent("new-host")->approved);
                REQUIRE(w.store->get_agent_groups("new-host").empty());
            }
        }

        WHEN("an admin approves it into Dev")
        {
            const auto res = w.request("POST", path, "admin",
                                       {
                                           {"group_ids", {dev}}
            });

            THEN("it is approved, only in Dev, and its key is bound in ZAP")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_agent("new-host")->approved);
                REQUIRE(w.store->get_agent_groups("new-host") == std::vector<int64_t>{dev});
                REQUIRE(w.zap.user_id_for_key(keys.public_key_z85) == std::optional<std::string>("new-host"));
            }
        }

        WHEN("an admin approves it with an empty group list")
        {
            const auto res = w.request("POST", path, "admin",
                                       {
                                           {"group_ids", json::array()}
            });

            THEN("it is approved with no groups: visible to global users only")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_agent_groups("new-host").empty());
            }
        }

        WHEN("it is approved twice")
        {
            const json body = {
                {"group_ids", {dev}}
            };
            REQUIRE(w.request("POST", path, "admin", body).status == 200);
            const auto again = w.request("POST", path, "admin",
                                         {
                                             {"group_ids", {prod}}
            });

            THEN("the second approval is a 409 and the groups are unchanged")
            {
                REQUIRE(again.status == 409);
                REQUIRE(w.store->get_agent_groups("new-host") == std::vector<int64_t>{dev});
            }
        }

        WHEN("it was rejected before")
        {
            REQUIRE(w.request("POST", "/api/agents/new-host/reject", "admin").status == 200);
            const auto res = w.request("POST", path, "admin",
                                       {
                                           {"group_ids", json::array()}
            });

            THEN("approval is a 409 and the agent stays rejected")
            {
                REQUIRE(res.status == 409);
                REQUIRE(w.store->get_agent("new-host")->rejected);
            }
        }

        WHEN("the agent id is unknown")
        {
            const auto res = w.request("POST", "/api/agents/ghost/approve", "admin",
                                       {
                                           {"group_ids", json::array()}
            });

            THEN("it is a 404")
            {
                REQUIRE(res.status == 404);
            }
        }
    }
}

// ── #66 read access for viewer roles ──────────────────────────────────────────

SCENARIO("Global viewers read pending enrollments and silences; group users see wildcard windows (issue #66)")
{
    GIVEN("a pending agent, a silence, an all-agents maintenance window and one for agent-a")
    {
        AccessWorld w;
        w.add_agent("pending-host", false);
        thewatcher::server::SilenceRecord silence;
        silence.agent_id = "agent-a";
        silence.indicator = "*";
        silence.until_ms = ApiFixture::now_ms() + 3600000;
        silence.created_at = ApiFixture::now_ms();
        w.store->create_silence(silence);
        thewatcher::server::MaintenanceWindowRecord all_window;
        all_window.agent_id = "*";
        all_window.start_ms = ApiFixture::now_ms();
        all_window.end_ms = all_window.start_ms + 3600000;
        const auto all_id = w.store->create_maintenance_window(all_window);
        auto own_window = all_window;
        own_window.agent_id = "agent-a";
        w.store->create_maintenance_window(own_window);

        WHEN("a global viewer lists pending enrollments and silences")
        {
            const auto pending = w.request("GET", "/api/pending-enrollments", "global_viewer");
            const auto silences = w.request("GET", "/api/silences", "global_viewer");

            THEN("both are 200 with data")
            {
                REQUIRE(pending.status == 200);
                REQUIRE(pending.body.size() == 1);
                REQUIRE(silences.status == 200);
                REQUIRE(silences.body.size() == 1);
            }
        }

        WHEN("a global viewer tries to approve the pending agent")
        {
            const auto res = w.request("POST", "/api/agents/pending-host/approve", "global_viewer",
                                       {
                                           {"group_ids", json::array()}
            });

            THEN("reading is allowed but changing is not")
            {
                REQUIRE(res.status == 403);
                REQUIRE_FALSE(w.store->get_agent("pending-host")->approved);
            }
        }

        WHEN("a group operator asks for pending enrollments")
        {
            const auto res = w.request("GET", "/api/pending-enrollments", "a_op");

            THEN("they are refused: pending agents belong to no group")
            {
                REQUIRE(res.status == 403);
            }
        }

        WHEN("a group viewer lists maintenance windows")
        {
            const auto res = w.request("GET", "/api/maintenance-windows", "a_viewer");

            THEN("they see the all-agents window and the one for their agent")
            {
                REQUIRE(res.status == 200);
                REQUIRE(res.body.size() == 2);
            }
        }

        WHEN("a group operator tries to delete the all-agents window")
        {
            const auto res = w.request("DELETE", "/api/maintenance-windows/" + std::to_string(all_id), "a_op");

            THEN("it is read-only to them")
            {
                REQUIRE(res.status == 403);
                REQUIRE(w.store->list_maintenance_windows().size() == 2);
            }
        }
    }
}

// ── Password changes do not reveal which user ids exist ──────────────────────

SCENARIO("A password change checks permission before it looks the user up")
{
    GIVEN("a global admin, a group admin and a group viewer")
    {
        AccessWorld w;
        const auto victim = w.add_user("victim", "group_viewer", {w.group_a});
        const json body = {
            {"password", "a-new-long-password"}
        };

        WHEN("a group viewer targets an id that does not exist")
        {
            const auto res = w.request("PUT", "/api/users/99999/password", "a_viewer", body);

            THEN("they get 403, the same as for a real user that is not them")
            {
                REQUIRE(res.status == 403);
                REQUIRE(
                    w.request("PUT", "/api/users/" + std::to_string(victim) + "/password", "a_viewer", body).status ==
                    403);
            }
        }

        WHEN("a group admin targets an id that does not exist")
        {
            THEN("they get 403: group admins may only change their own password")
            {
                REQUIRE(w.request("PUT", "/api/users/99999/password", "a_admin", body).status == 403);
            }
        }

        WHEN("a global admin targets an id that does not exist")
        {
            const auto res = w.request("PUT", "/api/users/99999/password", "admin", body);

            THEN("they get 404")
            {
                REQUIRE(res.status == 404);
            }
        }

        WHEN("a global admin sends a non-numeric id or a bad body")
        {
            const auto bad_id = w.request("PUT", "/api/users/abc/password", "admin", body);
            const auto bad_body = w.request("PUT", "/api/users/" + std::to_string(victim) + "/password", "admin",
                                            {
                                                {"password", ""}
            });

            THEN("both stay 400")
            {
                REQUIRE(bad_id.status == 400);
                REQUIRE(bad_body.status == 400);
            }
        }
    }
}

// ── Rejecting an agent ────────────────────────────────────────────────────────

SCENARIO("Rejecting an agent only works on a pending enrollment")
{
    GIVEN("a pending agent, an approved agent and a rejected agent")
    {
        AccessWorld w;
        w.add_agent("pending-host", false);
        w.add_agent("rejected-host", false);
        REQUIRE(w.request("POST", "/api/agents/rejected-host/reject", "admin").status == 200);

        WHEN("an admin rejects the pending agent")
        {
            const auto res = w.request("POST", "/api/agents/pending-host/reject", "admin");

            THEN("it is rejected")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_agent("pending-host")->rejected);
            }
        }

        WHEN("an admin rejects an agent that does not exist")
        {
            THEN("it is a 404")
            {
                REQUIRE(w.request("POST", "/api/agents/ghost/reject", "admin").status == 404);
            }
        }

        WHEN("an admin rejects an approved agent")
        {
            const auto res = w.request("POST", "/api/agents/agent-a/reject", "admin");

            THEN("it is a 409 and the agent stays approved")
            {
                REQUIRE(res.status == 409);
                REQUIRE(w.store->get_agent("agent-a")->approved);
                REQUIRE_FALSE(w.store->get_agent("agent-a")->rejected);
            }
        }

        WHEN("an admin rejects an already rejected agent")
        {
            THEN("it is a 409")
            {
                REQUIRE(w.request("POST", "/api/agents/rejected-host/reject", "admin").status == 409);
            }
        }
    }
}

// ── Deleting an agent is an admin action ─────────────────────────────────────

SCENARIO("Only admins delete agents")
{
    GIVEN("agents in groups A and B")
    {
        AccessWorld w;
        w.add_agent("a-1", true, {w.group_a});
        w.add_agent("a-2", true, {w.group_a});
        w.add_agent("b-1", true, {w.group_b});

        WHEN("a global operator or a group operator deletes an agent")
        {
            const auto global_op = w.request("DELETE", "/api/agents/a-1", "g_op");
            const auto group_op = w.request("DELETE", "/api/agents/a-1", "a_op");

            THEN("both are refused and the agent is kept")
            {
                REQUIRE(global_op.status == 403);
                REQUIRE(group_op.status == 403);
                REQUIRE(w.store->get_agent("a-1").has_value());
            }
        }

        WHEN("a group admin deletes an agent of their own group")
        {
            const auto res = w.request("DELETE", "/api/agents/a-1", "a_admin");

            THEN("it is deleted")
            {
                REQUIRE(res.status == 200);
                REQUIRE_FALSE(w.store->get_agent("a-1").has_value());
            }
        }

        WHEN("a group admin deletes an agent of another group, or an unknown id")
        {
            const auto other = w.request("DELETE", "/api/agents/b-1", "a_admin");
            const auto unknown = w.request("DELETE", "/api/agents/ghost", "a_admin");

            THEN("both are 403 and nothing is deleted")
            {
                REQUIRE(other.status == 403);
                REQUIRE(unknown.status == 403);
                REQUIRE(w.store->get_agent("b-1").has_value());
            }
        }

        WHEN("a global admin deletes any agent or an unknown id")
        {
            const auto other = w.request("DELETE", "/api/agents/b-1", "admin");
            const auto unknown = w.request("DELETE", "/api/agents/ghost", "admin");

            THEN("the agent is deleted and the unknown id is a 404")
            {
                REQUIRE(other.status == 200);
                REQUIRE(unknown.status == 404);
                REQUIRE_FALSE(w.store->get_agent("b-1").has_value());
            }
        }
    }
}

// ── Webhook URLs are secrets: only admins read or change them ────────────────

SCENARIO("Webhook URLs are masked for global operators and cannot be changed by them")
{
    GIVEN("stored webhook URLs with a secret path")
    {
        AccessWorld w;
        const std::string alert_url = "https://hooks.slack.com/services/T000/B000/secret";
        const std::string report_url = "https://reports.example.com:8443/in/token?key=abc";
        const std::string alert_mask = "https://hooks.slack.com/\xE2\x80\xA6";
        const std::string report_mask = "https://reports.example.com:8443/\xE2\x80\xA6";
        w.store->set_setting("notifications.webhook_url", alert_url);
        w.store->set_setting("reports.webhook_url", report_url);

        WHEN("a global admin reads the settings")
        {
            const auto res = w.request("GET", "/api/settings", "admin");

            THEN("the full URLs are returned and nothing is flagged as masked")
            {
                REQUIRE(res.status == 200);
                REQUIRE(res.body.at("webhook_url") == alert_url);
                REQUIRE(res.body.at("reports_webhook_url") == report_url);
                REQUIRE(res.body.at("webhooks_masked") == false);
            }
        }

        WHEN("a global operator reads the settings")
        {
            const auto res = w.request("GET", "/api/settings", "g_op");

            THEN("only scheme and host are shown and the response says so")
            {
                REQUIRE(res.status == 200);
                REQUIRE(res.body.at("webhook_url") == alert_mask);
                REQUIRE(res.body.at("reports_webhook_url") == report_mask);
                REQUIRE(res.body.at("webhooks_masked") == true);
                REQUIRE(res.body.at("offline_after_seconds").is_number());
            }
        }

        WHEN("a global operator changes a webhook URL, clears it, or changes it along with other settings")
        {
            const auto change = w.request("PUT", "/api/settings", "g_op",
                                          {
                                              {"webhook_url", "https://evil.example/hook"}
            });
            const auto clear = w.request("PUT", "/api/settings", "g_op",
                                         {
                                             {"reports_webhook_url", ""}
            });
            const auto mixed =
                w.request("PUT", "/api/settings", "g_op",
                          {
                              {"offline_after_seconds", 300                        },
                              {"webhook_url",           "https://evil.example/hook"}
            });

            THEN("all are 403 and nothing is written, not even the other settings")
            {
                REQUIRE(change.status == 403);
                REQUIRE(clear.status == 403);
                REQUIRE(mixed.status == 403);
                REQUIRE(w.store->get_setting("notifications.webhook_url") == alert_url);
                REQUIRE(w.store->get_setting("reports.webhook_url") == report_url);
                REQUIRE(w.store->get_setting("offline_after_seconds", "120") == "120");
            }
        }

        WHEN("a global operator saves other settings, with the masked placeholders sent back unchanged")
        {
            const auto res = w.request(
                "PUT", "/api/settings", "g_op",
                {
                    {"offline_after_seconds", 300        },
                    {"webhook_url",           alert_mask },
                    {"reports_webhook_url",   report_mask}
            });

            THEN("the other setting is saved and the stored URLs are untouched")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_setting("offline_after_seconds") == "300");
                REQUIRE(w.store->get_setting("notifications.webhook_url") == alert_url);
                REQUIRE(w.store->get_setting("reports.webhook_url") == report_url);
            }
        }

        WHEN("a global admin saves the masked placeholder back")
        {
            const auto res = w.request("PUT", "/api/settings", "admin",
                                       {
                                           {"webhook_url", alert_mask}
            });

            THEN("the placeholder is never stored over the real URL")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_setting("notifications.webhook_url") == alert_url);
            }
        }

        WHEN("a global admin sets a new URL")
        {
            const auto res = w.request("PUT", "/api/settings", "admin",
                                       {
                                           {"webhook_url", "https://new.example/hook"}
            });

            THEN("it is stored")
            {
                REQUIRE(res.status == 200);
                REQUIRE(w.store->get_setting("notifications.webhook_url") == "https://new.example/hook");
            }
        }
    }
}

// ── Commands to an offline agent respect group scope (#54 with #62) ──────────

SCENARIO("A command for an offline agent is refused after the access check, not instead of it")
{
    GIVEN("an offline agent in group B and another in group A")
    {
        AccessWorld w;
        w.agents_reachable = false;

        WHEN("a group A operator commands the group B agent")
        {
            const auto res = w.request("POST", "/api/agents/agent-b/get_status", "a_op");

            THEN("they get 403, so the agent's connection state is not revealed")
            {
                REQUIRE(res.status == 403);
            }
        }

        WHEN("a group A operator commands their own offline agent")
        {
            const auto res = w.request("POST", "/api/agents/agent-a/get_status", "a_op");

            THEN("they get the 409 that says the command was not queued")
            {
                REQUIRE(res.status == 409);
            }
        }

        WHEN("a group viewer commands their own offline agent")
        {
            const auto res = w.request("POST", "/api/agents/agent-a/get_status", "a_viewer");

            THEN("they get 403: the role check comes first")
            {
                REQUIRE(res.status == 403);
            }
        }
    }
}
