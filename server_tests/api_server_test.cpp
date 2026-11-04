// ApiServer startup and agent-approval tests.
//
// - start() must report a failed bind, so Server::run() can fail startup
//   instead of announcing a REST API that is not listening.
// - Approval must not bind a CURVE key that already belongs to another
//   approved agent: ZAP maps each key to one agent_id, so a duplicate would
//   rebind the key and cut the original agent off.
#include "../server/api.hpp"
#include "../server/store.hpp"
#include "../server/zap_handler.hpp"
#include "common/crypto.hpp"
#include "test_support/net_helpers.hpp"
#include "test_support/server_fixture.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <memory>
#include <stdexcept>
#include <string>

#include <httplib.h>
#include <zmq.hpp>

using thewatcher::server::AgentRecord;
using thewatcher::server::ApiServer;
using thewatcher::test::free_http_port;
using thewatcher::test::login_as_default_admin;

namespace
{

// An ApiServer on an in-memory store, started on a free port.
struct ApiFixture
{
    ApiFixture()
        : store(thewatcher::server::make_store("sqlite", ":memory:"))
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
    std::unique_ptr<thewatcher::server::Store> store;
    thewatcher::server::ZapHandler zap{ctx};
    ApiServer api;
    httplib::Client client;
};

AgentRecord agent(const std::string& agent_id, const std::string& key, bool approved)
{
    AgentRecord rec;
    rec.agent_id = agent_id;
    rec.hostname = agent_id + "-host";
    rec.platform = "linux";
    rec.curve_public_key_z85 = key;
    rec.approved = approved;
    return rec;
}

} // namespace

SCENARIO("ApiServer::start reports an address that cannot be bound")
{
    GIVEN("an API server configured for an address this host does not own")
    {
        // 192.0.2.1 is TEST-NET-1 (RFC 5737): never a local address, so bind()
        // fails with EADDRNOTAVAIL. (A busy port is not a reliable failure:
        // cpp-httplib sets SO_REUSEPORT on Linux.)
        zmq::context_t ctx{1};
        auto store = thewatcher::server::make_store("sqlite", ":memory:");
        thewatcher::server::ZapHandler zap{ctx};
        ApiServer api(*store, zap, "192.0.2.1", free_http_port());

        WHEN("it is started")
        {
            THEN("start() throws instead of returning as if it were listening")
            {
                REQUIRE_THROWS_AS(api.start(), std::runtime_error);
            }
        }
    }
}

SCENARIO("Approval refuses a CURVE key that another approved agent holds")
{
    GIVEN("an approved agent and a pending agent stored with the same key")
    {
        thewatcher::crypto::init();
        const auto shared = thewatcher::crypto::generate_curve_keypair().public_key_z85;
        const auto unique = thewatcher::crypto::generate_curve_keypair().public_key_z85;

        ApiFixture fx;
        fx.store->upsert_agent(agent("original-agent", shared, true));
        fx.zap.add_key(shared, "original-agent");
        fx.store->upsert_agent(agent("duplicate-agent", shared, false));
        fx.store->upsert_agent(agent("unique-agent", unique, false));
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("an admin approves the agent with the duplicated key")
        {
            auto res = fx.client.Post("/api/agents/duplicate-agent/approve", "", "application/json");

            THEN("the request is refused with 409 and nothing is rebound")
            {
                REQUIRE(res);
                REQUIRE(res->status == 409);
                auto dup = fx.store->get_agent("duplicate-agent");
                REQUIRE(dup);
                REQUIRE_FALSE(dup->approved);
                REQUIRE(fx.zap.user_id_for_key(shared) == std::optional<std::string>("original-agent"));
            }
        }

        WHEN("an admin approves an agent whose key is not in use")
        {
            auto res = fx.client.Post("/api/agents/unique-agent/approve", "", "application/json");

            THEN("the approval succeeds and the key is bound to that agent")
            {
                REQUIRE(res);
                REQUIRE(res->status == 200);
                REQUIRE(fx.zap.user_id_for_key(unique) == std::optional<std::string>("unique-agent"));
            }
        }
    }
}

SCENARIO("Alert notes are cut on a UTF-8 boundary and lists survive invalid stored text (issue #147)")
{
    GIVEN("an approved agent with one open alert")
    {
        ApiFixture fx;
        fx.store->upsert_agent(agent("utf8-agent", "", true));
        thewatcher::server::AlertRecord alert;
        alert.agent_id = "utf8-agent";
        alert.indicator = "cpu";
        alert.old_status = "green";
        alert.new_status = "red";
        alert.message = "cpu high";
        alert.created_at = 1;
        const auto alert_id = fx.store->insert_alert(alert);
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("the alert is acknowledged with a 4,097-byte note whose last character is two bytes")
        {
            const std::string note = std::string(4095, 'a') + "\xC3\xA9"; // ... + "é"
            const auto body = nlohmann::json{
                {"note", note}
            }.dump();
            auto ack = fx.client.Post("/api/alerts/" + std::to_string(alert_id) + "/ack", body, "application/json");
            REQUIRE(ack);
            REQUIRE(ack->status == 200);

            THEN("the stored note drops the whole character and GET /api/alerts still works")
            {
                const auto alerts = fx.store->list_alerts(false);
                REQUIRE(alerts.size() == 1);
                REQUIRE(alerts[0].note == std::string(4095, 'a'));
                auto list = fx.client.Get("/api/alerts");
                REQUIRE(list);
                REQUIRE(list->status == 200);
            }
        }

        WHEN("a note with invalid UTF-8 is already in the database")
        {
            fx.store->acknowledge_alert(alert_id, "thewatcher", 2, std::string("bad \xC3", 5));
            auto list = fx.client.Get("/api/alerts");

            THEN("GET /api/alerts returns 200 with the bad byte replaced")
            {
                REQUIRE(list);
                REQUIRE(list->status == 200);
                const auto alerts = nlohmann::json::parse(list->body);
                REQUIRE(alerts.size() == 1);
                REQUIRE(alerts[0]["note"].get<std::string>() == "bad \xEF\xBF\xBD");
            }
        }
    }
}

SCENARIO("Oversize settings values are rejected, not truncated (issue #147)")
{
    GIVEN("a logged-in global admin")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("reports_webhook_url is longer than 2048 bytes")
        {
            const auto url = "http://hooks.example.com/" + std::string(2048, 'a');
            const auto body = nlohmann::json{
                {"reports_webhook_url",
                 url}
            }.dump();
            auto res = fx.client.Put("/api/settings", body, "application/json");

            THEN("the request fails with 400 and nothing is stored")
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
                REQUIRE(fx.store->get_setting("reports.webhook_url", "").empty());
            }
        }
    }
}

SCENARIO("Only real loopback addresses get the loopback origin exception (issue #164)")
{
    GIVEN("a logged-in admin talking to the API on 127.0.0.1")
    {
        ApiFixture fx;
        REQUIRE(login_as_default_admin(fx.client));

        WHEN("an unsafe request carries an Origin whose DNS name starts with \"127.\"")
        {
            httplib::Headers headers{
                {"Origin", "http://127.attacker.example"}
            };
            auto res = fx.client.Post("/api/groups", headers, R"({"name":"Spoofed"})", "application/json");

            THEN("the request is refused")
            {
                REQUIRE(res);
                REQUIRE(res->status == 403);
            }
        }

        WHEN("the same request comes from a loopback dashboard on another port")
        {
            httplib::Headers headers{
                {"Origin", "http://127.0.0.1:5173"}
            };
            auto res = fx.client.Post("/api/groups", headers, R"({"name":"Dev Dashboard"})", "application/json");

            THEN("it is allowed")
            {
                REQUIRE(res);
                REQUIRE(res->status == 200);
            }
        }
    }
}
