// REST API input validation (#67, #69), alert endpoint behaviour (#68) and the
// agent-delete cascade seen through the API (#146).
//
// - ids in paths and bodies are parsed strictly: malformed -> 400, unknown -> 404
// - every error is {"error": "<message>"}
// - the same limits apply on every endpoint that accepts the same kind of value
#include "../server/api.hpp"
#include "../server/api_validation.hpp"
#include "../server/store.hpp"
#include "../server/zap_handler.hpp"
#include "common/protocol.hpp"
#include "test_support/net_helpers.hpp"
#include "test_support/server_fixture.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <httplib.h>
#include <sqlite3.h>
#include <zmq.hpp>

using json = nlohmann::json;
using namespace thewatcher::server;
using thewatcher::test::free_http_port;
using thewatcher::test::login_as;
using thewatcher::test::login_as_default_admin;

namespace
{

// An ApiServer on an in-memory store, started on a free port.
struct ApiFixture
{
    explicit ApiFixture(const std::string& db = ":memory:")
        : store(make_store("sqlite", db))
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

    // Declared first so it is initialised before `api` uses it.
    int port = free_http_port();
    zmq::context_t ctx{1};
    std::unique_ptr<Store> store;
    ZapHandler zap{ctx};
    ApiServer api;
    httplib::Client client;

    httplib::Result post(const std::string& path, const json& body)
    {
        return client.Post(path, body.dump(), "application/json");
    }
    httplib::Result put(const std::string& path, const json& body)
    {
        return client.Put(path, body.dump(), "application/json");
    }
};

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

AgentRecord approved_agent(const std::string& agent_id)
{
    AgentRecord rec;
    rec.agent_id = agent_id;
    rec.hostname = agent_id + "-host";
    rec.platform = "linux";
    rec.curve_public_key_z85 = agent_id + "-key";
    rec.approved = true;
    // Command routes refuse an agent that is not connected (409, #54) before they
    // look at the body; these tests are about the body, so the agent is online.
    rec.connected = true;
    return rec;
}

int64_t add_alert(Store& store, const std::string& agent_id, int64_t created_at)
{
    AlertRecord alert;
    alert.agent_id = agent_id;
    alert.indicator = "cpu";
    alert.old_status = "green";
    alert.new_status = "red";
    alert.message = "cpu red";
    alert.created_at = created_at;
    return store.insert_alert(alert);
}

// True when the response is JSON of the shape {"error": "<non-empty text>"}.
bool is_error_body(const httplib::Result& res)
{
    if (!res)
        return false;
    const auto body = json::parse(res->body, nullptr, false);
    return body.is_object() && body.size() == 1 && body.contains("error") && body["error"].is_string() &&
           !body["error"].get<std::string>().empty() &&
           res->get_header_value("Content-Type").find("application/json") != std::string::npos;
}

} // namespace

// ── Pure helpers ──────────────────────────────────────────────────────────────

SCENARIO("parse_id accepts only plain positive decimal ids")
{
    using thewatcher::server::validation::parse_id;

    GIVEN("well formed ids")
    {
        THEN("they parse")
        {
            REQUIRE(parse_id("1") == 1);
            REQUIRE(parse_id("42") == 42);
            REQUIRE(parse_id("9223372036854775807") == INT64_MAX);
        }
    }

    GIVEN("malformed ids")
    {
        THEN("none of them parses")
        {
            for (const char* bad : {"", "abc", "12abc", "-1", "+1", "0", " 1", "1 ", "0x10", "1.5",
                                    "99999999999999999999", "9223372036854775808"})
            {
                INFO("input: '" << bad << "'");
                REQUIRE_FALSE(parse_id(bad).has_value());
            }
        }
    }
}

SCENARIO("parse_int enforces its range")
{
    using thewatcher::server::validation::parse_int;

    GIVEN("a 1-1000 range")
    {
        THEN("in-range numbers parse and everything else is rejected")
        {
            REQUIRE(parse_int("1", 1, 1000) == 1);
            REQUIRE(parse_int("1000", 1, 1000) == 1000);
            REQUIRE_FALSE(parse_int("0", 1, 1000).has_value());
            REQUIRE_FALSE(parse_int("1001", 1, 1000).has_value());
            REQUIRE_FALSE(parse_int("abc", 1, 1000).has_value());
            REQUIRE_FALSE(parse_int("", 1, 1000).has_value());
            REQUIRE_FALSE(parse_int("5x", 1, 1000).has_value());
            REQUIRE_FALSE(parse_int("99999999999", 1, 1000).has_value());
        }
    }
}

// ── Malformed ids ─────────────────────────────────────────────────────────────

SCENARIO("A malformed numeric id in a path is a 400 with a JSON error, never a 500")
{
    GIVEN("an admin session")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));

        THEN("every route that takes a numeric path id answers 400")
        {
            const std::vector<httplib::Result> results = [&] {
                std::vector<httplib::Result> out;
                out.push_back(fx.client.Post("/api/alerts/abc/ack", "", "application/json"));
                out.push_back(fx.client.Delete("/api/alerts/abc"));
                out.push_back(fx.client.Delete("/api/maintenance-windows/abc"));
                out.push_back(fx.client.Delete("/api/silences/abc"));
                out.push_back(fx.client.Delete("/api/runbooks/abc"));
                out.push_back(fx.client.Get("/api/views/abc"));
                out.push_back(fx.put("/api/views/abc", json::object()));
                out.push_back(fx.client.Delete("/api/views/abc"));
                out.push_back(fx.put("/api/users/abc/disable", json::object()));
                out.push_back(fx.put("/api/users/abc/enable", json::object()));
                out.push_back(fx.client.Delete("/api/users/abc"));
                out.push_back(fx.put("/api/users/abc/password", json{
                                                                    {"password", "x"}
                }));
                return out;
            }();
            for (const auto& res : results)
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
                REQUIRE(is_error_body(res));
            }
        }
    }
}

SCENARIO("A malformed numeric query value is a 400, not an exception")
{
    GIVEN("an admin session and an agent")
    {
        ApiFixture fx;
        fx.store->upsert_agent(approved_agent("a1"));
        REQUIRE(login_as_default_admin(fx.client));

        THEN("limit, days, offset and page values are validated")
        {
            for (const char* url :
                 {"/api/metrics/a1?limit=abc", "/api/metrics/a1?limit=0", "/api/agents/a1/history?limit=x",
                  "/api/agents/a1/log-matches?limit=abc", "/api/uptime/a1?days=abc", "/api/uptime/a1?days=0",
                  "/api/alerts?limit=abc", "/api/alerts?limit=0", "/api/alerts?offset=-1",
                  "/api/alerts/unacknowledged?limit=abc"})
            {
                INFO("url: " << url);
                auto res = fx.client.Get(url);
                REQUIRE(res);
                REQUIRE(res->status == 400);
                REQUIRE(is_error_body(res));
            }
        }
    }
}

// ── Unknown ids ───────────────────────────────────────────────────────────────

SCENARIO("An unknown id is a 404 with a JSON error")
{
    GIVEN("an admin session and no agents, alerts or windows")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));

        THEN("agent routes answer 404 for an unknown agent id")
        {
            std::vector<httplib::Result> results;
            for (const char* action : {"pause", "resume", "restart_collectors", "disconnect", "get_status"})
                results.push_back(fx.post(std::string("/api/agents/ghost/") + action, json::object()));
            results.push_back(fx.post("/api/agents/ghost/command", json{
                                                                       {"command_type", 4}
            }));
            results.push_back(fx.post("/api/agents/ghost/set_interval", json{
                                                                            {"interval_seconds", 30}
            }));
            results.push_back(fx.post("/api/agents/ghost/set_process_limit", json{
                                                                                 {"limit", 10}
            }));
            results.push_back(fx.post("/api/agents/ghost/maintenance", json::object()));
            results.push_back(fx.post("/api/agents/ghost/description", json{
                                                                           {"description", "x"}
            }));
            results.push_back(fx.post("/api/agents/ghost/runbook", json{
                                                                       {"markdown", "x"}
            }));
            results.push_back(fx.post("/api/agents/ghost/groups", json{
                                                                      {"group_ids", json::array()}
            }));
            results.push_back(fx.post("/api/agents/ghost/approve", json::object()));
            results.push_back(fx.post("/api/agents/ghost/reject", json::object()));
            results.push_back(fx.client.Delete("/api/agents/ghost"));
            results.push_back(fx.client.Get("/api/agents/ghost/history"));
            results.push_back(fx.client.Get("/api/agents/ghost/log-matches"));
            results.push_back(fx.client.Get("/api/metrics/ghost"));
            results.push_back(fx.client.Get("/api/uptime/ghost"));
            for (const auto& res : results)
            {
                REQUIRE(res);
                REQUIRE(res->status == 404);
                REQUIRE(is_error_body(res));
            }
        }

        THEN("numeric-id routes answer 404 for an unknown id")
        {
            std::vector<httplib::Result> results;
            results.push_back(fx.post("/api/alerts/12345/ack", json::object()));
            results.push_back(fx.client.Delete("/api/alerts/12345"));
            results.push_back(fx.client.Delete("/api/maintenance-windows/12345"));
            results.push_back(fx.client.Delete("/api/silences/12345"));
            results.push_back(fx.client.Delete("/api/runbooks/12345"));
            results.push_back(fx.client.Get("/api/views/12345"));
            results.push_back(fx.put("/api/users/12345/enable", json::object()));
            results.push_back(fx.put("/api/users/12345/password", json{
                                                                      {"password", "pw"}
            }));
            for (const auto& res : results)
            {
                REQUIRE(res);
                REQUIRE(res->status == 404);
                REQUIRE(is_error_body(res));
            }
        }

        THEN("a body that references an unknown agent or group answers 404")
        {
            const auto until = now_ms() + 3'600'000;
            auto silence = fx.post("/api/silences", json{
                                                        {"agent_id", "ghost"},
                                                        {"until_ms", until  }
            });
            auto window = fx.post("/api/maintenance-windows",
                                  json{
                                      {"agent_id", "ghost" },
                                      {"start_ms", now_ms()},
                                      {"end_ms",   until   }
            });
            auto runbook = fx.post(
                "/api/runbooks", json{
                                     {"agent_id", "ghost"                      },
                                     {"status",   "red"                        },
                                     {"url",      "https://wiki.example.com/rb"}
            });
            auto user = fx.post("/api/users", json{
                                                  {"username",  "bob"},
                                                  {"password",  "pw" },
                                                  {"group_ids", {777}}
            });
            for (const auto* res : {&silence, &window, &runbook, &user})
            {
                REQUIRE(*res);
                REQUIRE((*res)->status == 404);
                REQUIRE(is_error_body(*res));
            }
        }
    }
}

SCENARIO("A group user gets 404 for a missing alert and 403 for one in another group")
{
    GIVEN("a viewer in group A, an alert in group A and an alert in group B")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));
        const auto group_a = json::parse(fx.post("/api/groups",
                                                 json{
                                                     {"name", "A"}
        })
                                             ->body)["group_id"]
                                 .get<int64_t>();
        const auto group_b = json::parse(fx.post("/api/groups",
                                                 json{
                                                     {"name", "B"}
        })
                                             ->body)["group_id"]
                                 .get<int64_t>();
        fx.store->upsert_agent(approved_agent("agent-a"));
        fx.store->upsert_agent(approved_agent("agent-b"));
        fx.store->set_agent_groups("agent-a", {group_a});
        fx.store->set_agent_groups("agent-b", {group_b});
        const auto alert_a = add_alert(*fx.store, "agent-a", 100);
        const auto alert_b = add_alert(*fx.store, "agent-b", 200);
        REQUIRE(fx.post("/api/users",
                        json{
                            {"username",  "op"            },
                            {"password",  "pw12345!"      },
                            {"role",      "group_operator"},
                            {"group_ids", {group_a}       }
        })
                    ->status == 200);

        httplib::Client user("http://127.0.0.1:" + std::to_string(fx.port));
        REQUIRE(login_as(user, "op", "pw12345!"));

        THEN("an unknown alert is 404, another group's alert is 403 and their own is acknowledged")
        {
            REQUIRE(user.Post("/api/alerts/999999/ack", "", "application/json")->status == 404);
            REQUIRE(user.Post("/api/alerts/" + std::to_string(alert_b) + "/ack", "", "application/json")->status ==
                    403);
            REQUIRE(user.Post("/api/alerts/" + std::to_string(alert_a) + "/ack", "", "application/json")->status ==
                    200);
            REQUIRE(fx.store->get_alert(alert_a)->acknowledged_at > 0);
            REQUIRE(fx.store->get_alert(alert_b)->acknowledged_at == 0);
        }
    }
}

SCENARIO("A duplicate username or group name is a 409")
{
    GIVEN("an existing user and group")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));
        REQUIRE(fx.post("/api/users",
                        json{
                            {"username", "dup"},
                            {"password", "pw" }
        })
                    ->status == 200);
        REQUIRE(fx.post("/api/groups",
                        json{
                            {"name", "ops"}
        })
                    ->status == 200);

        THEN("creating them again conflicts")
        {
            auto user = fx.post("/api/users", json{
                                                  {"username", "dup"},
                                                  {"password", "pw" }
            });
            auto group = fx.post("/api/groups", json{
                                                    {"name", "ops"}
            });
            REQUIRE(user->status == 409);
            REQUIRE(is_error_body(user));
            REQUIRE(group->status == 409);
            REQUIRE(is_error_body(group));
        }
    }
}

// ── Shared limits (#69) ───────────────────────────────────────────────────────

SCENARIO("The same limits apply on every endpoint that accepts the same kind of value")
{
    GIVEN("an admin session and an agent")
    {
        ApiFixture fx;
        fx.store->upsert_agent(approved_agent("a1"));
        REQUIRE(login_as_default_admin(fx.client));
        const auto until = now_ms() + 3'600'000;
        const std::string too_long_reason(1025, 'r');

        THEN("oversized reasons are rejected on maintenance, windows and silences")
        {
            REQUIRE(fx.post("/api/agents/a1/maintenance",
                            json{
                                {"reason", too_long_reason}
            })
                        ->status == 400);
            REQUIRE(
                fx.post("/api/maintenance-windows",
                        json{
                            {"agent_id", "a1"           },
                            {"start_ms", now_ms()       },
                            {"end_ms",   until          },
                            {"reason",   too_long_reason}
            })
                    ->status == 400);
            REQUIRE(fx.post("/api/silences",
                            json{
                                {"agent_id", "a1"           },
                                {"until_ms", until          },
                                {"reason",   too_long_reason}
            })
                        ->status == 400);
        }

        THEN("a reason at the limit is accepted")
        {
            REQUIRE(fx.post("/api/agents/a1/maintenance",
                            json{
                                {"reason", std::string(1024, 'r')}
            })
                        ->status == 200);
        }

        THEN("timestamps must be sane")
        {
            REQUIRE(fx.post("/api/agents/a1/maintenance",
                            json{
                                {"until_ms", -5}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/maintenance-windows",
                            json{
                                {"agent_id", "a1" },
                                {"start_ms", -1   },
                                {"end_ms",   until}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/maintenance-windows",
                            json{
                                {"agent_id", "a1"                 },
                                {"start_ms", 1                    },
                                {"end_ms",   9'000'000'000'000'000}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/silences",
                            json{
                                {"agent_id", "a1"                 },
                                {"until_ms", 9'000'000'000'000'000}
            })
                        ->status == 400);
        }

        THEN("silence indicators and agent ids are bounded")
        {
            REQUIRE(fx.post("/api/silences",
                            json{
                                {"agent_id", "a1"},
                                {"indicator", std::string(65, 'i')},
                                {"until_ms", until}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/silences",
                            json{
                                {"agent_id", std::string(129, 'a')},
                                {"until_ms", until}
            })
                        ->status == 400);
        }

        THEN("collection intervals and process limits have upper bounds")
        {
            REQUIRE(fx.post("/api/agents/a1/set_interval",
                            json{
                                {"interval_seconds", 86401}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/agents/a1/set_interval",
                            json{
                                {"interval_seconds", 86400}
            })
                        ->status == 200);
            REQUIRE(fx.post("/api/agents/a1/set_process_limit",
                            json{
                                {"limit", 1001}
            })
                        ->status == 400);
        }

        THEN("views bound their name and agent list")
        {
            REQUIRE(fx.post("/api/views",
                            json{
                                {"name", std::string(65, 'v')}
            })
                        ->status == 400);
            std::vector<std::string> many(1001, "a1");
            REQUIRE(fx.post("/api/views",
                            json{
                                {"name",      "v" },
                                {"agent_ids", many}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/views",
                            json{
                                {"name", std::string(64, 'v')},
                                {"agent_ids", {"a1"}}
            })
                        ->status == 201);
        }

        THEN("runbooks bound their url and notes")
        {
            REQUIRE(fx.post("/api/runbooks",
                            json{
                                {"status", "red"},
                                {"url", "https://x/" + std::string(2048, 'u')}
            })
                        ->status == 400);
            REQUIRE(
                fx.post(
                      "/api/runbooks",
                      json{
                          {"status", "red"},
                          {"url", "https://wiki.example.com/rb"},
                          {"notes", std::string(4097, 'n')}
            })
                    ->status == 400);
        }

        THEN("a settings webhook url is bounded like the report webhook url")
        {
            REQUIRE(fx.put("/api/settings",
                           json{
                               {"webhook_url", "https://x/" + std::string(2048, 'u')}
            })
                        ->status == 400);
        }

        THEN("a collector runbook is rejected, not truncated, when over 65536 bytes")
        {
            const auto rec = fx.store->get_agent("a1");
            json body{
                {"collection_interval", 30},
                {"process_limit", 25},
                {"collector_config", json(rec->collector_config)},
                {"runbook_markdown", std::string(65537, 'm')}
            };
            REQUIRE(fx.post("/api/agents/a1/collector_config", body)->status == 400);
        }

        THEN("bulk alert requests bound the id list and reject non-integer ids")
        {
            REQUIRE(fx.post("/api/alerts/bulk-ack",
                            json{
                                {"alert_ids", std::vector<int>(1001, 1)}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/alerts/bulk-ack",
                            json{
                                {"alert_ids", {"abc"}}
            })
                        ->status == 400);
            REQUIRE(fx.post("/api/alerts/bulk-archive",
                            json{
                                {"alert_ids", {1.5}}
            })
                        ->status == 400);
        }
    }
}

// ── Alert listing (#68) ───────────────────────────────────────────────────────

SCENARIO("Alert listings are filtered and paged in the store")
{
    GIVEN("five alerts, one acknowledged")
    {
        ApiFixture fx;
        fx.store->upsert_agent(approved_agent("a1"));
        std::vector<int64_t> ids;
        for (int i = 1; i <= 5; ++i)
            ids.push_back(add_alert(*fx.store, "a1", i * 100));
        fx.store->acknowledge_alert(ids[0], "someone", 1000);
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("a page of two alerts is requested")
        {
            auto res = fx.client.Get("/api/alerts?limit=2&offset=1");

            THEN("the second and third newest alerts come back")
            {
                REQUIRE(res->status == 200);
                const auto body = json::parse(res->body);
                REQUIRE(body.size() == 2);
                REQUIRE(body[0]["alert_id"] == ids[3]);
                REQUIRE(body[1]["alert_id"] == ids[2]);
            }
        }

        WHEN("unacknowledged alerts are listed")
        {
            auto res = fx.client.Get("/api/alerts/unacknowledged");

            THEN("the acknowledged one is left out")
            {
                REQUIRE(res->status == 200);
                REQUIRE(json::parse(res->body).size() == 4);
            }
        }

        WHEN("a bulk acknowledge names a real, an unknown and an archived alert")
        {
            fx.store->soft_delete_alert(ids[4], 2000);
            auto res = fx.post("/api/alerts/bulk-ack", json{
                                                           {"alert_ids", {ids[1], 99999, ids[4]}}
            });

            THEN("only the real one is acknowledged and counted")
            {
                REQUIRE(res->status == 200);
                REQUIRE(json::parse(res->body)["count"] == 1);
                REQUIRE(fx.store->get_alert(ids[1])->acknowledged_at > 0);
            }
        }
    }
}

SCENARIO("One undecodable metrics row does not break /api/metrics")
{
    GIVEN("two agents, one of which has a corrupt latest metrics row")
    {
        ApiFixture fx;
        fx.store->upsert_agent(approved_agent("good"));
        fx.store->upsert_agent(approved_agent("bad"));

        thewatcher::SystemMetrics metrics;
        metrics.hostname = "good-host";
        MetricsRow good;
        good.agent_id = "good";
        good.timestamp_ms = now_ms();
        good.metrics_cbor = thewatcher::proto::pack(metrics);
        fx.store->insert_metrics(good);

        MetricsRow bad;
        bad.agent_id = "bad";
        bad.timestamp_ms = now_ms();
        bad.metrics_cbor = {0xff, 0x00, 0x13, 0x37};
        fx.store->insert_metrics(bad);
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("all latest metrics are requested")
        {
            auto res = fx.client.Get("/api/metrics");

            THEN("the response is 200 and carries the readable agent only")
            {
                REQUIRE(res->status == 200);
                const auto body = json::parse(res->body);
                REQUIRE(body.size() == 1);
                REQUIRE(body[0]["agent_id"] == "good");
            }
        }

        WHEN("the history of the corrupt agent is requested")
        {
            auto res = fx.client.Get("/api/metrics/bad");

            THEN("the response is 200 with the unreadable row skipped")
            {
                REQUIRE(res->status == 200);
                REQUIRE(json::parse(res->body).empty());
            }
        }
    }
}

SCENARIO("A corrupt stored setting does not turn GET /api/settings into a 500")
{
    GIVEN("a stored numeric setting that is not a number")
    {
        ApiFixture fx;
        fx.store->set_setting("offline_after_seconds", "garbage");
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("the settings are read")
        {
            auto res = fx.client.Get("/api/settings");

            THEN("the default is reported")
            {
                REQUIRE(res->status == 200);
                REQUIRE(json::parse(res->body)["offline_after_seconds"] == 120);
            }
        }
    }
}

// ── Agent delete cascade (#146) ───────────────────────────────────────────────

SCENARIO("Deleting an agent through the API leaves nothing for a re-enrolled agent to inherit")
{
    GIVEN("an agent with a silence, a maintenance window and an agent runbook")
    {
        ApiFixture fx;
        fx.store->upsert_agent(approved_agent("again"));
        REQUIRE(login_as_default_admin(fx.client));
        const auto until = now_ms() + 3'600'000;
        REQUIRE(fx.post("/api/silences",
                        json{
                            {"agent_id", "again"},
                            {"until_ms", until  }
        })
                    ->status == 200);
        REQUIRE(fx.post("/api/maintenance-windows",
                        json{
                            {"agent_id", "again" },
                            {"start_ms", now_ms()},
                            {"end_ms",   until   }
        })
                    ->status == 200);
        REQUIRE(fx.post("/api/runbooks",
                        json{
                            {"agent_id",  "again"                      },
                            {"indicator", "cpu"                        },
                            {"status",    "red"                        },
                            {"url",       "https://wiki.example.com/rb"}
        })
                    ->status == 201);

        WHEN("the agent is deleted and later enrols again with the same id")
        {
            REQUIRE(fx.client.Delete("/api/agents/again")->status == 200);
            fx.store->upsert_agent(approved_agent("again"));

            THEN("it is not silenced, in maintenance or given the old runbook")
            {
                REQUIRE_FALSE(fx.store->is_silenced("again", "cpu", now_ms()));
                REQUIRE(fx.store->active_maintenance_windows(now_ms()).empty());
                REQUIRE_FALSE(fx.store->get_runbook("again", "cpu", "red").has_value());
            }
        }
    }
}

// ── Review follow-ups ─────────────────────────────────────────────────────────

SCENARIO("Views may only reference agents and groups that exist")
{
    GIVEN("an admin, one real agent and one real group")
    {
        ApiFixture fx;
        fx.store->upsert_agent(approved_agent("a1"));
        REQUIRE(login_as_default_admin(fx.client));
        const auto group = json::parse(fx.post("/api/groups",
                                               json{
                                                   {"name", "G"}
        })
                                           ->body)["group_id"]
                               .get<int64_t>();
        const auto created = fx.post("/api/views", json{
                                                       {"name",      "ok"  },
                                                       {"agent_ids", {"a1"}},
                                                       {"group_id",  group }
        });
        REQUIRE(created->status == 201);
        const auto view_id = json::parse(created->body)["view_id"].get<int64_t>();
        const auto path = "/api/views/" + std::to_string(view_id);

        THEN("creating a view with an unknown agent or group is 404")
        {
            REQUIRE(fx.post("/api/views",
                            json{
                                {"name",      "v"      },
                                {"agent_ids", {"ghost"}}
            })
                        ->status == 404);
            REQUIRE(fx.post("/api/views",
                            json{
                                {"name",     "v"  },
                                {"group_id", 98765}
            })
                        ->status == 404);
        }

        THEN("updating a view to an unknown agent or group is 404 and leaves it unchanged")
        {
            REQUIRE(fx.put(path,
                           json{
                               {"agent_ids", {"ghost"}}
            })
                        ->status == 404);
            REQUIRE(fx.put(path,
                           json{
                               {"group_id", 98765}
            })
                        ->status == 404);
            REQUIRE(fx.store->get_view(view_id)->agent_ids == std::vector<std::string>{"a1"});
        }

        THEN("agent_ids that is not an array is 400 on create and update")
        {
            REQUIRE(fx.post("/api/views",
                            json{
                                {"name",      "v" },
                                {"agent_ids", "a1"}
            })
                        ->status == 400);
            REQUIRE(fx.put(path,
                           json{
                               {"agent_ids", "a1"}
            })
                        ->status == 400);
        }

        THEN("renaming a view that holds an old stale agent id still works")
        {
            ViewRecord stale;
            stale.name = "stale";
            stale.owner_user_id = 1;
            stale.created_at = 1;
            stale.agent_ids = {"long-gone"};
            const auto stale_id = fx.store->create_view(stale);
            REQUIRE(fx.put("/api/views/" + std::to_string(stale_id),
                           json{
                               {"name", "renamed"}
            })
                        ->status == 200);
        }
    }
}

SCENARIO("Bulk acknowledge and approve reject malformed bodies")
{
    GIVEN("an admin, a pending agent and an alert")
    {
        ApiFixture fx;
        auto pending = approved_agent("p1");
        pending.approved = false;
        fx.store->upsert_agent(pending);
        fx.store->upsert_agent(approved_agent("a1"));
        const auto alert = add_alert(*fx.store, "a1", 100);
        REQUIRE(login_as_default_admin(fx.client));

        THEN("a non-string note on bulk-ack is 400 and nothing is acknowledged")
        {
            REQUIRE(fx.post("/api/alerts/bulk-ack",
                            json{
                                {"alert_ids", {alert}},
                                {"note",      123    }
            })
                        ->status == 400);
            REQUIRE(fx.store->get_alert(alert)->acknowledged_at == 0);
        }

        THEN("an approve body that is not a JSON object is 400 and the agent stays pending")
        {
            for (const char* body : {"[]", "null", "\"x\"", "7"})
            {
                INFO("body: " << body);
                REQUIRE(fx.client.Post("/api/agents/p1/approve", body, "application/json")->status == 400);
            }
            REQUIRE_FALSE(fx.store->get_agent("p1")->approved);
        }
    }
}

SCENARIO("A storage fault is a generic 500, not a 400 carrying the database error")
{
    GIVEN("an API on a file database whose runbooks table is dropped behind its back")
    {
        const auto dir = thewatcher::test::process_temp_dir().path() / "api-validation";
        std::filesystem::create_directories(dir);
        const auto db = (dir / "fault.db").string();
        std::filesystem::remove(db);
        ApiFixture fx(db);
        REQUIRE(login_as_default_admin(fx.client));
        {
            sqlite3* raw = nullptr;
            REQUIRE(sqlite3_open(db.c_str(), &raw) == SQLITE_OK);
            REQUIRE(sqlite3_exec(raw, "DROP TABLE runbooks;", nullptr, nullptr, nullptr) == SQLITE_OK);
            sqlite3_close(raw);
        }

        WHEN("a valid runbook is created")
        {
            auto res = fx.post("/api/runbooks", json{
                                                    {"status", "red"                        },
                                                    {"url",    "https://wiki.example.com/rb"}
            });

            THEN("the answer is 500 with the generic message and no SQL text")
            {
                REQUIRE(res->status == 500);
                REQUIRE(json::parse(res->body) == json{
                                                      {"error", "internal error"}
                });
            }
        }

        WHEN("an invalid runbook is created")
        {
            auto res = fx.post("/api/runbooks", json{
                                                    {"status", "purple"                     },
                                                    {"url",    "https://wiki.example.com/rb"}
            });

            THEN("it is still a 400 validation error")
            {
                REQUIRE(res->status == 400);
            }
        }
    }
}

SCENARIO("A command body is validated before the agent's connection state is checked")
{
    GIVEN("an approved agent that is offline")
    {
        ApiFixture fx;
        auto offline = approved_agent("off1");
        offline.connected = false;
        fx.store->upsert_agent(offline);
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("a command, interval or process limit request has a malformed or invalid body")
        {
            THEN("each route answers 400, not 409")
            {
                // Not JSON at all.
                REQUIRE(fx.client.Post("/api/agents/off1/command", "{not json", "application/json")->status == 400);
                REQUIRE(fx.client.Post("/api/agents/off1/set_interval", "{not json", "application/json")->status ==
                        400);
                REQUIRE(fx.client.Post("/api/agents/off1/set_process_limit", "{not json", "application/json")->status ==
                        400);
                // Valid JSON, required key missing.
                REQUIRE(fx.post("/api/agents/off1/command", json::object())->status == 400);
                REQUIRE(fx.post("/api/agents/off1/set_interval", json::object())->status == 400);
                REQUIRE(fx.post("/api/agents/off1/set_process_limit", json::object())->status == 400);
                // Out of range or unknown values.
                REQUIRE(fx.post("/api/agents/off1/command",
                                json{
                                    {"command_type", 250}
                })
                            ->status == 400);
                REQUIRE(fx.post("/api/agents/off1/set_interval",
                                json{
                                    {"interval_seconds", 86401}
                })
                            ->status == 400);
                REQUIRE(fx.post("/api/agents/off1/set_process_limit",
                                json{
                                    {"limit", 1001}
                })
                            ->status == 400);
            }
        }

        WHEN("the bodies are valid")
        {
            THEN("each route still answers 409 and nothing is queued")
            {
                // command_type 6 is GET_STATUS.
                REQUIRE(fx.post("/api/agents/off1/command",
                                json{
                                    {"command_type", 6}
                })
                            ->status == 409);
                REQUIRE(fx.post("/api/agents/off1/set_interval",
                                json{
                                    {"interval_seconds", 30}
                })
                            ->status == 409);
                REQUIRE(fx.post("/api/agents/off1/set_process_limit",
                                json{
                                    {"limit", 50}
                })
                            ->status == 409);
            }
        }
    }
}

SCENARIO("command_type must be an integer from 0 to 255 and is never wrapped")
{
    GIVEN("an online agent")
    {
        ApiFixture fx;
        fx.store->upsert_agent(approved_agent("on1"));
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("command_type is 256 plus a valid type, negative, or not an integer")
        {
            const auto status_of = [&](const json& value) {
                return fx
                    .post("/api/agents/on1/command",
                          json{
                              {"command_type", value}
                })
                    ->status;
            };

            THEN("each is 400 and nothing is queued")
            {
                REQUIRE(status_of(262) == 400);  // 256 + 6 would wrap to GET_STATUS
                REQUIRE(status_of(-255) == 400); // would wrap to 1
                REQUIRE(status_of(6.5) == 400);
                REQUIRE(status_of("6") == 400);
                REQUIRE(status_of(true) == 400);
                REQUIRE(status_of(nullptr) == 400);
                std::vector<PendingCommand> queued;
                fx.api.drain_commands(queued);
                REQUIRE(queued.empty());
            }
        }

        WHEN("command_type is a valid type")
        {
            THEN("it is queued")
            {
                REQUIRE(fx.post("/api/agents/on1/command",
                                json{
                                    {"command_type", 6}
                })
                            ->status == 200);
                std::vector<PendingCommand> queued;
                fx.api.drain_commands(queued);
                REQUIRE(queued.size() == 1);
            }
        }
    }
}
