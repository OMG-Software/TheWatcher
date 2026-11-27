// An ApiServer on an in-memory store with one user and one ready-made session
// per role, so tests can call the REST API as any role without paying for an
// Argon2 login each time.
#pragma once

#include "server/api.hpp"
#include "server/store.hpp"
#include "server/zap_handler.hpp"
#include "test_support/net_helpers.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <httplib.h>
#include <zmq.hpp>

namespace thewatcher::test
{

// What a test needs to know about a response.
struct ApiReply
{
    int status = 0;
    nlohmann::json body;
};

class ApiFixture
{
public:
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

    ApiFixture(const ApiFixture&) = delete;
    ApiFixture& operator=(const ApiFixture&) = delete;

    // Creates group `name` and returns its id.
    int64_t add_group(const std::string& name)
    {
        return store->create_group(name);
    }

    // Creates a user with `role` in `groups` and a session for it. The user is
    // addressed later by `key` (any label the test likes). Returns the user id.
    int64_t add_user(const std::string& key, const std::string& role, const std::vector<int64_t>& groups = {})
    {
        const auto user_id = store->create_user(key, "not-a-real-hash", role);
        store->set_user_groups(user_id, groups);
        thewatcher::server::SessionRecord session;
        session.token = "token-" + key;
        session.user_id = user_id;
        session.username = key;
        session.role = role;
        session.created_at = now_ms();
        session.expires_at = session.created_at + 3600LL * 1000LL;
        store->create_session(session);
        return user_id;
    }

    // Stores an agent. `groups` is only applied when the agent is approved.
    void add_agent(const std::string& agent_id, bool approved, const std::vector<int64_t>& groups = {})
    {
        thewatcher::server::AgentRecord rec;
        rec.agent_id = agent_id;
        rec.hostname = agent_id + "-host";
        rec.platform = "linux";
        rec.approved = approved;
        store->upsert_agent(rec);
        if (!groups.empty())
            store->set_agent_groups(agent_id, groups);
    }

    // Sends a request as the user added under `key`; an empty key sends no session.
    ApiReply request(const std::string& method, const std::string& path, const std::string& as,
                     const nlohmann::json& body = nullptr)
    {
        httplib::Headers headers;
        if (!as.empty())
            headers.emplace("Cookie", "tw_session=token-" + as);
        const auto payload = body.is_null() ? std::string() : body.dump();

        httplib::Result res;
        if (method == "GET")
            res = client.Get(path, headers);
        else if (method == "POST")
            res = client.Post(path, headers, payload, "application/json");
        else if (method == "PUT")
            res = client.Put(path, headers, payload, "application/json");
        else if (method == "DELETE")
            res = client.Delete(path, headers, payload, "application/json");

        ApiReply reply;
        if (!res)
            return reply;
        reply.status = res->status;
        reply.body = nlohmann::json::parse(res->body, nullptr, false);
        return reply;
    }

    static int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Declared first so it is initialised before `api` uses it.
    int port = free_http_port();
    zmq::context_t ctx{1};
    std::unique_ptr<thewatcher::server::Store> store;
    thewatcher::server::ZapHandler zap{ctx};
    thewatcher::server::ApiServer api;
    httplib::Client client;
};

} // namespace thewatcher::test
