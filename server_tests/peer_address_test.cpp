// Agent IP address tests (issue #65).
//
// - parse_peer_ip reduces ZeroMQ's "Peer-Address" property to a bare IP
//   literal and rejects anything else, so the stored value is safe text.
// - The address is stored on the agent record in both backends, survives a
//   column migration and is not blanked by a later update with no address.
// - The server records it at enrollment and on every data-plane frame, and the
//   API exposes it for the Monitoring, Agents and Pending pages.
#include "../server/peer_address.hpp"
#include "../server/store.hpp"
#include "test_support/enrollment_client.hpp"
#include "test_support/server_fixture.hpp"
#include "test_support/temp_dir.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <memory>
#include <string>

#include <sqlite3.h>
#include <zmq.hpp>

using namespace thewatcher;
using namespace thewatcher::server;
using namespace std::chrono_literals;
using json = nlohmann::json;

namespace
{

AgentRecord make_agent(const std::string& id, bool approved)
{
    AgentRecord rec;
    rec.agent_id = id;
    rec.hostname = id + "-host";
    rec.platform = "linux";
    rec.approved = approved;
    rec.first_seen = 1000;
    rec.last_seen = 1000;
    return rec;
}

// Returns the agent object with `agent_id` from a JSON list, or null.
json find_in(const json& list, const std::string& agent_id)
{
    for (const auto& item : list)
    {
        if (item.at("agent_id") == agent_id)
            return item;
    }
    return nullptr;
}

} // namespace

SCENARIO("A ZeroMQ peer address is reduced to a bare IP literal")
{
    GIVEN("peer addresses in the forms libzmq reports them")
    {
        THEN("an IPv4 address loses its port")
        {
            REQUIRE(parse_peer_ip("192.168.1.5:54321") == "192.168.1.5");
            REQUIRE(parse_peer_ip("127.0.0.1") == "127.0.0.1");
        }

        THEN("a bracketed IPv6 address loses its brackets and port")
        {
            REQUIRE(parse_peer_ip("[2001:db8::1]:5555") == "2001:db8::1");
            REQUIRE(parse_peer_ip("[::1]:9") == "::1");
        }

        THEN("a bare IPv6 address is accepted")
        {
            REQUIRE(parse_peer_ip("::1") == "::1");
            REQUIRE(parse_peer_ip("::ffff:10.0.0.7") == "::ffff:10.0.0.7");
        }

        THEN("an IPv6 zone identifier is dropped")
        {
            REQUIRE(parse_peer_ip("[fe80::1%eth0]:4000") == "fe80::1");
        }
    }

    GIVEN("values that are not addresses")
    {
        THEN("every one is rejected as an empty string")
        {
            REQUIRE(parse_peer_ip("").empty());
            REQUIRE(parse_peer_ip("unknown").empty());
            REQUIRE(parse_peer_ip("<script>alert(1)</script>:80").empty());
            REQUIRE(parse_peer_ip("999.1.1.1:80").empty());
            REQUIRE(parse_peer_ip("1.2.3.4:80x").empty());
            REQUIRE(parse_peer_ip("1.2.3.4:123456").empty());
            REQUIRE(parse_peer_ip("1.2.3.4\n:80").empty());
            REQUIRE(parse_peer_ip("1.2.3.4:5:6").empty());
            REQUIRE(parse_peer_ip("[1.2.3.4").empty());
            REQUIRE(parse_peer_ip("[::1]x").empty());
            REQUIRE(parse_peer_ip("evil.example.com:80").empty());
            REQUIRE(parse_peer_ip(std::string(200, '1')).empty());
            REQUIRE(parse_peer_ip(std::string("1.2.3.4\0:80", 11)).empty());
        }
    }
}

SCENARIO("The agent address is stored and kept")
{
    GIVEN("an in-memory store with a pending agent")
    {
        auto store = make_store("sqlite", ":memory:");
        auto rec = make_agent("ip-agent", false);
        rec.ip_address = "203.0.113.9";
        store->upsert_agent(rec);

        WHEN("the agent is read back")
        {
            THEN("the address is on the record and in the pending list")
            {
                REQUIRE(store->get_agent("ip-agent")->ip_address == "203.0.113.9");
                const auto pending = store->list_pending_agents();
                REQUIRE(pending.size() == 1);
                REQUIRE(pending[0].ip_address == "203.0.113.9");
            }
        }

        WHEN("the agent connects from a new address")
        {
            rec.ip_address = "198.51.100.2";
            store->upsert_agent(rec);

            THEN("the new address replaces the old one")
            {
                REQUIRE(store->get_agent("ip-agent")->ip_address == "198.51.100.2");
            }
        }

        WHEN("a later update carries no address")
        {
            auto blank = make_agent("ip-agent", false);
            store->upsert_agent(blank);

            THEN("the last known address is kept")
            {
                REQUIRE(store->get_agent("ip-agent")->ip_address == "203.0.113.9");
            }
        }

        WHEN("the agent is approved")
        {
            store->approve_agent("ip-agent");

            THEN("the approved list still carries the address")
            {
                const auto approved = store->list_approved_agents();
                REQUIRE(approved.size() == 1);
                REQUIRE(approved[0].ip_address == "203.0.113.9");
            }
        }
    }
}

SCENARIO("A database created before the agent address existed is migrated")
{
    GIVEN("a database file whose agents table has no ip_address column")
    {
        test::ScopedTempDir dir("thewatcher-ip-migration");
        const auto db_path = (dir.path() / "old.db").string();
        {
            auto store = make_store("sqlite", db_path);
            store->upsert_agent(make_agent("old-agent", true));
        }
        {
            sqlite3* raw = nullptr;
            REQUIRE(sqlite3_open(db_path.c_str(), &raw) == SQLITE_OK);
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
            REQUIRE(sqlite3_exec(db.get(), "ALTER TABLE agents DROP COLUMN ip_address;", nullptr, nullptr, nullptr) ==
                    SQLITE_OK);
        }

        WHEN("the store opens it")
        {
            auto store = make_store("sqlite", db_path);

            THEN("existing agents survive with an empty address and the column accepts new values")
            {
                auto existing = store->get_agent("old-agent");
                REQUIRE(existing);
                REQUIRE(existing->ip_address.empty());
                existing->ip_address = "192.0.2.44";
                store->upsert_agent(*existing);
                REQUIRE(store->get_agent("old-agent")->ip_address == "192.0.2.44");
            }
        }
    }
}

SCENARIO("A pending agent's address is recorded at enrollment and shown by the API")
{
    GIVEN("a running server and a signed-in admin")
    {
        test::ServerFixture fx;
        fx.start_and_login();

        WHEN("an agent enrolls over the enrollment socket")
        {
            REQUIRE(fx.enroll_agent("pending-ip-agent"));

            THEN("the pending list shows the address the server saw")
            {
                auto res = fx.client.Get("/api/pending-enrollments");
                REQUIRE(res);
                REQUIRE(res->status == 200);
                const auto entry = find_in(json::parse(res->body), "pending-ip-agent");
                REQUIRE_FALSE(entry.is_null());
                REQUIRE(entry.at("ip_address") == "127.0.0.1");
            }
        }
    }
}

SCENARIO("An approved agent's address is refreshed by its data-plane frames")
{
    GIVEN("an approved agent whose stored address has been cleared")
    {
        test::ServerFixture fx;
        fx.start_and_login();
        REQUIRE(fx.enroll_and_approve("data-plane-ip-agent"));
        {
            // Enrollment recorded the address; clear it behind the server's back
            // so only a data-plane frame can bring it back.
            sqlite3* raw = nullptr;
            REQUIRE(sqlite3_open(fx.config.db_path.c_str(), &raw) == SQLITE_OK);
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
            sqlite3_busy_timeout(db.get(), 5000);
            REQUIRE(sqlite3_exec(db.get(), "UPDATE agents SET ip_address='';", nullptr, nullptr, nullptr) == SQLITE_OK);

            auto res = fx.client.Get("/api/agents");
            REQUIRE(res);
            const auto entry = find_in(json::parse(res->body), "data-plane-ip-agent");
            REQUIRE_FALSE(entry.is_null());
            REQUIRE(entry.at("ip_address") == "");
        }

        WHEN("the agent sends a heartbeat over the data plane")
        {
            zmq::context_t ctx{1};
            zmq::socket_t dealer{ctx, ZMQ_DEALER};
            dealer.set(zmq::sockopt::linger, 0);
            dealer.connect(fx.config.bind_address);

            proto::Frame frame;
            frame.type = static_cast<uint8_t>(proto::FrameType::HEARTBEAT);
            frame.agent_id = "data-plane-ip-agent";
            frame.timestamp_ms = test::now_ms();
            const auto encoded = proto::encode_frame(frame);
            dealer.send(zmq::message_t{}, zmq::send_flags::sndmore);
            dealer.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);

            THEN("the agent list shows the address the server saw and the agent is connected")
            {
                const auto seen = test::eventually(
                    [&] {
                        auto res = fx.client.Get("/api/agents");
                        if (!res || res->status != 200)
                            return false;
                        const auto entry = find_in(json::parse(res->body), "data-plane-ip-agent");
                        return !entry.is_null() && entry.at("connected") == true &&
                               entry.at("ip_address") == "127.0.0.1";
                    },
                    10s);
                REQUIRE(seen);
            }
        }
    }
}
