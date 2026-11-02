#pragma once

#include "common/commands.hpp"
#include "store.hpp"
#include "zap_handler.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace httplib
{
struct Request;
struct Response;
class Server;
} // namespace httplib

namespace thewatcher::server
{

struct PendingCommand
{
    std::string agent_id;
    CommandMessage cmd;
};

// REST API server (runs on its own thread via httplib).
// Commands posted to the API are placed in a thread-safe queue; the ZMQ
// server thread drains that queue and forwards them to agents.
//
// TLS note (I-4): this server uses httplib::Server (plain HTTP). All API
// traffic — including session cookies and login credentials — is sent
// unencrypted unless a TLS-terminating reverse proxy (e.g. nginx, Caddy)
// sits in front of it. For production deployments, place the API behind
// such a proxy and set the THEWATCHER_API_HOST binding to 127.0.0.1 so
// the plain-HTTP port is not exposed directly on the network.
class ApiServer
{
public:
    // login_lockout_ms / login_ip_window_ms configure the login rate
    // limiter windows; production uses the defaults, tests shrink them.
    ApiServer(Store& store, ZapHandler& zap, const std::string& host, int port,
              int64_t login_lockout_ms = 15LL * 60LL * 1000LL, int64_t login_ip_window_ms = 60LL * 1000LL);

    ~ApiServer();

    // Non-blocking start. Binds the port before returning, then runs the accept
    // loop on a worker thread. If the bind fails it logs an error and returns
    // without starting the thread; it does not throw.
    void start();

    void stop();

    // Addresses (exact remote_addr strings, e.g. "127.0.0.1" or "::1") of
    // reverse proxies whose X-Forwarded-For / X-Real-IP headers are trusted
    // for login throttling. Empty (the default) means the headers are
    // ignored and the TCP peer address is always used.
    void set_trusted_proxies(std::vector<std::string> proxies);

    // Drain pending commands into caller's vector (called from ZMQ thread).
    void drain_commands(std::vector<PendingCommand>& out);

private:
    void setup_routes();
    std::optional<SessionRecord> current_session(const httplib::Request& req);
    std::optional<SessionRecord> require_role(const httplib::Request& req, httplib::Response& res,
                                              const std::string& role);
    bool can_access_agent(const SessionRecord& session, const std::string& agent_id);
    std::optional<SessionRecord> require_agent_access(const httplib::Request& req, httplib::Response& res,
                                                      const std::string& role, const std::string& agent_id);
    std::string enqueue_simple_command(const std::string& id, CommandType ct);
    // Client address used for login throttling: honours X-Forwarded-For /
    // X-Real-IP only when the TCP peer is a trusted proxy. Falls back to the
    // peer address when the header value is not a plausible IP literal.
    std::string client_ip(const httplib::Request& req);

    Store& store_;
    ZapHandler& zap_;
    std::string host_;
    int port_;

    std::unique_ptr<httplib::Server> http_;
    std::thread thread_;

    std::queue<PendingCommand> cmd_queue_;
    std::mutex cmd_mutex_;

    // Login failure tracker for rate limiting (M-1): (client_ip, username) -> (fail_count, window_start_ms)
    std::unordered_map<std::string, std::pair<int, int64_t>> login_failures_;
    // Account-wide failure tracker keyed by username alone (high threshold):
    // username -> (fail_count, window_start_ms)
    std::unordered_map<std::string, std::pair<int, int64_t>> login_account_failures_;
    std::vector<std::string> trusted_proxies_; // guarded by login_mutex_
    // SEC-12: per-source-IP failure throttle: ip -> (fail_count, window_start_ms)
    std::unordered_map<std::string, std::pair<int, int64_t>> login_ip_failures_;
    int64_t login_lockout_ms_;
    int64_t login_ip_window_ms_;
    std::mutex login_mutex_;
};

} // namespace thewatcher::server
