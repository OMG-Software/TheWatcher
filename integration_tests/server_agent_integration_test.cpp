#include "../agent/agent.hpp"
#include "../server/server.hpp"
#include "../server/startup_signal.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <stdexcept>
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
    // Ask the OS for a free port rather than deriving one from the clock —
    // parallel `meson test` runs otherwise collide across test executables.
    zmq::context_t ctx{1};
    zmq::socket_t socket{ctx, ZMQ_REP};
    socket.bind("tcp://127.0.0.1:*");
    const auto endpoint = socket.get(zmq::sockopt::last_endpoint);
    socket.close();
    const auto colon = endpoint.rfind(':');
    return std::stoi(endpoint.substr(colon + 1));
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout, std::chrono::milliseconds interval = 100ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(interval);
    }
    return predicate();
}

std::optional<json> get_json(httplib::Client& client, const std::string& path)
{
    auto res = client.Get(path);
    if (!res || res->status != 200)
        return std::nullopt;
    return json::parse(res->body);
}

bool login_as_default_admin(httplib::Client& client)
{
    auto res = client.Post("/api/login", R"({"username":"thewatcher","password":"look_at_me"})", "application/json");
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

bool post_ok(httplib::Client& client, const std::string& path)
{
    auto res = client.Post(path);
    return res && res->status == 200;
}

std::optional<json> post_json(httplib::Client& client, const std::string& path, const json& body)
{
    auto res = client.Post(path, body.dump(), "application/json");
    if (!res || res->status != 200)
        return std::nullopt;
    return json::parse(res->body);
}

bool put_json_ok(httplib::Client& client, const std::string& path, const json& body)
{
    auto res = client.Put(path, body.dump(), "application/json");
    return res && res->status == 200;
}

bool put_json_error(httplib::Client& client, const std::string& path, const json& body)
{
    auto res = client.Put(path, body.dump(), "application/json");
    return res && res->status == 400;
}

bool delete_ok(httplib::Client& client, const std::string& path)
{
    auto res = client.Delete(path);
    return res && res->status == 200;
}

std::optional<json> find_agent(httplib::Client& client, const std::string& agent_id)
{
    auto body = get_json(client, "/api/agents");
    if (!body)
        return std::nullopt;

    for (const auto& agent : *body)
    {
        if (agent.value("agent_id", "") == agent_id)
            return agent;
    }
    return std::nullopt;
}

std::optional<json> find_pending_agent(httplib::Client& client, const std::string& agent_id)
{
    auto body = get_json(client, "/api/pending-enrollments");
    if (!body)
        return std::nullopt;

    for (const auto& agent : *body)
    {
        if (agent.value("agent_id", "") == agent_id)
            return agent;
    }
    return std::nullopt;
}

std::optional<json> find_metrics(httplib::Client& client, const std::string& agent_id)
{
    auto body = get_json(client, "/api/metrics");
    if (!body)
        return std::nullopt;

    for (const auto& row : *body)
    {
        if (row.value("agent_id", "") == agent_id)
            return row.at("metrics");
    }
    return std::nullopt;
}

thewatcher::proto::EnrollResponse send_enrollment_request(const thewatcher::agent::AgentConfig& agent_config)
{
    zmq::context_t ctx{1};
    zmq::socket_t req{ctx, ZMQ_REQ};
    req.set(zmq::sockopt::linger, 0);
    req.set(zmq::sockopt::rcvtimeo, 2000);
    req.connect(agent_config.enrollment_address);

    thewatcher::proto::EnrollRequest enroll;
    enroll.agent_id = agent_config.agent_id;
    enroll.hostname = "integration-host";
    enroll.platform = "integration-platform";
    enroll.curve_public_key_z85 = agent_config.agent_public_key;

    thewatcher::proto::Frame frame;
    frame.type = static_cast<uint8_t>(thewatcher::proto::FrameType::ENROLL_REQUEST);
    frame.agent_id = agent_config.agent_id;
    frame.timestamp_ms = now_ms();
    frame.payload = thewatcher::proto::pack(enroll);

    const auto encoded = thewatcher::proto::encode_frame(frame);
    req.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);

    zmq::message_t reply;
    auto received = req.recv(reply, zmq::recv_flags::none);
    if (!received)
        throw std::runtime_error("server did not respond to enrollment request");

    const auto reply_frame = thewatcher::proto::decode_frame(reply.data(), reply.size());
    return thewatcher::proto::unpack<thewatcher::proto::EnrollResponse>(reply_frame.payload);
}

struct ServerThreadGuard
{
    thewatcher::server::Server& server;
    std::thread& thread;

    ~ServerThreadGuard()
    {
        server.stop();
        if (thread.joinable())
            thread.join();
    }
};

struct AgentThreadGuard
{
    thewatcher::agent::Agent& agent;
    std::thread& thread;

    ~AgentThreadGuard()
    {
        agent.stop();
        if (thread.joinable())
            thread.join();
    }
};

} // namespace

SCENARIO("A real agent enrolls, sends metrics, disconnects, and the server records the lifecycle")
{
    GIVEN("a server configured with isolated local endpoints and a real agent using generated CURVE keys")
    {
        thewatcher::crypto::init();
        const auto server_keys = thewatcher::crypto::generate_curve_keypair();
        const auto agent_keys = thewatcher::crypto::generate_curve_keypair();
        const std::string agent_id = "integration-agent-" + std::to_string(now_ms());

        const auto db_base =
            std::filesystem::temp_directory_path() / ("thewatcher-integration-" + std::to_string(now_ms()));

        thewatcher::server::ServerConfig server_config;
        server_config.bind_address = free_zmq_endpoint();
        server_config.enrollment_address = free_zmq_endpoint();
        server_config.api_host = "127.0.0.1";
        server_config.api_port = free_http_port();
        server_config.db_path = db_base.string() + ".db";
        server_config.server_public_key = server_keys.public_key_z85;
        server_config.server_secret_key = server_keys.secret_key_z85;

        thewatcher::agent::AgentConfig agent_config;
        agent_config.agent_id = agent_id;
        agent_config.server_address = server_config.bind_address;
        agent_config.enrollment_address = server_config.enrollment_address;
        agent_config.agent_public_key = agent_keys.public_key_z85;
        agent_config.agent_secret_key = agent_keys.secret_key_z85;
        agent_config.collection_interval = 1;
        agent_config.process_limit = 5;

        thewatcher::server::Server server(server_config);
        thewatcher::server::StartupSignal startup;
        std::exception_ptr server_error;

        std::thread server_thread([&] {
            try
            {
                server.run(&startup);
            }
            catch (...)
            {
                server_error = std::current_exception();
                startup.fail(server_error);
            }
        });
        ServerThreadGuard server_guard{server, server_thread};

        const auto startup_result = startup.wait();
        REQUIRE(startup_result.started);
        REQUIRE(server_error == nullptr);

        httplib::Client client("http://127.0.0.1:" + std::to_string(server_config.api_port));
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);

        WHEN("the agent enrollment is approved through the server API")
        {
            REQUIRE(eventually(
                [&] {
                    return login_as_default_admin(client);
                },
                20s));

            REQUIRE(eventually(
                [&] {
                    return get_json(client, "/api/agents").has_value();
                },
                5s));

            const auto pending_response = send_enrollment_request(agent_config);
            REQUIRE_FALSE(pending_response.approved);
            REQUIRE(pending_response.message == "pending approval");

            REQUIRE(eventually(
                [&] {
                    const auto pending = find_pending_agent(client, agent_id);
                    return pending && !pending->value("approved", true) && !pending->value("rejected", true);
                },
                5s));

            REQUIRE(post_ok(client, "/api/agents/" + agent_id + "/approve"));
            // A second enrollment request within 10 s is rate-limited; verify approval
            // via the HTTP API instead and let the CURVE handshake prove key exchange.
            REQUIRE(eventually(
                [&] {
                    const auto approved = find_agent(client, agent_id);
                    return approved && approved->value("approved", false);
                },
                5s));

            thewatcher::agent::Agent agent(agent_config);
            std::thread agent_thread([&] {
                agent.start();
            });
            AgentThreadGuard agent_guard{agent, agent_thread};

            if (agent_thread.joinable())
                agent_thread.join();

            THEN("the server receives collector data and records the agent disconnect")
            {
                std::optional<json> metrics;
                REQUIRE(eventually(
                    [&] {
                        metrics = find_metrics(client, agent_id);
                        return metrics.has_value();
                    },
                    15s, 200ms));

                REQUIRE(metrics->contains("cpu"));
                REQUIRE(metrics->contains("memory"));
                REQUIRE(metrics->contains("disks"));
                REQUIRE(metrics->contains("temperatures"));
                REQUIRE(metrics->contains("top_processes"));
                REQUIRE(metrics->contains("networks"));
                REQUIRE(metrics->contains("os_name"));
                REQUIRE(metrics->contains("os_version"));
                REQUIRE(metrics->contains("hostname"));
                REQUIRE(metrics->contains("platform"));
                REQUIRE(metrics->contains("uptime_seconds"));
                REQUIRE(metrics->at("memory").value("total_bytes", 0ULL) > 0);
                REQUIRE_FALSE(metrics->at("disks").empty());

                REQUIRE(eventually(
                    [&] {
                        const auto connected_agent = find_agent(client, agent_id);
                        return connected_agent && connected_agent->value("connected", false);
                    },
                    5s, 100ms));

                REQUIRE(post_ok(client, "/api/agents/" + agent_id + "/disconnect"));

                REQUIRE(eventually(
                    [&] {
                        const auto disconnected_agent = find_agent(client, agent_id);
                        return disconnected_agent && !disconnected_agent->value("connected", true);
                    },
                    5s, 100ms));

                // Status history is recorded as the agent transitions through states
                REQUIRE(eventually(
                    [&] {
                        const auto history = get_json(client, "/api/agents/" + agent_id + "/history?limit=20");
                        return history.has_value() && !history->empty();
                    },
                    5s, 200ms));

                const auto history = get_json(client, "/api/agents/" + agent_id + "/history?limit=20");
                REQUIRE(history.has_value());
                REQUIRE_FALSE(history->empty());

                // Each row must carry all required fields
                for (const auto& row : *history)
                {
                    REQUIRE(row.contains("id"));
                    REQUIRE(row.contains("agent_id"));
                    REQUIRE(row.contains("indicator"));
                    REQUIRE(row.contains("old_status"));
                    REQUIRE(row.contains("new_status"));
                    REQUIRE(row.contains("message"));
                    REQUIRE(row.contains("created_at"));
                    REQUIRE(row["agent_id"].get<std::string>() == agent_id);
                    REQUIRE(row["created_at"].get<int64_t>() > 0);
                }

                // The limit parameter is respected
                const auto limited = get_json(client, "/api/agents/" + agent_id + "/history?limit=1");
                REQUIRE(limited.has_value());
                REQUIRE(limited->size() <= 1);
            }
        }

        server.stop();
        if (server_thread.joinable())
            server_thread.join();

        std::error_code ec;
        std::filesystem::remove(server_config.db_path, ec);
        std::filesystem::remove(server_config.db_path + "-shm", ec);
        std::filesystem::remove(server_config.db_path + "-wal", ec);
    }
}

// ── Helpers shared by the API-only scenarios ───────────────────────────────────

namespace
{

struct ServerFixture
{
    thewatcher::server::ServerConfig config;
    thewatcher::server::Server server;
    thewatcher::server::StartupSignal startup;
    std::exception_ptr error;
    std::thread thread;
    httplib::Client client;

    explicit ServerFixture()
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

    bool login()
    {
        // Generous window: under a parallel `meson test` run the host may be
        // busy with other suites' argon2 password verifications, which makes
        // individual login attempts slow.
        return eventually(
            [this] {
                return login_as_default_admin(client);
            },
            20s);
    }

private:
    thewatcher::server::ServerConfig make_config()
    {
        thewatcher::crypto::init();
        const auto keys = thewatcher::crypto::generate_curve_keypair();
        const auto ts = std::to_string(now_ms());
        const auto base = std::filesystem::temp_directory_path() / ("thewatcher-api-test-" + ts);

        thewatcher::server::ServerConfig cfg;
        cfg.bind_address = free_zmq_endpoint();
        cfg.enrollment_address = free_zmq_endpoint();
        cfg.api_host = "127.0.0.1";
        cfg.api_port = free_http_port();
        cfg.db_path = base.string() + ".db";
        cfg.server_public_key = keys.public_key_z85;
        cfg.server_secret_key = keys.secret_key_z85;
        config = cfg;
        return cfg;
    }
};

bool login_as(httplib::Client& client, const std::string& username, const std::string& password)
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

// POST expecting HTTP 201 Created (used by the views endpoint).
std::optional<json> post_json_created(httplib::Client& client, const std::string& path, const json& body)
{
    auto res = client.Post(path, body.dump(), "application/json");
    if (!res || res->status != 201)
        return std::nullopt;
    return json::parse(res->body);
}

} // namespace

// ── Settings API ──────────────────────────────────────────────────────────────

SCENARIO("Server settings can be read and updated via the HTTP API")
{
    GIVEN("a running server with default settings and an authenticated admin")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("the admin reads the current settings")
        {
            const auto settings = get_json(fx.client, "/api/settings");

            THEN("the response contains all expected setting keys with sensible defaults")
            {
                REQUIRE(settings.has_value());
                REQUIRE(settings->contains("webhook_url"));
                REQUIRE(settings->contains("offline_after_seconds"));
                REQUIRE(settings->contains("escalation_timeout_seconds"));
                REQUIRE(settings->contains("metrics_retention_days"));
                REQUIRE((*settings)["offline_after_seconds"].get<int>() > 0);
                REQUIRE((*settings)["metrics_retention_days"].get<int>() == 30);
            }
        }

        WHEN("the admin updates metrics_retention_days to 7")
        {
            REQUIRE(put_json_ok(fx.client, "/api/settings",
                                {
                                    {"metrics_retention_days", 7}
            }));

            THEN("subsequent GET /api/settings reflects the new value")
            {
                const auto updated = get_json(fx.client, "/api/settings");
                REQUIRE(updated.has_value());
                REQUIRE((*updated)["metrics_retention_days"].get<int>() == 7);
            }
        }

        WHEN("the admin submits an out-of-range metrics_retention_days value")
        {
            THEN("the server rejects it with a 400 response")
            {
                REQUIRE(put_json_error(fx.client, "/api/settings",
                                       {
                                           {"metrics_retention_days", 0}
                }));
                REQUIRE(put_json_error(fx.client, "/api/settings",
                                       {
                                           {"metrics_retention_days", 366}
                }));
            }
        }

        WHEN("the admin updates offline_after_seconds")
        {
            REQUIRE(put_json_ok(fx.client, "/api/settings",
                                {
                                    {"offline_after_seconds", 60}
            }));

            THEN("the value is persisted")
            {
                const auto updated = get_json(fx.client, "/api/settings");
                REQUIRE(updated.has_value());
                REQUIRE((*updated)["offline_after_seconds"].get<int>() == 60);
            }
        }

        WHEN("an unauthenticated request is made to GET /api/settings")
        {
            httplib::Client unauth("http://127.0.0.1:" + std::to_string(fx.config.api_port));
            unauth.set_connection_timeout(5, 0);
            unauth.set_read_timeout(5, 0);
            auto res = unauth.Get("/api/settings");

            THEN("the server returns 401")
            {
                REQUIRE(res);
                REQUIRE(res->status == 401);
            }
        }
    }
}

// ── Silence rules API ─────────────────────────────────────────────────────────

SCENARIO("Alert silence rules can be created, listed, and deleted via the HTTP API")
{
    GIVEN("a running server and an authenticated operator")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("the operator lists silence rules on a fresh server")
        {
            const auto list = get_json(fx.client, "/api/silences");

            THEN("the list is empty")
            {
                REQUIRE(list.has_value());
                REQUIRE(list->is_array());
                REQUIRE(list->empty());
            }
        }

        WHEN("the operator creates a global silence rule")
        {
            const int64_t until_ms = now_ms() + 3'600'000;
            const auto result = post_json(fx.client, "/api/silences",
                                          {
                                              {"agent_id",  "*"               },
                                              {"indicator", "*"               },
                                              {"reason",    "integration test"},
                                              {"until_ms",  until_ms          },
            });

            THEN("the server returns a silence_id")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->contains("silence_id"));
                const auto silence_id = (*result)["silence_id"].get<int64_t>();
                REQUIRE(silence_id > 0);

                AND_THEN("GET /api/silences returns the new rule with correct fields")
                {
                    const auto list = get_json(fx.client, "/api/silences");
                    REQUIRE(list.has_value());
                    REQUIRE(list->size() == 1);
                    const auto& rule = list->at(0);
                    REQUIRE(rule["agent_id"].get<std::string>() == "*");
                    REQUIRE(rule["indicator"].get<std::string>() == "*");
                    REQUIRE(rule["reason"].get<std::string>() == "integration test");
                    REQUIRE(rule["until_ms"].get<int64_t>() == until_ms);
                    REQUIRE(rule["created_by"].get<std::string>() == "thewatcher");
                }

                AND_THEN("deleting the rule removes it from the list")
                {
                    REQUIRE(delete_ok(fx.client, "/api/silences/" + std::to_string(silence_id)));

                    const auto after = get_json(fx.client, "/api/silences");
                    REQUIRE(after.has_value());
                    REQUIRE(after->empty());
                }
            }
        }

        WHEN("the operator creates a silence with until_ms in the past")
        {
            const auto result = post_json(fx.client, "/api/silences",
                                          {
                                              {"agent_id",  "*"          },
                                              {"indicator", "*"          },
                                              {"reason",    "stale"      },
                                              {"until_ms",  int64_t(1000)},
            });

            THEN("the server rejects it with a 400 response")
            {
                REQUIRE_FALSE(result.has_value());
            }
        }

        WHEN("a silence scoped to a specific agent and indicator is created")
        {
            const int64_t until_ms = now_ms() + 7'200'000;
            const auto result = post_json(fx.client, "/api/silences",
                                          {
                                              {"agent_id",  "my-agent"        },
                                              {"indicator", "cpu"             },
                                              {"reason",    "cpu spike window"},
                                              {"until_ms",  until_ms          },
            });

            THEN("the scoped rule appears in the list with correct fields")
            {
                REQUIRE(result.has_value());
                const auto list = get_json(fx.client, "/api/silences");
                REQUIRE(list.has_value());
                REQUIRE(list->size() == 1);
                REQUIRE(list->at(0)["agent_id"].get<std::string>() == "my-agent");
                REQUIRE(list->at(0)["indicator"].get<std::string>() == "cpu");
            }
        }
    }
}

// ── Maintenance windows API ───────────────────────────────────────────────────

SCENARIO("Maintenance windows can be scheduled and cancelled via the HTTP API")
{
    GIVEN("a running server and an authenticated operator")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("the operator lists maintenance windows on a fresh server")
        {
            const auto list = get_json(fx.client, "/api/maintenance-windows");

            THEN("the list is empty")
            {
                REQUIRE(list.has_value());
                REQUIRE(list->is_array());
                REQUIRE(list->empty());
            }
        }

        WHEN("a maintenance window is created for all agents")
        {
            const int64_t start_ms = now_ms() + 60'000;
            const int64_t end_ms = start_ms + 3'600'000;
            const auto result = post_json(fx.client, "/api/maintenance-windows",
                                          {
                                              {"agent_id", "*"              },
                                              {"start_ms", start_ms         },
                                              {"end_ms",   end_ms           },
                                              {"reason",   "weekly patching"},
            });

            THEN("the server returns a window_id")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->contains("window_id"));
                const auto window_id = (*result)["window_id"].get<int64_t>();
                REQUIRE(window_id > 0);

                AND_THEN("GET /api/maintenance-windows returns the scheduled window")
                {
                    const auto list = get_json(fx.client, "/api/maintenance-windows");
                    REQUIRE(list.has_value());
                    REQUIRE(list->size() == 1);
                    const auto& w = list->at(0);
                    REQUIRE(w["agent_id"].get<std::string>() == "*");
                    REQUIRE(w["start_ms"].get<int64_t>() == start_ms);
                    REQUIRE(w["end_ms"].get<int64_t>() == end_ms);
                    REQUIRE(w["reason"].get<std::string>() == "weekly patching");
                    REQUIRE(w["created_by"].get<std::string>() == "thewatcher");
                }

                AND_THEN("deleting the window removes it")
                {
                    REQUIRE(delete_ok(fx.client, "/api/maintenance-windows/" + std::to_string(window_id)));

                    const auto after = get_json(fx.client, "/api/maintenance-windows");
                    REQUIRE(after.has_value());
                    REQUIRE(after->empty());
                }
            }
        }

        WHEN("a window with end_ms before start_ms is submitted")
        {
            const int64_t start_ms = now_ms() + 3'600'000;
            const int64_t end_ms = start_ms - 1;
            const auto result = post_json(fx.client, "/api/maintenance-windows",
                                          {
                                              {"agent_id", "*"     },
                                              {"start_ms", start_ms},
                                              {"end_ms",   end_ms  },
                                              {"reason",   "bad"   },
            });

            THEN("the server rejects it with a 400 response")
            {
                REQUIRE_FALSE(result.has_value());
            }
        }
    }
}

// ── User management API ───────────────────────────────────────────────────────

SCENARIO("User management API supports create, list, disable, enable, password change, and delete")
{
    GIVEN("a running server with an authenticated admin")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("the admin creates a new group viewer user 'alice'")
        {
            const auto result = post_json(fx.client, "/api/users",
                                          {
                                              {"username", "alice"       },
                                              {"password", "s3cure!pw"   },
                                              {"role",     "group_viewer"},
            });

            THEN("the server returns a positive user_id")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->contains("user_id"));
                const auto user_id = (*result)["user_id"].get<int64_t>();
                REQUIRE(user_id > 0);

                AND_THEN("alice appears in GET /api/users")
                {
                    const auto users = get_json(fx.client, "/api/users");
                    REQUIRE(users.has_value());
                    bool found = false;
                    for (const auto& u : *users)
                        if (u.value("username", "") == "alice")
                            found = true;
                    REQUIRE(found);
                }

                AND_THEN("alice can login with her initial password")
                {
                    httplib::Client alice("http://127.0.0.1:" + std::to_string(fx.config.api_port));
                    alice.set_connection_timeout(5, 0);
                    alice.set_read_timeout(5, 0);
                    REQUIRE(login_as(alice, "alice", "s3cure!pw"));

                    AND_THEN("a group viewer can read only visible user records")
                    {
                        const auto users = get_json(alice, "/api/users");
                        REQUIRE(users.has_value());
                        REQUIRE(users->size() == 1);
                        REQUIRE(users->at(0)["username"].get<std::string>() == "alice");
                    }
                }

                AND_THEN("the admin can change alice's password and the old password stops working")
                {
                    REQUIRE(put_json_ok(fx.client, "/api/users/" + std::to_string(user_id) + "/password",
                                        {
                                            {"password", "n3wpassword!"}
                    }));

                    httplib::Client alice("http://127.0.0.1:" + std::to_string(fx.config.api_port));
                    alice.set_connection_timeout(5, 0);
                    alice.set_read_timeout(5, 0);
                    REQUIRE_FALSE(login_as(alice, "alice", "s3cure!pw"));
                    REQUIRE(login_as(alice, "alice", "n3wpassword!"));
                }

                AND_THEN("disabling alice prevents login and re-enabling restores it")
                {
                    const auto dis =
                        fx.client.Put("/api/users/" + std::to_string(user_id) + "/disable", "", "application/json");
                    REQUIRE(dis);
                    REQUIRE(dis->status == 200);

                    httplib::Client alice("http://127.0.0.1:" + std::to_string(fx.config.api_port));
                    alice.set_connection_timeout(5, 0);
                    alice.set_read_timeout(5, 0);
                    REQUIRE_FALSE(login_as(alice, "alice", "s3cure!pw"));

                    const auto en =
                        fx.client.Put("/api/users/" + std::to_string(user_id) + "/enable", "", "application/json");
                    REQUIRE(en);
                    REQUIRE(en->status == 200);

                    REQUIRE(login_as(alice, "alice", "s3cure!pw"));
                }

                AND_THEN("the admin can delete alice and she no longer appears in the user list")
                {
                    REQUIRE(delete_ok(fx.client, "/api/users/" + std::to_string(user_id)));

                    const auto users = get_json(fx.client, "/api/users");
                    REQUIRE(users.has_value());
                    bool found = false;
                    for (const auto& u : *users)
                        if (u.value("username", "") == "alice")
                            found = true;
                    REQUIRE_FALSE(found);
                }
            }
        }

        WHEN("the admin submits a user creation request with an invalid role")
        {
            const auto result = post_json(fx.client, "/api/users",
                                          {
                                              {"username", "baduser"   },
                                              {"password", "pw123"     },
                                              {"role",     "superadmin"},
            });

            THEN("the server rejects it with 400")
            {
                REQUIRE_FALSE(result.has_value());
            }
        }

        WHEN("the admin submits a user creation request with an empty username")
        {
            const auto result = post_json(fx.client, "/api/users",
                                          {
                                              {"username", ""     },
                                              {"password", "pw123"},
            });

            THEN("the server rejects it with 400")
            {
                REQUIRE_FALSE(result.has_value());
            }
        }
    }
}

// ── Group management API ──────────────────────────────────────────────────────

SCENARIO("Group management API supports create and list with name validation")
{
    GIVEN("a running server with an authenticated admin")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("the admin lists groups on a fresh server")
        {
            const auto list = get_json(fx.client, "/api/groups");

            THEN("the response is an array (built-in groups may already exist)")
            {
                REQUIRE(list.has_value());
                REQUIRE(list->is_array());
            }
        }

        WHEN("the admin creates a group named 'ops-team'")
        {
            const auto result = post_json(fx.client, "/api/groups",
                                          {
                                              {"name", "ops-team"}
            });

            THEN("the server returns a positive group_id")
            {
                REQUIRE(result.has_value());
                REQUIRE(result->contains("group_id"));
                const auto group_id = (*result)["group_id"].get<int64_t>();
                REQUIRE(group_id > 0);

                AND_THEN("GET /api/groups lists the new group with its name and group_id")
                {
                    const auto list = get_json(fx.client, "/api/groups");
                    REQUIRE(list.has_value());
                    bool found = false;
                    for (const auto& g : *list)
                        if (g.value("group_id", int64_t{0}) == group_id && g.value("name", "") == "ops-team")
                            found = true;
                    REQUIRE(found);
                }
            }
        }

        WHEN("the admin submits an empty group name")
        {
            const auto result = post_json(fx.client, "/api/groups",
                                          {
                                              {"name", ""}
            });

            THEN("the server rejects it with 400")
            {
                REQUIRE_FALSE(result.has_value());
            }
        }

        WHEN("the admin submits a group name that exceeds 64 characters")
        {
            const auto result = post_json(fx.client, "/api/groups",
                                          {
                                              {"name", std::string(65, 'x')}
            });

            THEN("the server rejects it with 400")
            {
                REQUIRE_FALSE(result.has_value());
            }
        }
    }
}

// ── Views API ─────────────────────────────────────────────────────────────────

SCENARIO("Group scoped accounts only see their assigned group and views")
{
    GIVEN("a running server with two groups and an authenticated group operator in one group")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        const auto ops_group = post_json(fx.client, "/api/groups",
                                         {
                                             {"name", "ops-visible"}
        });
        const auto db_group = post_json(fx.client, "/api/groups",
                                        {
                                            {"name", "db-hidden"}
        });
        REQUIRE(ops_group.has_value());
        REQUIRE(db_group.has_value());
        const auto ops_group_id = (*ops_group)["group_id"].get<int64_t>();
        const auto db_group_id = (*db_group)["group_id"].get<int64_t>();

        REQUIRE(post_json(fx.client, "/api/users",
                          {
                              {"username",  "ops_operator"             },
                              {"password",  "operatorpw!"              },
                              {"role",      "group_operator"           },
                              {"group_ids", json::array({ops_group_id})}
        })
                    .has_value());

        const auto ops_view = post_json_created(
            fx.client, "/api/views",
            {
                {"name",      "Ops view"   },
                {"is_public", true         },
                {"group_id",  ops_group_id },
                {"agent_ids", json::array()}
        });
        const auto db_view = post_json_created(
            fx.client, "/api/views",
            {
                {"name",      "DB view"    },
                {"is_public", true         },
                {"group_id",  db_group_id  },
                {"agent_ids", json::array()}
        });
        REQUIRE(ops_view.has_value());
        REQUIRE(db_view.has_value());

        httplib::Client ops("http://127.0.0.1:" + std::to_string(fx.config.api_port));
        ops.set_connection_timeout(5, 0);
        ops.set_read_timeout(5, 0);
        REQUIRE(login_as(ops, "ops_operator", "operatorpw!"));

        WHEN("the group operator lists groups")
        {
            const auto groups = get_json(ops, "/api/groups");

            THEN("only the operator's assigned group is returned")
            {
                REQUIRE(groups.has_value());
                REQUIRE(groups->size() == 1);
                REQUIRE(groups->at(0)["group_id"].get<int64_t>() == ops_group_id);
                REQUIRE(groups->at(0)["name"].get<std::string>() == "ops-visible");
            }
        }

        WHEN("the group operator lists views")
        {
            const auto views = get_json(ops, "/api/views");

            THEN("only views assigned to the operator's group are returned")
            {
                REQUIRE(views.has_value());
                REQUIRE(views->size() == 1);
                REQUIRE(views->at(0)["view_id"].get<int64_t>() == (*ops_view)["view_id"].get<int64_t>());
                REQUIRE(views->at(0)["name"].get<std::string>() == "Ops view");
            }
        }

        WHEN("the group operator opens a public view from another group")
        {
            const auto res = ops.Get("/api/views/" + std::to_string((*db_view)["view_id"].get<int64_t>()));

            THEN("the server rejects access")
            {
                REQUIRE(res);
                REQUIRE(res->status == 403);
            }
        }
    }
}

SCENARIO("Views API supports full CRUD and enforces public/private visibility between users")
{
    GIVEN("a running server with an authenticated admin")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("the admin lists views on a fresh server")
        {
            const auto list = get_json(fx.client, "/api/views");

            THEN("the response is an empty array")
            {
                REQUIRE(list.has_value());
                REQUIRE(list->is_array());
                REQUIRE(list->empty());
            }
        }

        WHEN("the admin creates a private view with two agent IDs")
        {
            const auto created = post_json_created(fx.client, "/api/views",
                                                   {
                                                       {"name",      "Production"                       },
                                                       {"is_public", false                              },
                                                       {"agent_ids", json::array({"agent-a", "agent-b"})},
            });

            THEN("the server responds 201 with the full view record")
            {
                REQUIRE(created.has_value());
                REQUIRE(created->contains("view_id"));
                const auto view_id = (*created)["view_id"].get<int64_t>();
                REQUIRE(view_id > 0);
                REQUIRE((*created)["name"].get<std::string>() == "Production");
                REQUIRE((*created)["is_public"].get<bool>() == false);
                REQUIRE((*created)["agent_ids"].size() == 2);

                AND_THEN("GET /api/views lists the created view")
                {
                    const auto list = get_json(fx.client, "/api/views");
                    REQUIRE(list.has_value());
                    REQUIRE(list->size() == 1);
                    REQUIRE(list->at(0)["view_id"].get<int64_t>() == view_id);
                }

                AND_THEN("GET /api/views/:id returns the correct record")
                {
                    const auto v = get_json(fx.client, "/api/views/" + std::to_string(view_id));
                    REQUIRE(v.has_value());
                    REQUIRE((*v)["name"].get<std::string>() == "Production");
                    REQUIRE((*v)["agent_ids"].size() == 2);
                }

                AND_THEN("PUT /api/views/:id updates the name and agent list")
                {
                    REQUIRE(put_json_ok(fx.client, "/api/views/" + std::to_string(view_id),
                                        {
                                            {"name",      "Production (updated)"                        },
                                            {"agent_ids", json::array({"agent-a", "agent-b", "agent-c"})},
                    }));

                    const auto v = get_json(fx.client, "/api/views/" + std::to_string(view_id));
                    REQUIRE(v.has_value());
                    REQUIRE((*v)["name"].get<std::string>() == "Production (updated)");
                    REQUIRE((*v)["agent_ids"].size() == 3);
                }

                AND_THEN("DELETE /api/views/:id removes the view from the list")
                {
                    REQUIRE(delete_ok(fx.client, "/api/views/" + std::to_string(view_id)));

                    const auto list = get_json(fx.client, "/api/views");
                    REQUIRE(list.has_value());
                    REQUIRE(list->empty());
                }
            }
        }

        WHEN("a private view is created and a different viewer user tries to access it")
        {
            const auto alice_result = post_json(fx.client, "/api/users",
                                                {
                                                    {"username", "alice_view"  },
                                                    {"password", "alicepw!"    },
                                                    {"role",     "group_viewer"},
            });
            REQUIRE(alice_result.has_value());

            const auto created = post_json_created(fx.client, "/api/views",
                                                   {
                                                       {"name",      "Private"},
                                                       {"is_public", false    },
            });
            REQUIRE(created.has_value());
            const auto view_id = (*created)["view_id"].get<int64_t>();

            httplib::Client alice("http://127.0.0.1:" + std::to_string(fx.config.api_port));
            alice.set_connection_timeout(5, 0);
            alice.set_read_timeout(5, 0);
            REQUIRE(login_as(alice, "alice_view", "alicepw!"));

            THEN("alice receives 403 on GET /api/views/:id")
            {
                const auto res = alice.Get("/api/views/" + std::to_string(view_id));
                REQUIRE(res);
                REQUIRE(res->status == 403);
            }

            AND_THEN("the private view is absent from alice's GET /api/views list")
            {
                const auto list = get_json(alice, "/api/views");
                REQUIRE(list.has_value());
                REQUIRE(list->empty());
            }

            AND_THEN("alice cannot delete the private view owned by admin (403)")
            {
                const auto res = alice.Delete("/api/views/" + std::to_string(view_id));
                REQUIRE(res);
                REQUIRE(res->status == 403);
            }
        }

        WHEN("a public view is created another user can read it")
        {
            const auto shared_group = post_json(fx.client, "/api/groups",
                                                {
                                                    {"name", "shared-view-group"}
            });
            REQUIRE(shared_group.has_value());
            const auto shared_group_id = (*shared_group)["group_id"].get<int64_t>();

            const auto bob_result = post_json(fx.client, "/api/users",
                                              {
                                                  {"username",  "bob_view"                    },
                                                  {"password",  "bobpw!"                      },
                                                  {"role",      "group_viewer"                },
                                                  {"group_ids", json::array({shared_group_id})}
            });
            REQUIRE(bob_result.has_value());

            const auto created = post_json_created(fx.client, "/api/views",
                                                   {
                                                       {"name",      "Public Fleet" },
                                                       {"is_public", true           },
                                                       {"group_id",  shared_group_id},
            });
            REQUIRE(created.has_value());
            const auto view_id = (*created)["view_id"].get<int64_t>();

            httplib::Client bob("http://127.0.0.1:" + std::to_string(fx.config.api_port));
            bob.set_connection_timeout(5, 0);
            bob.set_read_timeout(5, 0);
            REQUIRE(login_as(bob, "bob_view", "bobpw!"));

            THEN("bob can GET the public view directly")
            {
                const auto v = get_json(bob, "/api/views/" + std::to_string(view_id));
                REQUIRE(v.has_value());
                REQUIRE((*v)["name"].get<std::string>() == "Public Fleet");
            }

            AND_THEN("the public view appears in bob's GET /api/views list")
            {
                const auto list = get_json(bob, "/api/views");
                REQUIRE(list.has_value());
                REQUIRE(list->size() == 1);
                REQUIRE(list->at(0)["view_id"].get<int64_t>() == view_id);
            }
        }

        WHEN("a view is created with an empty name")
        {
            const auto res = fx.client.Post("/api/views",
                                            json{
                                                {"name", ""}
            }
                                                .dump(),
                                            "application/json");

            THEN("the server rejects it with 400")
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
            }
        }
    }
}

// ── Alert listing API ─────────────────────────────────────────────────────────

SCENARIO("Alert API returns well-formed responses and enforces authentication on a server with no agents")
{
    GIVEN("a running server with no agents enrolled")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("GET /api/alerts is called")
        {
            const auto alerts = get_json(fx.client, "/api/alerts");

            THEN("the response is an empty JSON array")
            {
                REQUIRE(alerts.has_value());
                REQUIRE(alerts->is_array());
                REQUIRE(alerts->empty());
            }
        }

        WHEN("GET /api/alerts?include_archived=1 is called")
        {
            const auto alerts = get_json(fx.client, "/api/alerts?include_archived=1");

            THEN("the response is an empty JSON array")
            {
                REQUIRE(alerts.has_value());
                REQUIRE(alerts->is_array());
                REQUIRE(alerts->empty());
            }
        }

        WHEN("GET /api/alerts/unacknowledged is called")
        {
            const auto alerts = get_json(fx.client, "/api/alerts/unacknowledged");

            THEN("the response is an empty JSON array")
            {
                REQUIRE(alerts.has_value());
                REQUIRE(alerts->is_array());
                REQUIRE(alerts->empty());
            }
        }

        WHEN("bulk-ack is called with an empty alert_ids array")
        {
            const auto result = post_json(fx.client, "/api/alerts/bulk-ack",
                                          {
                                              {"alert_ids", json::array()     },
                                              {"note",      "integration test"},
            });

            THEN("the server responds ok with count 0")
            {
                REQUIRE(result.has_value());
                REQUIRE((*result)["ok"].get<bool>() == true);
                REQUIRE((*result)["count"].get<int>() == 0);
            }
        }

        WHEN("bulk-archive is called with an empty alert_ids array")
        {
            const auto result = post_json(fx.client, "/api/alerts/bulk-archive",
                                          {
                                              {"alert_ids", json::array()},
            });

            THEN("the server responds ok with count 0")
            {
                REQUIRE(result.has_value());
                REQUIRE((*result)["ok"].get<bool>() == true);
                REQUIRE((*result)["count"].get<int>() == 0);
            }
        }

        WHEN("an unauthenticated client calls GET /api/alerts")
        {
            httplib::Client unauth("http://127.0.0.1:" + std::to_string(fx.config.api_port));
            unauth.set_connection_timeout(5, 0);
            unauth.set_read_timeout(5, 0);
            const auto res = unauth.Get("/api/alerts");

            THEN("the server returns 401")
            {
                REQUIRE(res);
                REQUIRE(res->status == 401);
            }
        }
    }
}

// ── Session and login API ─────────────────────────────────────────────────────

SCENARIO("Session and login API enforces authentication and provides accurate session information")
{
    GIVEN("a running server")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);

        WHEN("an unauthenticated client calls GET /api/session")
        {
            httplib::Client unauth("http://127.0.0.1:" + std::to_string(fx.config.api_port));
            unauth.set_connection_timeout(5, 0);
            unauth.set_read_timeout(5, 0);
            std::optional<int> status;

            THEN("the server returns 401")
            {
                REQUIRE(eventually(
                    [&] {
                        const auto res = unauth.Get("/api/session");
                        if (!res)
                            return false;
                        status = res->status;
                        return true;
                    },
                    5s));
                REQUIRE(status.has_value());
                REQUIRE(*status == 401);
            }
        }

        WHEN("the admin logs in with correct credentials")
        {
            REQUIRE(fx.login());

            THEN("GET /api/session returns username and role")
            {
                const auto session = get_json(fx.client, "/api/session");
                REQUIRE(session.has_value());
                REQUIRE((*session)["username"].get<std::string>() == "thewatcher");
                REQUIRE((*session)["role"].get<std::string>() == "global_admin");
            }

            AND_WHEN("the admin logs out")
            {
                REQUIRE(post_ok(fx.client, "/api/logout"));

                THEN("GET /api/session returns 401")
                {
                    const auto res = fx.client.Get("/api/session");
                    REQUIRE(res);
                    REQUIRE(res->status == 401);
                }
            }

            AND_WHEN("a browser sends an unsafe authenticated request from another origin")
            {
                httplib::Headers headers{
                    {"Origin", "http://evil.example"}
                };
                const auto res =
                    fx.client.Post("/api/groups", headers, R"({"name":"Cross Origin"})", "application/json");

                THEN("the server rejects the request before it changes state")
                {
                    REQUIRE(res);
                    REQUIRE(res->status == 403);

                    const auto groups = get_json(fx.client, "/api/groups");
                    REQUIRE(groups.has_value());
                    for (const auto& group : *groups)
                    {
                        REQUIRE(group["name"].get<std::string>() != "Cross Origin");
                    }
                }
            }

            AND_WHEN("the same authenticated request comes from the API host origin")
            {
                httplib::Headers headers{
                    {"Origin", "http://127.0.0.1:" + std::to_string(fx.config.api_port)}
                };
                const auto res =
                    fx.client.Post("/api/groups", headers, R"({"name":"Same Origin"})", "application/json");

                THEN("the server allows it")
                {
                    REQUIRE(res);
                    REQUIRE(res->status == 200);
                }
            }

            AND_WHEN("a proxied HTTPS request uses an origin with an implicit default port")
            {
                httplib::Headers headers{
                    {"Host",              "watcher.example.com:443"    },
                    {"Origin",            "https://watcher.example.com"},
                    {"X-Forwarded-Proto", "https"                      }
                };
                const auto res =
                    fx.client.Post("/api/groups", headers, R"({"name":"Default Port Origin"})", "application/json");

                THEN("the server normalizes the default HTTPS port and allows it")
                {
                    REQUIRE(res);
                    REQUIRE(res->status == 200);
                }
            }

            AND_WHEN("a proxied HTTPS request receives an HTTP origin on the same host")
            {
                httplib::Headers headers{
                    {"Host",              "watcher.example.com"       },
                    {"Origin",            "http://watcher.example.com"},
                    {"X-Forwarded-Proto", "https"                     }
                };
                const auto res =
                    fx.client.Post("/api/groups", headers, R"({"name":"Wrong Scheme Origin"})", "application/json");

                THEN("the server rejects it as a different browser origin")
                {
                    REQUIRE(res);
                    REQUIRE(res->status == 403);
                }
            }

            AND_WHEN("the authenticated request comes from the dashboard on the same loopback host")
            {
                httplib::Headers headers{
                    {"Origin", "http://localhost:5173"}
                };
                const auto res =
                    fx.client.Post("/api/groups", headers, R"({"name":"Dashboard Origin"})", "application/json");

                THEN("the server allows it and emits credentialed CORS headers")
                {
                    REQUIRE(res);
                    REQUIRE(res->status == 200);
                    REQUIRE(res->get_header_value("Access-Control-Allow-Origin") == "http://localhost:5173");
                    REQUIRE(res->get_header_value("Access-Control-Allow-Credentials") == "true");
                }
            }
        }

        WHEN("login is attempted with the wrong password")
        {
            httplib::Client bad("http://127.0.0.1:" + std::to_string(fx.config.api_port));
            bad.set_connection_timeout(5, 0);
            bad.set_read_timeout(5, 0);
            const auto res = bad.Post("/api/login",
                                      json{
                                          {"username", "thewatcher"   },
                                          {"password", "wrongpassword"}
            }
                                          .dump(),
                                      "application/json");

            THEN("the server returns 401")
            {
                REQUIRE(res);
                REQUIRE(res->status == 401);
            }
        }

        WHEN("login is attempted with a non-existent username")
        {
            httplib::Client bad("http://127.0.0.1:" + std::to_string(fx.config.api_port));
            bad.set_connection_timeout(5, 0);
            bad.set_read_timeout(5, 0);
            const auto res = bad.Post("/api/login",
                                      json{
                                          {"username", "nobody"},
                                          {"password", "pw"    }
            }
                                          .dump(),
                                      "application/json");

            THEN("the server returns 401")
            {
                REQUIRE(res);
                REQUIRE(res->status == 401);
            }
        }
    }
}

// ── Enrollment rejection + agent config APIs ──────────────────────────────────

SCENARIO(
    "Pending enrollment rejection removes the agent from the queue and approved-agent config APIs function correctly")
{
    GIVEN("a running server with an authenticated admin")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        thewatcher::crypto::init();
        const auto agent_keys = thewatcher::crypto::generate_curve_keypair();
        const std::string agent_id = "cfg-agent-" + std::to_string(now_ms());

        thewatcher::agent::AgentConfig agent_cfg;
        agent_cfg.agent_id = agent_id;
        agent_cfg.enrollment_address = fx.config.enrollment_address;
        agent_cfg.agent_public_key = agent_keys.public_key_z85;
        agent_cfg.agent_secret_key = agent_keys.secret_key_z85;
        agent_cfg.collection_interval = 30;
        agent_cfg.process_limit = 5;

        WHEN("the agent sends an enrollment request")
        {
            const auto pending_resp = send_enrollment_request(agent_cfg);
            REQUIRE_FALSE(pending_resp.approved);
            REQUIRE(pending_resp.message == "pending approval");

            THEN("the agent appears in GET /api/pending-enrollments with the correct fields")
            {
                REQUIRE(eventually(
                    [&] {
                        return find_pending_agent(fx.client, agent_id).has_value();
                    },
                    5s));

                const auto pending = find_pending_agent(fx.client, agent_id);
                REQUIRE(pending.has_value());
                REQUIRE((*pending)["agent_id"].get<std::string>() == agent_id);
                REQUIRE((*pending)["approved"].get<bool>() == false);
                REQUIRE((*pending)["rejected"].get<bool>() == false);
                REQUIRE((*pending)["hostname"].get<std::string>() == "integration-host");
            }

            AND_WHEN("the admin rejects the enrollment")
            {
                REQUIRE(eventually(
                    [&] {
                        return find_pending_agent(fx.client, agent_id).has_value();
                    },
                    5s));

                httplib::Headers headers{
                    {"Origin", "http://localhost:5173"}
                };
                const auto res = fx.client.Post("/api/agents/" + agent_id + "/reject", headers, "", "application/json");
                REQUIRE(res);
                REQUIRE(res->status == 200);
                REQUIRE(res->get_header_value("Access-Control-Allow-Origin") == "http://localhost:5173");

                THEN("the agent no longer appears in the pending-enrollments list")
                {
                    REQUIRE_FALSE(find_pending_agent(fx.client, agent_id).has_value());
                }
            }
        }

        WHEN("a second agent is enrolled, approved, and its configuration is updated via the API")
        {
            const auto agent_keys2 = thewatcher::crypto::generate_curve_keypair();
            const std::string agent_id2 = "cfg-agent-b-" + std::to_string(now_ms() + 1);

            thewatcher::agent::AgentConfig agent_cfg2;
            agent_cfg2.agent_id = agent_id2;
            agent_cfg2.enrollment_address = fx.config.enrollment_address;
            agent_cfg2.agent_public_key = agent_keys2.public_key_z85;
            agent_cfg2.agent_secret_key = agent_keys2.secret_key_z85;
            agent_cfg2.collection_interval = 30;
            agent_cfg2.process_limit = 5;

            send_enrollment_request(agent_cfg2);
            REQUIRE(eventually(
                [&] {
                    return find_pending_agent(fx.client, agent_id2).has_value();
                },
                5s));
            httplib::Headers headers{
                {"Origin", "http://localhost:5173"}
            };
            const auto approve_res = fx.client.Post("/api/agents/" + agent_id2 + "/approve", headers,
                                                    R"({"group_ids":[]})", "application/json");
            REQUIRE(approve_res);
            REQUIRE(approve_res->status == 200);
            REQUIRE(approve_res->get_header_value("Access-Control-Allow-Origin") == "http://localhost:5173");
            REQUIRE(eventually(
                [&] {
                    return find_agent(fx.client, agent_id2).has_value();
                },
                5s));

            THEN("the admin can set the agent's description and it is reflected in GET /api/agents")
            {
                const auto res = post_json(fx.client, "/api/agents/" + agent_id2 + "/description",
                                           {
                                               {"description", "Primary integration-test agent"}
                });
                REQUIRE(res.has_value());
                REQUIRE((*res)["ok"].get<bool>() == true);

                const auto a = find_agent(fx.client, agent_id2);
                REQUIRE(a.has_value());
                REQUIRE((*a)["description"].get<std::string>() == "Primary integration-test agent");
            }

            AND_THEN("the admin can update per-agent thresholds and the server acknowledges ok")
            {
                const auto res = post_json(fx.client, "/api/agents/" + agent_id2 + "/thresholds",
                                           {
                                               {"thresholds",
                                                {
                                                    {"cpu",
                                                     {{"warning_pct_of_avg", 110.0},
                                                      {"degraded_pct_of_avg", 130.0},
                                                      {"critical_pct_of_avg", 150.0}}},
                                                    {"memory",
                                                     {{"warning_pct_of_avg", 110.0},
                                                      {"degraded_pct_of_avg", 130.0},
                                                      {"critical_pct_of_avg", 150.0}}},
                                                    {"disk",
                                                     {{"warning_pct_of_avg", 110.0},
                                                      {"degraded_pct_of_avg", 130.0},
                                                      {"critical_pct_of_avg", 150.0}}},
                                                    {"network",
                                                     {{"warning_pct_of_avg", 110.0},
                                                      {"degraded_pct_of_avg", 130.0},
                                                      {"critical_pct_of_avg", 150.0}}},
                                                }},
                });
                REQUIRE(res.has_value());
                REQUIRE((*res)["ok"].get<bool>() == true);
            }

            AND_THEN("submitting invalid thresholds (warning >= degraded) returns 400")
            {
                const auto res = post_json(fx.client, "/api/agents/" + agent_id2 + "/thresholds",
                                           {
                                               {"thresholds",
                                                {
                                                    {"cpu",
                                                     {{"warning_pct_of_avg", 150.0},
                                                      {"degraded_pct_of_avg", 110.0},
                                                      {"critical_pct_of_avg", 200.0}}},
                                                    {"memory",
                                                     {{"warning_pct_of_avg", 110.0},
                                                      {"degraded_pct_of_avg", 130.0},
                                                      {"critical_pct_of_avg", 150.0}}},
                                                    {"disk",
                                                     {{"warning_pct_of_avg", 110.0},
                                                      {"degraded_pct_of_avg", 130.0},
                                                      {"critical_pct_of_avg", 150.0}}},
                                                    {"network",
                                                     {{"warning_pct_of_avg", 110.0},
                                                      {"degraded_pct_of_avg", 130.0},
                                                      {"critical_pct_of_avg", 150.0}}},
                                                }},
                });
                REQUIRE_FALSE(res.has_value());
            }

            AND_THEN("GET /api/uptime/:id returns a well-formed response with zero samples")
            {
                const auto uptime = get_json(fx.client, "/api/uptime/" + agent_id2);
                REQUIRE(uptime.has_value());
                REQUIRE((*uptime)["agent_id"].get<std::string>() == agent_id2);
                REQUIRE(uptime->contains("uptime_percent"));
                REQUIRE(uptime->contains("actual_samples"));
                REQUIRE(uptime->contains("expected_samples"));
                REQUIRE((*uptime)["actual_samples"].get<int64_t>() == 0);
            }

            AND_THEN("GET /api/uptime for a non-existent agent returns 404")
            {
                const auto res = fx.client.Get("/api/uptime/does-not-exist");
                REQUIRE(res);
                REQUIRE(res->status == 404);
            }

            AND_THEN("GET /api/agents/:id/log-matches returns an empty array for an agent with no log data")
            {
                const auto matches = get_json(fx.client, "/api/agents/" + agent_id2 + "/log-matches");
                REQUIRE(matches.has_value());
                REQUIRE(matches->is_array());
                REQUIRE(matches->empty());
            }

            AND_THEN("DELETE /api/agents/:id removes the agent from GET /api/agents")
            {
                REQUIRE(delete_ok(fx.client, "/api/agents/" + agent_id2));
                REQUIRE_FALSE(find_agent(fx.client, agent_id2).has_value());
            }
        }
    }
}

// ── Scenario 11: Runbooks API ─────────────────────────────────────────────────

SCENARIO("Runbooks API supports CRUD and enforces role-based access")
{
    GIVEN("a running server with the default admin user logged in")
    {
        ServerFixture fx;
        REQUIRE(fx.startup.wait().started);
        REQUIRE(fx.login());

        WHEN("GET /api/runbooks is called on an empty store")
        {
            const auto list = get_json(fx.client, "/api/runbooks");

            THEN("it returns an empty array")
            {
                REQUIRE(list.has_value());
                REQUIRE(list->is_array());
                REQUIRE(list->empty());
            }
        }

        WHEN("POST /api/runbooks creates a cpu/red runbook")
        {
            const auto created = post_json_created(fx.client, "/api/runbooks",
                                                   {
                                                       {"indicator", "cpu"                                 },
                                                       {"status",    "red"                                 },
                                                       {"url",       "https://wiki.example.com/cpu-runbook"},
                                                       {"notes",     "Check top processes"                 },
            });

            THEN("a 201 response is returned with the runbook record")
            {
                REQUIRE(created.has_value());
                REQUIRE((*created)["runbook_id"].get<int64_t>() > 0);
                REQUIRE((*created)["indicator"].get<std::string>() == "cpu");
                REQUIRE((*created)["status"].get<std::string>() == "red");
                REQUIRE((*created)["url"].get<std::string>() == "https://wiki.example.com/cpu-runbook");
                REQUIRE((*created)["notes"].get<std::string>() == "Check top processes");
            }

            AND_THEN("GET /api/runbooks lists the new runbook")
            {
                const auto list = get_json(fx.client, "/api/runbooks");
                REQUIRE(list.has_value());
                REQUIRE(list->size() == 1);
                REQUIRE(list->at(0)["indicator"].get<std::string>() == "cpu");
            }

            AND_WHEN("DELETE /api/runbooks/:id removes it")
            {
                const auto runbook_id = (*created)["runbook_id"].get<int64_t>();
                REQUIRE(delete_ok(fx.client, "/api/runbooks/" + std::to_string(runbook_id)));

                THEN("GET /api/runbooks is empty again")
                {
                    const auto list = get_json(fx.client, "/api/runbooks");
                    REQUIRE(list.has_value());
                    REQUIRE(list->empty());
                }
            }
        }

        WHEN("POST /api/runbooks is called with an invalid status")
        {
            const auto res =
                fx.client.Post("/api/runbooks",
                               json{
                                   {"indicator", "cpu"                },
                                   {"status",    "purple"             },
                                   {"url",       "https://example.com"}
            }
                                   .dump(),
                               "application/json");

            THEN("a 400 error is returned")
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
            }
        }

        WHEN("POST /api/runbooks is called with a missing url")
        {
            const auto res = fx.client.Post("/api/runbooks",
                                            json{
                                                {"indicator", "cpu"},
                                                {"status",    "red"}
            }
                                                .dump(),
                                            "application/json");

            THEN("a 400 error is returned")
            {
                REQUIRE(res);
                REQUIRE(res->status == 400);
            }
        }

        WHEN("a viewer user tries to create a runbook")
        {
            const auto viewer_body = json{
                {"username", "viewer_rb"   },
                {"password", "viewerpw!"   },
                {"role",     "group_viewer"}
            };
            REQUIRE(post_json(fx.client, "/api/users", viewer_body).has_value());

            httplib::Client viewer("127.0.0.1", fx.config.api_port);
            REQUIRE(login_as(viewer, "viewer_rb", "viewerpw!"));

            const auto res = viewer.Post("/api/runbooks",
                                         json{
                                             {"indicator", "cpu"                },
                                             {"status",    "red"                },
                                             {"url",       "https://example.com"}
            }
                                             .dump(),
                                         "application/json");

            THEN("a 403 Forbidden response is returned")
            {
                REQUIRE(res);
                REQUIRE(res->status == 403);
            }

            AND_THEN("the viewer can still GET /api/runbooks (viewer+ access)")
            {
                const auto list = get_json(viewer, "/api/runbooks");
                REQUIRE(list.has_value());
                REQUIRE(list->is_array());
            }
        }

        WHEN("a group admin tries to mutate global runbooks")
        {
            const auto group = post_json(fx.client, "/api/groups",
                                         {
                                             {"name", "runbook-group"}
            });
            REQUIRE(group.has_value());
            const auto group_id = (*group)["group_id"].get<int64_t>();

            const auto user_body = json{
                {"username",  "group_admin_rb"       },
                {"password",  "adminpw!"             },
                {"role",      "group_admin"          },
                {"group_ids", json::array({group_id})}
            };
            REQUIRE(post_json(fx.client, "/api/users", user_body).has_value());

            const auto created = post_json_created(fx.client, "/api/runbooks",
                                                   {
                                                       {"indicator", "cpu"                                 },
                                                       {"status",    "red"                                 },
                                                       {"url",       "https://wiki.example.com/cpu-runbook"},
            });
            REQUIRE(created.has_value());
            const auto runbook_id = (*created)["runbook_id"].get<int64_t>();

            httplib::Client group_admin("127.0.0.1", fx.config.api_port);
            REQUIRE(login_as(group_admin, "group_admin_rb", "adminpw!"));

            const auto create_res =
                group_admin.Post("/api/runbooks",
                                 json{
                                     {"indicator", "memory"             },
                                     {"status",    "red"                },
                                     {"url",       "https://example.com"}
            }
                                     .dump(),
                                 "application/json");
            const auto delete_res = group_admin.Delete("/api/runbooks/" + std::to_string(runbook_id));

            THEN("the server rejects create and delete with 403")
            {
                REQUIRE(create_res);
                REQUIRE(create_res->status == 403);
                REQUIRE(delete_res);
                REQUIRE(delete_res->status == 403);
            }
        }
    }
}
