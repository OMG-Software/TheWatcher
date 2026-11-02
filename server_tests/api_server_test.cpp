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
