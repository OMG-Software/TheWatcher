#pragma once

#include "common/commands.hpp"
#include "store.hpp"
#include "zap_handler.hpp"

#include <array>
#include <atomic>
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

// A trusted-proxy network: address bytes (IPv4 as IPv4-mapped IPv6), family and the
// number of leading bits that must match.
struct ProxyRange
{
    std::array<unsigned char, 16> bytes{};
    int prefix_bits = 128;
    bool is_v4 = false; // address family; a range only matches its own family
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
    // loop on a worker thread. Throws std::runtime_error if the bind fails, so
    // Server::run() reports a startup failure instead of a running server.
    void start();

    void stop();

    // Addresses or CIDR ranges (e.g. "127.0.0.1", "::1", "10.0.0.0/8") of
    // reverse proxies whose X-Forwarded-For / X-Real-IP headers are trusted
    // for login throttling. Empty (the default) means the headers are
    // ignored and the TCP peer address is always used. Invalid entries are
    // skipped with a warning.
    void set_trusted_proxies(std::vector<std::string> proxies);

    // Test seam, call before start(): replaces the clock used by the login
    // throttle and installs a hook that runs right after an attempt's slot is
    // counted (with the username), so tests can drive window expiry without
    // sleeping. Both are empty in production.
    void set_login_test_hooks(std::function<int64_t()> clock,
                              std::function<void(const std::string&)> after_reservation);

    // Test seam, call before start(): runs on the request thread with the
    // user id after a self-service password change verified the current
    // password and before the store applies the change, so a test can land an
    // admin reset exactly in that window. Empty in production.
    void set_password_change_test_hook(std::function<void(int64_t)> after_current_password_verified);

    // Deletes expired sessions when at least five minutes passed since the last
    // successful purge (a failed purge is retried on the next call). Safe to
    // call from any thread. Requests call it opportunistically; the server's
    // periodic job calls it too, so an idle server still purges.
    void purge_expired_sessions_if_due(int64_t now_ms);

    // Decides whether an agent can receive a command right now. Without a
    // check the persisted `connected` flag is used. The server installs a
    // check backed by its live routes, because after a restart an agent can
    // still be stored as connected with no route. Call before start().
    using ConnectivityCheck = std::function<bool(const std::string& agent_id)>;
    void set_connectivity_check(ConnectivityCheck check);

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
    // True when a command sent to agent_id now can be delivered.
    bool agent_reachable(const std::string& agent_id);
    // Command admission (#54): call first in every route that queues a command,
    // before anything is changed or queued. Replies 409 and returns false when
    // the agent cannot receive commands.
    bool admit_command(const std::string& agent_id, httplib::Response& res);
    std::string enqueue_simple_command(const std::string& id, CommandType ct);
    // Client address used for login throttling: honours X-Real-IP /
    // X-Forwarded-For only when the TCP peer is a trusted proxy. For a trusted
    // peer a present but malformed forwarded value yields "unknown-client"
    // (its own bucket); the peer address is used when the peer is not trusted
    // or no forwarding header was sent.
    std::string client_ip(const httplib::Request& req);
    // Writes an AUDIT line (NOTICE) for an authentication or authorization
    // event. user/target are sanitised; detail must not contain secrets.
    void audit(const char* event, const char* outcome, const std::string& user, const std::string& source,
               const std::string& target = "") const;
    // RAII hold on one login attempt's slot in the three throttle counters.
    // arm() is called once the slot was counted (under login_mutex_). The
    // destructor gives the slot back unless keep() recorded a genuine
    // credential failure, so a success or an exception never consumes a slot
    // and never touches other in-flight attempts' slots.
    class LoginReservation
    {
    public:
        LoginReservation(ApiServer& api, std::string pair_key, std::string username, std::string remote);
        LoginReservation(const LoginReservation&) = delete;
        LoginReservation& operator=(const LoginReservation&) = delete;
        ~LoginReservation();
        // Records the window each counter entry belonged to when this attempt
        // was counted. release() then touches an entry only while it is still
        // that window's entry: pruning may have dropped it and a newer attempt
        // recreated the key, and that newer slot must not be decremented.
        void arm(int64_t pair_window, int64_t account_window, int64_t ip_window) noexcept
        {
            armed_ = true;
            pair_window_ = pair_window;
            account_window_ = account_window;
            ip_window_ = ip_window;
        }
        void keep() noexcept
        {
            armed_ = false;
        }
        void release();

    private:
        ApiServer& api_;
        std::string pair_key_;
        std::string username_;
        std::string remote_;
        bool armed_ = false;
        int64_t pair_window_ = 0;
        int64_t account_window_ = 0;
        int64_t ip_window_ = 0;
    };

    // Why a credential check was refused by the throttle (HTTP 429).
    struct ThrottleDenial
    {
        const char* message;
        const char* retry_after; // seconds, for the Retry-After header
    };
    // Counts one credential check against the three throttle windows, keyed as
    // login keys them (pair_key = "<ip>|<username>", username, ip), or returns
    // why it is refused. On admission the slot is armed on `reservation`.
    // Shared by login and the current-password check of a password change.
    std::optional<ThrottleDenial> reserve_login_attempt(LoginReservation& reservation, const std::string& pair_key,
                                                        const std::string& username, const std::string& remote,
                                                        int64_t now);

    Store& store_;
    ZapHandler& zap_;
    std::string host_;
    int port_;

    std::unique_ptr<httplib::Server> http_;
    std::thread thread_;

    ConnectivityCheck connectivity_check_;
    std::queue<PendingCommand> cmd_queue_;
    std::mutex cmd_mutex_;

    // Login failure tracker for rate limiting (M-1): (client_ip, username) -> (fail_count, window_start_ms)
    std::unordered_map<std::string, std::pair<int, int64_t>> login_failures_;
    // Account-wide failure tracker keyed by username alone (high threshold):
    // username -> (fail_count, window_start_ms)
    std::unordered_map<std::string, std::pair<int, int64_t>> login_account_failures_;
    std::vector<ProxyRange> trusted_proxies_; // guarded by login_mutex_
    // SEC-12: per-source-IP failure throttle: ip -> (fail_count, window_start_ms)
    std::unordered_map<std::string, std::pair<int, int64_t>> login_ip_failures_;
    int64_t login_lockout_ms_;
    int64_t login_ip_window_ms_;
    std::mutex login_mutex_;
    // Argon2 hash of a throwaway password; verified when a login names an
    // unknown or disabled user so those paths cost the same as a real one.
    std::string dummy_password_hash_;
    std::atomic<int64_t> last_session_purge_ms_{0};  // advanced only after a successful purge
    std::atomic<bool> session_purge_running_{false}; // at most one purge in flight
    std::function<int64_t()> login_clock_;
    std::function<void(const std::string&)> after_reservation_hook_;
    std::function<void(int64_t)> password_change_hook_;
};

} // namespace thewatcher::server
