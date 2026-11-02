// Collector-config API security tests (SEC-10).
//
// A log-monitor path makes the agent read a local file and surface its
// contents on the dashboard, so the server both validates entries strictly
// and reserves log paths for global admins.
#include "test_support/server_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>

#include <httplib.h>

using namespace thewatcher::test;
using json = nlohmann::json;

namespace
{

json collector_config_body(json logs, int cpu_readings = 1)
{
    return json{
        {"collection_interval", 30                                                             },
        {"process_limit",       5                                                              },
        {"collector_config",    json{{"logs", std::move(logs)}, {"cpu_readings", cpu_readings}}}
    };
}

json valid_log_entry()
{
    return json{
        {"path",           "/var/log/app.log"},
        {"pattern",        "ERROR"           },
        {"indicator_name", "log:app"         },
        {"severity",       "red"             }
    };
}

// POSTs a collector config carrying `logs` to the agent's collector_config endpoint.
httplib::Result post_collector_config(httplib::Client& client, const std::string& agent_id, json logs,
                                      int cpu_readings = 1)
{
    return client.Post("/api/agents/" + agent_id + "/collector_config",
                       collector_config_body(std::move(logs), cpu_readings).dump(), "application/json");
}

void require_status(const httplib::Result& res, int status)
{
    REQUIRE(res);
    REQUIRE(res->status == status);
}

// Creates a group operator inside the "Admins" group (the group the approve
// flow assigns agents to) and leaves the admin session logged in.
void create_admins_group_operator(ServerFixture& fx, const std::string& username)
{
    std::optional<int64_t> admins_group_id;
    {
        auto res = fx.client.Get("/api/groups");
        REQUIRE(res);
        REQUIRE(res->status == 200);
        for (const auto& g : json::parse(res->body))
            if (g.value("name", "") == "Admins")
                admins_group_id = g.value("group_id", int64_t{0});
    }
    REQUIRE(admins_group_id.has_value());

    auto created = fx.client.Post("/api/users",
                                  json{
                                      {"username",  username                       },
                                      {"password",  "operatorpw!"                  },
                                      {"role",      "group_operator"               },
                                      {"group_ids", json::array({*admins_group_id})}
    }
                                      .dump(),
                                  "application/json");
    REQUIRE(created);
    REQUIRE(created->status == 200);
}

} // namespace

SCENARIO("Collector-config log entries are validated and admin-restricted")
{
    GIVEN("a running server with an approved agent and a global admin session")
    {
        ServerFixture fx;
        fx.start_and_login();
        REQUIRE(fx.enroll_and_approve("cfg-sec-agent", "collector-test-host"));

        WHEN("the admin submits a valid log monitor entry")
        {
            auto res = post_collector_config(fx.client, "cfg-sec-agent", json::array({valid_log_entry()}));

            THEN("it is accepted")
            {
                require_status(res, 200);
            }
        }

        WHEN("the admin submits an invalid log severity")
        {
            auto entry = valid_log_entry();
            entry["severity"] = "purple";
            auto res = post_collector_config(fx.client, "cfg-sec-agent", json::array({entry}));

            THEN("it is rejected with 400")
            {
                require_status(res, 400);
            }
        }

        WHEN("the admin submits an oversized log pattern")
        {
            auto entry = valid_log_entry();
            entry["pattern"] = std::string(300, 'a');
            auto res = post_collector_config(fx.client, "cfg-sec-agent", json::array({entry}));

            THEN("it is rejected with 400")
            {
                require_status(res, 400);
            }
        }
    }
}

SCENARIO("Non-admin operators cannot configure log monitor paths")
{
    GIVEN("a running server with an approved agent, an admin, and a group operator in the agent's group")
    {
        ServerFixture fx;
        fx.start_and_login();
        REQUIRE(fx.enroll_and_approve("cfg-sec-agent-2", "collector-test-host"));
        create_admins_group_operator(fx, "log_op");

        // Switch the HTTP session to the group operator.
        REQUIRE(fx.login("log_op", "operatorpw!"));

        WHEN("the operator submits a collector config that includes a log path")
        {
            auto res = post_collector_config(fx.client, "cfg-sec-agent-2", json::array({valid_log_entry()}));

            THEN("the server rejects it with 403")
            {
                require_status(res, 403);
            }
        }

        WHEN("the operator submits the same config without log entries")
        {
            auto res = post_collector_config(fx.client, "cfg-sec-agent-2", json::array());

            THEN("the operator's other collector settings are still accepted")
            {
                require_status(res, 200);
            }
        }
    }
}

SCENARIO("Operators keep admin-set log monitors intact")
{
    GIVEN("an agent whose admin-set collector config already has a log monitor, and a group operator")
    {
        ServerFixture fx;
        fx.start_and_login();
        REQUIRE(fx.enroll_and_approve("cfg-sec-agent-3", "collector-test-host"));
        require_status(post_collector_config(fx.client, "cfg-sec-agent-3", json::array({valid_log_entry()})), 200);
        create_admins_group_operator(fx, "log_op3");

        WHEN("the global admin changes the log monitors")
        {
            auto entry = valid_log_entry();
            entry["pattern"] = "FATAL";
            auto res = post_collector_config(fx.client, "cfg-sec-agent-3", json::array({entry}));

            THEN("it is accepted")
            {
                require_status(res, 200);
            }
        }

        WHEN("the operator resubmits the unchanged log monitors with another setting edited")
        {
            REQUIRE(fx.login("log_op3", "operatorpw!"));
            auto res = post_collector_config(fx.client, "cfg-sec-agent-3", json::array({valid_log_entry()}), 2);

            THEN("it is accepted")
            {
                require_status(res, 200);
            }
        }

        WHEN("the operator sends an empty log list to wipe the monitors")
        {
            REQUIRE(fx.login("log_op3", "operatorpw!"));
            auto res = post_collector_config(fx.client, "cfg-sec-agent-3", json::array());

            THEN("it is rejected with 403")
            {
                require_status(res, 403);
            }
        }

        WHEN("the operator changes a log monitor")
        {
            REQUIRE(fx.login("log_op3", "operatorpw!"));
            auto entry = valid_log_entry();
            entry["path"] = "/var/log/other.log";
            auto res = post_collector_config(fx.client, "cfg-sec-agent-3", json::array({entry}));

            THEN("it is rejected with 403")
            {
                require_status(res, 403);
            }
        }
    }
}
