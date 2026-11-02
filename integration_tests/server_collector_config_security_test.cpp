// Collector-config API security tests (SEC-10).
//
// A log-monitor path makes the agent read a local file and surface its
// contents on the dashboard, so the server both validates entries strictly
// and reserves log paths for global admins.
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

    // Enrolls and approves an agent so collector-config requests address a
    // real record. Returns false on failure.
    bool setup_agent(const std::string& agent_id)
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
        enroll.hostname = "collector-test-host";
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

        auto res = client.Post("/api/agents/" + agent_id + "/approve", "", "application/json");
        return res && res->status == 200;
    }

private:
    thewatcher::server::ServerConfig make_config()
    {
        thewatcher::crypto::init();
        const auto ts = std::to_string(now_ms());
        const auto base = std::filesystem::temp_directory_path() / ("thewatcher-collector-api-test-" + ts);

        thewatcher::server::ServerConfig cfg;
        cfg.bind_address = free_zmq_endpoint();
        cfg.enrollment_address = free_zmq_endpoint();
        cfg.api_host = "127.0.0.1";
        cfg.api_port = free_http_port();
        cfg.db_path = base.string() + ".db";
        config = cfg;
        return cfg;
    }
};

json collector_config_body(json logs)
{
    return json{
        {"collection_interval", 30                             },
        {"process_limit",       5                              },
        {"collector_config",    json{{"logs", std::move(logs)}}}
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

} // namespace

SCENARIO("Collector-config log entries are validated and admin-restricted")
{
    GIVEN("a running server with an approved agent and a global admin session")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login("thewatcher", "look_at_me"));
        REQUIRE(fx.setup_agent("cfg-sec-agent"));

        WHEN("the admin submits a valid log monitor entry")
        {
            auto res =
                fx.client.Post("/api/agents/cfg-sec-agent/collector_config",
                               collector_config_body(json::array({valid_log_entry()})).dump(), "application/json");

            THEN("it is accepted")
            {
                REQUIRE(res);
                REQUIRE(res->status == 200);
            }
        }

        WHEN("the admin submits an invalid log severity")
        {
            auto entry = valid_log_entry();
            entry["severity"] = "purple";
            auto res = fx.client.Post("/api/agents/cfg-sec-agent/collector_config",
                                      collector_config_body(json::array({entry})).dump(), "application/json");

            THEN("it is rejected with 400")
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
            }
        }

        WHEN("the admin submits an oversized log pattern")
        {
            auto entry = valid_log_entry();
            entry["pattern"] = std::string(300, 'a');
            auto res = fx.client.Post("/api/agents/cfg-sec-agent/collector_config",
                                      collector_config_body(json::array({entry})).dump(), "application/json");

            THEN("it is rejected with 400")
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
            }
        }
    }
}

SCENARIO("Non-admin operators cannot configure log monitor paths")
{
    GIVEN("a running server with an approved agent, an admin, and a group operator in the agent's group")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.error == nullptr);
        REQUIRE(fx.login("thewatcher", "look_at_me"));
        REQUIRE(fx.setup_agent("cfg-sec-agent-2"));

        // Find the group the approve flow assigns ("Admins") and create a
        // group operator inside it.
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
                                          {"username",  "log_op"                       },
                                          {"password",  "operatorpw!"                  },
                                          {"role",      "group_operator"               },
                                          {"group_ids", json::array({*admins_group_id})}
        }
                                          .dump(),
                                      "application/json");
        REQUIRE(created);
        REQUIRE(created->status == 200);

        // Switch the HTTP session to the group operator.
        REQUIRE(fx.login("log_op", "operatorpw!"));

        WHEN("the operator submits a collector config that includes a log path")
        {
            auto res =
                fx.client.Post("/api/agents/cfg-sec-agent-2/collector_config",
                               collector_config_body(json::array({valid_log_entry()})).dump(), "application/json");

            THEN("the server rejects it with 403")
            {
                REQUIRE(res);
                REQUIRE(res->status == 403);
            }
        }

        WHEN("the operator submits the same config without log entries")
        {
            auto res = fx.client.Post("/api/agents/cfg-sec-agent-2/collector_config",
                                      collector_config_body(json::array()).dump(), "application/json");

            THEN("the operator's other collector settings are still accepted")
            {
                REQUIRE(res);
                REQUIRE(res->status == 200);
            }
        }
    }
}