// Cross-group access-scope tests (SEC-11).
//
// Per-agent endpoints, runbooks and views must all respect group scoping;
// previously GET /api/agents/:id/history skipped the agent-access check,
// GET /api/runbooks returned every runbook to every viewer, and view
// agent_ids were never validated.
#include "../server/server.hpp"
#include "../server/startup_signal.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <zmq.hpp>

using namespace std::chrono_literals;
using json = nlohmann::json;

namespace
{

int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string free_zmq_endpoint()
{
    zmq::context_t ctx{1};
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    return endpoint;
}

int free_http_port()
{
    // OS-assigned free port: clock-derived ports collide when meson runs
    // test executables in parallel.
    zmq::context_t ctx{1};
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    const auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    const auto colon = endpoint.rfind(':');
    return std::stoi(endpoint.substr(colon + 1));
}

struct ServerFixture
{
    thewatcher::server::ServerConfig config;
    thewatcher::server::Server server;
    thewatcher::server::StartupSignal startup;
    std::exception_ptr error;
    std::thread thread;
    httplib::Client client;

    ServerFixture()
        : server(make_config())
        , client("http://127.0.0.1:" + std::to_string(config.api_port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
        thread = std::thread([this] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                error = std::current_exception();
                startup.fail(error);
            }
        });
    }

    ~ServerFixture()
    {
        server.stop();
        if (thread.joinable())
            thread.join();
        std::error_code ec;
        std::filesystem::remove(config.db_path, ec);
        std::filesystem::remove(config.db_path + "-shm", ec);
        std::filesystem::remove(config.db_path + "-wal", ec);
    }

    bool login(const std::string& username, const std::string& password)
    {
        auto res = client.Post("/api/login",
                               json{
                                   {"username", username},
                                   {"password", password}
        }
                                   .dump(),
                               "application/json");
        if (!res || res->status != 200)
            return false;
        const auto set_cookie = res->get_header_value("Set-Cookie");
        const auto end = set_cookie.find(';');
        if (set_cookie.empty() || end == std::string::npos)
            return false;
        client.set_default_headers({
            {"Cookie", set_cookie.substr(0, end)}
        });
        return true;
    }

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

    // Enrolls an agent and approves it into the given groups.
    bool enroll_and_approve(const std::string& agent_id, const std::vector<int64_t>& group_ids)
    {
        thewatcher::crypto::init();
        const auto keys = thewatcher::crypto::generate_curve_keypair();

        zmq::context_t ctx{1};
        zmq::socket_t req{ctx, ZMQ_REQ};
        req.set(zmq::sockopt::linger, 0);
        req.set(zmq::sockopt::rcvtimeo, 2000);
        req.connect(config.enrollment_address);

        thewatcher::proto::EnrollRequest enroll;
        enroll.agent_id = agent_id;
        enroll.hostname = "scope-test-host";
        enroll.platform = "windows";
        enroll.curve_public_key_z85 = keys.public_key_z85;

        thewatcher::proto::Frame frame;
        frame.type = static_cast<uint8_t>(thewatcher::proto::FrameType::ENROLL_REQUEST);
        frame.agent_id = agent_id;
        frame.timestamp_ms = now_ms();
        frame.payload = thewatcher::proto::pack(enroll);

        const auto encoded = thewatcher::proto::encode_frame(frame);
        req.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
        zmq::message_t reply;
        if (!req.recv(reply, zmq::recv_flags::none).has_value())
            return false;

        auto res = client.Post("/api/agents/" + agent_id + "/approve",
                               json{
                                   {"group_ids", group_ids}
        }
                                   .dump(),
                               "application/json");
        return res && res->status == 200;
    }

private:
    thewatcher::server::ServerConfig make_config()
    {
        thewatcher::crypto::init();
        const auto ts = std::to_string(now_ms());
        const auto base = std::filesystem::temp_directory_path() / ("thewatcher-scope-test-" + ts);

        thewatcher::server::ServerConfig cfg;
        cfg.bind_address = free_zmq_endpoint();
        cfg.enrollment_address = free_zmq_endpoint();
        cfg.api_host = "127.0.0.1";
        cfg.api_port = free_http_port();
        // The (unique) API port keeps concurrent runs from sharing one DB file.
        cfg.db_path = base.string() + "-" + std::to_string(cfg.api_port) + ".db";
        config = cfg;
        return cfg;
    }
};

} // namespace

SCENARIO("Per-agent history enforces group scoping")
{
    GIVEN("a server with group A and group B, an agent in each, and a viewer in group B")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login("thewatcher", "look_at_me"));

        const auto group_a = fx.create_group("scope-group-a");
        const auto group_b = fx.create_group("scope-group-b");
        REQUIRE(group_a.has_value());
        REQUIRE(group_b.has_value());
        REQUIRE(fx.create_user("scope_viewer", "group_viewer", {*group_b}));
        REQUIRE(fx.enroll_and_approve("scope-agent-a", {*group_a}));
        std::this_thread::sleep_for(1100ms); // per-source enrollment rate limit
        REQUIRE(fx.enroll_and_approve("scope-agent-b", {*group_b}));

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
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login("thewatcher", "look_at_me"));

        const auto group_a = fx.create_group("rb-group-a");
        const auto group_b = fx.create_group("rb-group-b");
        REQUIRE(group_a.has_value());
        REQUIRE(group_b.has_value());
        REQUIRE(fx.create_user("rb_viewer", "group_viewer", {*group_b}));
        REQUIRE(fx.enroll_and_approve("rb-agent-a", {*group_a}));
        std::this_thread::sleep_for(1100ms); // per-source enrollment rate limit
        REQUIRE(fx.enroll_and_approve("rb-agent-b", {*group_b}));

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
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login("thewatcher", "look_at_me"));

        const auto group_a = fx.create_group("view-group-a");
        const auto group_b = fx.create_group("view-group-b");
        REQUIRE(group_a.has_value());
        REQUIRE(group_b.has_value());
        REQUIRE(fx.create_user("view_operator", "group_operator", {*group_b}));
        REQUIRE(fx.enroll_and_approve("view-agent-a", {*group_a}));
        std::this_thread::sleep_for(1100ms); // per-source enrollment rate limit
        REQUIRE(fx.enroll_and_approve("view-agent-b", {*group_b}));

        REQUIRE(fx.login("view_operator", "sc0pepw!"));

        WHEN("the operator creates a view referencing group A's agent")
        {
            auto res = fx.client.Post("/api/views",
                                      json{
                                          {"name",      "Cross-group leak"           },
                                          {"is_public", true                         },
                                          {"group_id",  *group_b                     },
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
                                          {"group_id",  *group_b                     },
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