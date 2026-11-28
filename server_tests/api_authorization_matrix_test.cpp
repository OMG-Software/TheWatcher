// Role x endpoint authorization matrix (issue #71).
//
// Every route registered in server/api.cpp is called once per role, and each
// row states the status that role must get. The table IS the policy: it is
// mirrored in docs/authorization.md. A new route needs a row here; a policy
// change needs an edited row.
//
// Rules the table encodes (see docs/authorization.md):
//   401  no session
//   403  the role may never use the route, or the resource is outside the
//        caller's groups (the API answers 403, not 404, for a resource that
//        exists but is out of scope; 404 means the id does not exist)
//   400  a valid request shape is not needed to prove a refusal, so rows send
//        valid bodies and an allowed role sees the success status
// Each role runs against its own fresh server, so rows may consume the
// resource they name (delete, approve, ...) without affecting other rows.
#include "common/collector_config.hpp"
#include "test_support/api_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <map>
#include <string>
#include <vector>

using nlohmann::json;
using thewatcher::test::ApiFixture;

namespace
{

// Column order of every expectation row.
enum Column : std::size_t
{
    None = 0,
    GlobalAdmin,
    GlobalOperator,
    GlobalViewer,
    GroupAdmin,
    GroupOperator,
    GroupViewer,
    ColumnCount
};

using Expect = std::array<int, ColumnCount>;

struct Row
{
    const char* method;
    const char* path; // may hold {placeholders}, see World::expand
    const char* body; // JSON text with placeholders, or "" for no body
    Expect expect;
};

struct RoleCase
{
    const char* label;
    const char* stored_role; // as stored in the users table
    Column column;           // whose expectations apply
    bool group_scoped;
};

const std::vector<RoleCase>& role_cases()
{
    static const std::vector<RoleCase> cases = {
        {"no session",      "",                None,           false},
        {"global_admin",    "global_admin",    GlobalAdmin,    false},
        {"global_operator", "global_operator", GlobalOperator, false},
        {"global_viewer",   "global_viewer",   GlobalViewer,   false},
        {"group_admin",     "group_admin",     GroupAdmin,     true },
        {"group_operator",  "group_operator",  GroupOperator,  true },
        {"group_viewer",    "group_viewer",    GroupViewer,    true },
        // Legacy role names are aliases and must behave exactly like their global counterparts.
        {"legacy admin",    "admin",           GlobalAdmin,    false},
        {"legacy operator", "operator",        GlobalOperator, false},
        {"legacy viewer",   "viewer",          GlobalViewer,   false},
    };
    return cases;
}

// The resources the rows refer to. "a" belongs to group A, which every
// group-scoped test user is a member of; "b" belongs to group B, which none is.
struct World
{
    std::map<std::string, std::string> names;

    void set(const std::string& key, const std::string& value)
    {
        names["{" + key + "}"] = value;
    }

    void set(const std::string& key, int64_t value)
    {
        set(key, std::to_string(value));
    }

    std::string expand(std::string text) const
    {
        for (const auto& [placeholder, value] : names)
        {
            for (auto pos = text.find(placeholder); pos != std::string::npos; pos = text.find(placeholder, pos))
            {
                text.replace(pos, placeholder.size(), value);
                pos += value.size();
            }
        }
        return text;
    }
};

int64_t make_alert(ApiFixture& fx, const std::string& agent_id)
{
    thewatcher::server::AlertRecord alert;
    alert.agent_id = agent_id;
    alert.indicator = "cpu";
    alert.new_status = "red";
    alert.message = "cpu high";
    alert.created_at = ApiFixture::now_ms();
    return fx.store->insert_alert(alert);
}

int64_t make_view(ApiFixture& fx, const std::string& name, int64_t group_id, bool is_public)
{
    thewatcher::server::ViewRecord view;
    view.name = name;
    view.owner_user_id = fx.store->get_user_by_username("owner")->user_id;
    view.group_id = group_id;
    view.is_public = is_public;
    view.created_at = ApiFixture::now_ms();
    return fx.store->create_view(view);
}

int64_t make_window(ApiFixture& fx, const std::string& agent_id)
{
    thewatcher::server::MaintenanceWindowRecord window;
    window.agent_id = agent_id;
    window.start_ms = ApiFixture::now_ms();
    window.end_ms = window.start_ms + 3600000;
    return fx.store->create_maintenance_window(window);
}

int64_t make_silence(ApiFixture& fx, const std::string& agent_id)
{
    thewatcher::server::SilenceRecord silence;
    silence.agent_id = agent_id;
    silence.indicator = "*";
    silence.created_at = ApiFixture::now_ms();
    silence.until_ms = silence.created_at + 3600000;
    return fx.store->create_silence(silence);
}

// Builds the groups, agents and other resources every row may name. `fx`
// already holds the session user under test, added as "subject".
World build_world(ApiFixture& fx, int64_t group_a, int64_t group_b, int64_t self_id)
{
    World w;
    w.set("group_a", group_a);
    w.set("group_b", group_b);
    w.set("self", self_id);

    // A stored webhook URL, so a request that clears or replaces it is a change.
    fx.store->set_setting("reports.webhook_url", "https://hooks.example/secret-token");
    fx.add_agent("agent-a", true, {group_a});
    fx.add_agent("agent-b", true, {group_b});
    fx.add_agent("agent-del-a", true, {group_a});
    fx.add_agent("agent-del-b", true, {group_b});
    fx.add_agent("pending-1", false);
    fx.add_agent("pending-2", false);
    fx.add_agent("pending-3", false);
    w.set("agent_a", "agent-a");
    w.set("agent_b", "agent-b");

    w.set("alert_ack_a", make_alert(fx, "agent-a"));
    w.set("alert_ack_b", make_alert(fx, "agent-b"));
    w.set("alert_bulk_ack", make_alert(fx, "agent-a"));
    w.set("alert_bulk_archive", make_alert(fx, "agent-a"));
    w.set("alert_del_a", make_alert(fx, "agent-a"));
    w.set("alert_del_b", make_alert(fx, "agent-b"));

    fx.store->create_user("owner", "not-a-real-hash", "global_admin");
    w.set("view_a", make_view(fx, "in-a", group_a, false));
    w.set("view_b", make_view(fx, "in-b", group_b, false));
    w.set("view_public", make_view(fx, "public", 0, true));
    w.set("view_private", make_view(fx, "private", 0, false));
    for (const auto* key : {"put_a", "put_b", "put_public", "del_a", "del_b", "del_public"})
    {
        const std::string k = key;
        const bool in_a = k.ends_with("_a");
        const bool in_b = k.ends_with("_b");
        w.set("view_" + k, make_view(fx, k, in_a ? group_a : in_b ? group_b : 0, k.ends_with("public")));
    }

    for (const auto* key : {"mw_del_a", "mw_del_b", "mw_del_all"})
    {
        const std::string k = key;
        w.set(k, make_window(fx, k.ends_with("_a") ? "agent-a" : k.ends_with("_b") ? "agent-b" : "*"));
    }
    for (const auto* key : {"silence_del_a", "silence_del_b", "silence_del_all"})
    {
        const std::string k = key;
        w.set(k, make_silence(fx, k.ends_with("_a") ? "agent-a" : k.ends_with("_b") ? "agent-b" : "*"));
    }

    // Victims of the user-management rows, one per row.
    for (const auto* key : {"victim_disable", "victim_enable", "victim_delete", "victim_password"})
        w.set(key, fx.store->create_user(key, "not-a-real-hash", "group_viewer"));

    thewatcher::server::RunbookRecord runbook;
    runbook.status = "red";
    runbook.indicator = "*";
    runbook.url = "https://runbooks.example/one";
    w.set("runbook_del", fx.store->create_runbook(runbook));
    return w;
}

// A complete, valid thresholds payload (every indicator needs all three values).
const std::string thresholds_body =
    json{
        {"thresholds",
         {{"cpu", {{"warning_pct_of_avg", 125.0}, {"degraded_pct_of_avg", 150.0}, {"critical_pct_of_avg", 200.0}}},
          {"memory", {{"warning_pct_of_avg", 125.0}, {"degraded_pct_of_avg", 150.0}, {"critical_pct_of_avg", 200.0}}},
          {"disk", {{"warning_pct_of_avg", 125.0}, {"degraded_pct_of_avg", 150.0}, {"critical_pct_of_avg", 200.0}}},
          {"network",
           {{"warning_pct_of_avg", 125.0}, {"degraded_pct_of_avg", 150.0}, {"critical_pct_of_avg", 200.0}}}}}
}
        .dump();

const std::string collector_config_body = json{
    {"collection_interval", 30          },
    {"process_limit",       10          },
    {"collector_config",
     json(thewatcher::CollectorConfig{})}
}.dump();

// Shorthands for the rows below. Columns: none, global admin, global
// operator, global viewer, group admin, group operator, group viewer.
//   ANYONE      every logged-in role
//   OPERATORS   global admin/operator and group admin/operator
//   GLOBAL_OPS  global admin/operator
//   ADMIN       global admin only
constexpr Expect ANYONE{401, 200, 200, 200, 200, 200, 200};
constexpr Expect OPERATORS{401, 200, 200, 403, 200, 200, 403};
constexpr Expect GLOBAL_OPS{401, 200, 200, 403, 403, 403, 403};
constexpr Expect GLOBAL_ANY{401, 200, 200, 200, 403, 403, 403};
constexpr Expect ADMIN{401, 200, 403, 403, 403, 403, 403};
constexpr Expect GLOBAL_READ{401, 200, 200, 200, 403, 403, 403};
// Views are created with 201.
constexpr Expect CREATE_OPERATORS{401, 201, 201, 403, 201, 201, 403};

const std::vector<Row>& rows()
{
    static const std::vector<Row> table = {
        // ── Session ───────────────────────────────────────────────────────────
        // Login and logout are public; an empty login body is a 400 for everyone.
        {"POST",   "/api/login",                               "{}",                                                                                                   {400, 400, 400, 400, 400, 400, 400}},
        {"GET",    "/api/session",                             "",                                                                                                     {401, 200, 200, 200, 200, 200, 200}},
        // ── Groups ────────────────────────────────────────────────────────────
        {"GET",    "/api/groups",                              "",                                                                                                     ANYONE                             },
        {"POST",   "/api/groups",                              R"({"name":"New group"})",                                                                              {401, 200, 403, 403, 403, 403, 403}},
        // ── Users ─────────────────────────────────────────────────────────────
        {"GET",    "/api/users",                               "",                                                                                                     ANYONE                             },
        {"POST",
         "/api/users",                                         R"({"username":"newuser","password":"a-long-password","role":"group_viewer","group_ids":[{group_a}]})",
         {401, 200, 403, 403, 200, 403, 403}                                                                                                                                                              },
        {"PUT",    "/api/users/{victim_disable}/disable",      "",                                                                                                     ADMIN                              },
        {"PUT",    "/api/users/{victim_enable}/enable",        "",                                                                                                     ADMIN                              },
        {"DELETE", "/api/users/{victim_delete}",               "",                                                                                                     ADMIN                              },
        // Anyone may change their own password; only a global admin may change another user's.
        {"PUT",
         "/api/users/{self}/password",                         R"({"password":"another-long-password"})",
         {401, 200, 200, 200, 200, 200, 200}                                                                                                                                                              },
        {"PUT",    "/api/users/{victim_password}/password",    R"({"password":"another-long-password"})",                                                              ADMIN                              },
        // Permission before existence: only a global admin learns that an id is unknown (404).
        {"PUT",
         "/api/users/999999/password",                         R"({"password":"another-long-password"})",
         {401, 404, 403, 403, 403, 403, 403}                                                                                                                                                              },
        // ── Alerts ────────────────────────────────────────────────────────────
        {"GET",    "/api/alerts",                              "",                                                                                                     ANYONE                             },
        {"GET",    "/api/alerts/unacknowledged",               "",                                                                                                     ANYONE                             },
        {"POST",   "/api/alerts/{alert_ack_a}/ack",            "{}",                                                                                                   OPERATORS                          },
        {"POST",   "/api/alerts/{alert_ack_b}/ack",            "{}",                                                                                                   GLOBAL_OPS                         },
        {"POST",   "/api/alerts/bulk-ack",                     R"({"alert_ids":[{alert_bulk_ack}]})",                                                                  OPERATORS                          },
        {"POST",   "/api/alerts/bulk-archive",                 R"({"alert_ids":[{alert_bulk_archive}]})",                                                              OPERATORS                          },
        {"DELETE", "/api/alerts/{alert_del_a}",                "",                                                                                                     OPERATORS                          },
        {"DELETE", "/api/alerts/{alert_del_b}",                "",                                                                                                     GLOBAL_OPS                         },
        // ── Enrollment ────────────────────────────────────────────────────────
        // Pending agents belong to no group yet, so only global roles see them;
        // only global operators and admins decide on them.
        {"GET",    "/api/pending-enrollments",                 "",                                                                                                     GLOBAL_ANY                         },
        {"POST",   "/api/agents/pending-1/approve",            R"({"group_ids":[{group_a}]})",                                                                         GLOBAL_OPS                         },
        {"POST",   "/api/agents/pending-2/reject",             "",                                                                                                     GLOBAL_OPS                         },
        // A missing alert is a 404 (roles that may act), an out-of-scope one a 403.
        {"POST",   "/api/alerts/999999/ack",                   "{}",                                                                                                   {401, 404, 404, 403, 404, 404, 403}},
        {"DELETE", "/api/alerts/999999",                       "",                                                                                                     {401, 404, 404, 403, 404, 404, 403}},
        // Reject needs a pending agent: unknown is 404, approved is 409 (global operators); others are refused first.
        {"POST",   "/api/agents/ghost/reject",                 "",                                                                                                     {401, 404, 404, 403, 403, 403, 403}},
        {"POST",   "/api/agents/{agent_b}/reject",             "",                                                                                                     {401, 409, 409, 403, 403, 403, 403}},
        // ── Agents ────────────────────────────────────────────────────────────
        {"GET",    "/api/agents",                              "",                                                                                                     ANYONE                             },
        {"POST",   "/api/agents/{agent_a}/groups",             R"({"group_ids":[{group_a}]})",                                                                         GLOBAL_OPS                         },
        {"DELETE", "/api/agents/agent-del-a",                  "",                                                                                                     {401, 200, 403, 403, 200, 403, 403}},
        {"DELETE", "/api/agents/agent-del-b",                  "",                                                                                                     {401, 200, 403, 403, 403, 403, 403}},
        {"DELETE", "/api/agents/ghost-agent",                  "",                                                                                                     {401, 404, 403, 403, 403, 403, 403}},
        {"GET",    "/api/metrics",                             "",                                                                                                     ANYONE                             },
        {"GET",    "/api/metrics/{agent_a}",                   "",                                                                                                     ANYONE                             },
        {"GET",    "/api/metrics/{agent_b}",                   "",                                                                                                     GLOBAL_READ                        },
        {"POST",   "/api/agents/{agent_a}/command",            R"({"command_type":6})",                                                                                OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/command",            R"({"command_type":6})",                                                                                GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/set_interval",       R"({"interval_seconds":30})",                                                                           OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/set_interval",       R"({"interval_seconds":30})",                                                                           GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/set_process_limit",  R"({"limit":10})",                                                                                      OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/set_process_limit",  R"({"limit":10})",                                                                                      GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/thresholds",         "THRESHOLDS",                                                                                           OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/thresholds",         "THRESHOLDS",                                                                                           GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/collector_config",   "COLLECTOR",                                                                                            OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/collector_config",   "COLLECTOR",                                                                                            GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/maintenance",        R"({"reason":"patching"})",                                                                             OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/maintenance",        R"({"reason":"patching"})",                                                                             GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/pause",              "",                                                                                                     OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/pause",              "",                                                                                                     GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/resume",             "",                                                                                                     OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/resume",             "",                                                                                                     GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/restart_collectors", "",                                                                                                     OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/restart_collectors", "",                                                                                                     GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/disconnect",         "",                                                                                                     OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/disconnect",         "",                                                                                                     GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/get_status",         "",                                                                                                     OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/get_status",         "",                                                                                                     GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/description",        R"({"description":"web"})",                                                                             OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/description",        R"({"description":"web"})",                                                                             GLOBAL_OPS                         },
        {"POST",   "/api/agents/{agent_a}/runbook",            R"({"markdown":"restart it"})",                                                                         OPERATORS                          },
        {"POST",   "/api/agents/{agent_b}/runbook",            R"({"markdown":"restart it"})",                                                                         GLOBAL_OPS                         },
        {"GET",    "/api/agents/{agent_a}/log-matches",        "",                                                                                                     ANYONE                             },
        {"GET",    "/api/agents/{agent_b}/log-matches",        "",                                                                                                     GLOBAL_READ                        },
        {"GET",    "/api/agents/{agent_a}/history",            "",                                                                                                     ANYONE                             },
        {"GET",    "/api/agents/{agent_b}/history",            "",                                                                                                     GLOBAL_READ                        },
        {"GET",    "/api/uptime/{agent_a}",                    "",                                                                                                     ANYONE                             },
        {"GET",    "/api/uptime/{agent_b}",                    "",                                                                                                     GLOBAL_READ                        },
        // ── Maintenance windows ───────────────────────────────────────────────
        // Group users see all-agents ("*") windows read-only but only manage their agents'.
        {"GET",    "/api/maintenance-windows",                 "",                                                                                                     ANYONE                             },
        {"POST",   "/api/maintenance-windows",                 R"({"agent_id":"agent-a","start_ms":1,"end_ms":2})",                                                    OPERATORS                          },
        {"POST",   "/api/maintenance-windows",                 R"({"agent_id":"agent-b","start_ms":1,"end_ms":2})",                                                    GLOBAL_OPS                         },
        {"POST",   "/api/maintenance-windows",                 R"({"agent_id":"*","start_ms":1,"end_ms":2})",                                                          GLOBAL_OPS                         },
        {"DELETE", "/api/maintenance-windows/{mw_del_a}",      "",                                                                                                     OPERATORS                          },
        {"DELETE", "/api/maintenance-windows/{mw_del_b}",      "",                                                                                                     GLOBAL_OPS                         },
        {"DELETE", "/api/maintenance-windows/{mw_del_all}",    "",                                                                                                     GLOBAL_OPS                         },
        // ── Silences ──────────────────────────────────────────────────────────
        {"GET",    "/api/silences",                            "",                                                                                                     ANYONE                             },
        {"POST",   "/api/silences",                            R"({"agent_id":"agent-a","until_ms":4102444800000})",                                                   OPERATORS                          },
        {"POST",   "/api/silences",                            R"({"agent_id":"agent-b","until_ms":4102444800000})",                                                   GLOBAL_OPS                         },
        {"POST",   "/api/silences",                            R"({"agent_id":"*","until_ms":4102444800000})",                                                         GLOBAL_OPS                         },
        {"DELETE", "/api/silences/{silence_del_a}",            "",                                                                                                     OPERATORS                          },
        {"DELETE", "/api/silences/{silence_del_b}",            "",                                                                                                     GLOBAL_OPS                         },
        {"DELETE", "/api/silences/{silence_del_all}",          "",                                                                                                     GLOBAL_OPS                         },
        // ── Settings and reports ──────────────────────────────────────────────
        {"GET",    "/api/settings",                            "",                                                                                                     GLOBAL_OPS                         },
        {"PUT",    "/api/settings",                            R"({"offline_after_seconds":120})",                                                                     GLOBAL_OPS                         },
        // Webhook URLs belong to admins: operators may save other keys but not these.
        {"PUT",
         "/api/settings",                                      R"({"webhook_url":"https://other.example/hook"})",
         {401, 200, 403, 403, 403, 403, 403}                                                                                                                                                              },
        {"PUT",    "/api/settings",                            R"({"reports_webhook_url":""})",                                                                        {401, 200, 403, 403, 403, 403, 403}},
        {"POST",   "/api/reports/send",                        "",                                                                                                     GLOBAL_OPS                         },
        // ── Runbooks ──────────────────────────────────────────────────────────
        {"GET",    "/api/runbooks",                            "",                                                                                                     ANYONE                             },
        {"POST",
         "/api/runbooks",                                      R"({"status":"red","url":"https://runbooks.example/two"})",
         {401, 201, 403, 403, 403, 403, 403}                                                                                                                                                              },
        {"DELETE", "/api/runbooks/{runbook_del}",              "",                                                                                                     ADMIN                              },
        // ── Views ─────────────────────────────────────────────────────────────
        // Group users see public and own ungrouped views plus their groups' views;
        // they manage views of their own groups (any author) and views they own.
        {"GET",    "/api/views",                               "",                                                                                                     ANYONE                             },
        {"POST",   "/api/views",                               R"({"name":"new","group_id":{group_a}})",                                                               CREATE_OPERATORS                   },
        {"POST",   "/api/views",                               R"({"name":"new","group_id":0})",                                                                       {401, 201, 201, 403, 400, 400, 403}},
        {"GET",    "/api/views/{view_a}",                      "",                                                                                                     ANYONE                             },
        {"GET",    "/api/views/{view_b}",                      "",                                                                                                     GLOBAL_READ                        },
        {"GET",    "/api/views/{view_public}",                 "",                                                                                                     ANYONE                             },
        {"GET",    "/api/views/{view_private}",                "",                                                                                                     GLOBAL_READ                        },
        {"PUT",    "/api/views/{view_put_a}",                  R"({"name":"renamed"})",                                                                                OPERATORS                          },
        {"PUT",    "/api/views/{view_put_b}",                  R"({"name":"renamed"})",                                                                                GLOBAL_OPS                         },
        {"PUT",    "/api/views/{view_put_public}",             R"({"name":"renamed"})",                                                                                GLOBAL_OPS                         },
        {"DELETE", "/api/views/{view_del_a}",                  "",                                                                                                     OPERATORS                          },
        {"DELETE", "/api/views/{view_del_b}",                  "",                                                                                                     GLOBAL_OPS                         },
        {"DELETE", "/api/views/{view_del_public}",             "",                                                                                                     GLOBAL_OPS                         },
        // ── Logout ────────────────────────────────────────────────────────────
        // Last: it ends the session the rows above use.
        {"POST",   "/api/logout",                              "",                                                                                                     {200, 200, 200, 200, 200, 200, 200}},
    };
    return table;
}

} // namespace

SCENARIO("Every API route enforces the documented role policy (issue #71)")
{
    const auto role = GENERATE(from_range(role_cases()));

    GIVEN(std::string("a server with two groups, agents in each, and a session for: ") + role.label)
    {
        ApiFixture fx;
        const auto group_a = fx.add_group("Group A");
        const auto group_b = fx.add_group("Group B");
        const std::string subject = role.stored_role[0] == '\0' ? "" : "subject";
        int64_t self_id = 0;
        if (!subject.empty())
            self_id = fx.add_user(subject, role.stored_role,
                                  role.group_scoped ? std::vector<int64_t>{group_a} : std::vector<int64_t>{});
        const auto world = build_world(fx, group_a, group_b, self_id);

        WHEN("each route is called with a valid request")
        {
            THEN("the status matches the policy table")
            {
                for (const auto& row : rows())
                {
                    const auto path = world.expand(row.path);
                    const std::string placeholder = row.body;
                    const std::string raw_body = placeholder == "COLLECTOR"    ? collector_config_body
                                                 : placeholder == "THRESHOLDS" ? thresholds_body
                                                                               : placeholder;
                    const auto body_text = world.expand(raw_body);
                    const auto body = body_text.empty() ? json(nullptr) : json::parse(body_text);

                    const auto reply = fx.request(row.method, path, subject, body);

                    INFO(std::string(role.label) + ": " + row.method + " " + path + " " + body_text);
                    CHECK(reply.status == row.expect[role.column]);
                }
            }
        }
    }
}
