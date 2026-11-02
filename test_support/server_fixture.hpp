// A running Server on isolated local endpoints with its own scratch database
// directory and an HTTP client for the REST API.
#pragma once

#include "common/crypto.hpp"
#include "test_support/enrollment_client.hpp"
#include "test_support/net_helpers.hpp"
#include "test_support/server_runner.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <utility>

#include <httplib.h>

namespace thewatcher::test
{

// POSTs credentials to /api/login and, on success, stores the session cookie
// as a default header on `client`.
inline bool login_as(httplib::Client& client, const std::string& username, const std::string& password)
{
    auto res = client.Post("/api/login",
                           nlohmann::json{
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

inline bool login_as_default_admin(httplib::Client& client)
{
    return login_as(client, "thewatcher", "look_at_me");
}

struct ServerFixtureOptions
{
    // Generate a fresh CURVE keypair for the data plane (otherwise CURVE is off).
    bool curve = false;
    // Pre-shared enrollment token (empty = none required).
    std::string enrollment_token;

    static ServerFixtureOptions with_curve(bool enabled = true)
    {
        ServerFixtureOptions options;
        options.curve = enabled;
        return options;
    }

    static ServerFixtureOptions with_enrollment_token(std::string token)
    {
        ServerFixtureOptions options;
        options.enrollment_token = std::move(token);
        return options;
    }
};

// The scratch directory is a private base so it is created before, and
// removed after, the server that stores its database in it.
class ServerFixture : private ScopedTempDir, public ServerRunner
{
public:
    explicit ServerFixture(const ServerFixtureOptions& options = {})
        : ScopedTempDir("thewatcher-server-test")
        , ServerRunner(make_config(ScopedTempDir::path(), options))
        , client("http://127.0.0.1:" + std::to_string(config.api_port))
    {
        client.set_connection_timeout(5, 0);
        client.set_read_timeout(5, 0);
    }

    bool login(const std::string& username, const std::string& password)
    {
        return login_as(client, username, password);
    }

    // Logs in as the built-in admin, retrying while the server comes up (or
    // while a busy host makes the Argon2 verification slow).
    bool login(std::chrono::milliseconds timeout = std::chrono::seconds(20))
    {
        return eventually(
            [this] {
                return login_as_default_admin(client);
            },
            timeout);
    }

    // Asserts the server came up cleanly, then logs in as the default admin.
    void start_and_login()
    {
        REQUIRE(startup.wait().started);
        REQUIRE(error == nullptr);
        REQUIRE(login());
    }

    // Enrolls `agent_id` over the enrollment socket with a fresh keypair.
    // Returns false when the server does not reply.
    bool enroll_agent(const std::string& agent_id, const std::string& hostname = "enroll-test-host")
    {
        crypto::init();
        const auto keys = crypto::generate_curve_keypair();
        return exchange_enrollment(config.enrollment_address,
                                   encode_enrollment(make_enroll_request(agent_id, keys.public_key_z85, hostname)))
            .has_value();
    }

    // Enrolls `agent_id` and approves it (optionally with a JSON request body).
    bool enroll_and_approve(const std::string& agent_id, const std::string& hostname = "enroll-test-host",
                            const std::string& approve_body = "")
    {
        return enroll_agent(agent_id, hostname) && approve(agent_id, approve_body);
    }

    bool approve(const std::string& agent_id, const std::string& body = "")
    {
        auto res = client.Post("/api/agents/" + agent_id + "/approve", body, "application/json");
        return res && res->status == 200;
    }

    bool reject(const std::string& agent_id)
    {
        auto res = client.Post("/api/agents/" + agent_id + "/reject", "", "application/json");
        return res && res->status == 200;
    }

    httplib::Client client;

private:
    static thewatcher::server::ServerConfig make_config(const std::filesystem::path& dir,
                                                        const ServerFixtureOptions& options)
    {
        crypto::init();
        thewatcher::server::ServerConfig cfg;
        cfg.bind_address = free_zmq_endpoint();
        cfg.enrollment_address = free_zmq_endpoint();
        cfg.api_host = "127.0.0.1";
        cfg.api_port = free_http_port();
        cfg.db_path = (dir / "thewatcher.db").string();
        if (options.curve)
        {
            const auto keys = crypto::generate_curve_keypair();
            cfg.server_public_key = keys.public_key_z85;
            cfg.server_secret_key = keys.secret_key_z85;
        }
        cfg.enrollment_token = options.enrollment_token;
        return cfg;
    }
};

} // namespace thewatcher::test
