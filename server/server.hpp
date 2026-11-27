#pragma once

#include "alert_pipeline.hpp"
#include "api.hpp"
#include "common/protocol.hpp"
#include "config.hpp"
#include "notification_worker.hpp"
#include "offline_alerts.hpp"
#include "startup_signal.hpp"
#include "store.hpp"
#include "zap_handler.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <zmq.hpp>

namespace thewatcher::server
{

// Orchestrates the ROUTER (metric/command data plane), enrollment REP socket,
// REST API, ZAP auth handler, and storage backend.
class Server
{
public:
    // Opens the store named by config.db_type ("postgres" or sqlite), creates
    // the ZAP handler and API server, and preloads the keys of approved agents
    // into ZAP, bound to their agent ids (SEC-04). Store errors propagate.
    explicit Server(ServerConfig config);
    ~Server();

    // Blocks until stop() is called or a fatal error occurs. Binds the ROUTER
    // (CURVE when a server key is set) and enrollment REP sockets, starts the
    // API, then polls them. SEC-01: a handler exception is logged and never
    // escapes the loop. SEC-02: message size and handshake time are bounded.
    // Calls startup->succeed() once ready. A socket bind failure throws.
    void run(StartupSignal* startup = nullptr);
    void stop();

private:
    // A command dispatched to a specific agent, awaiting its ACK.
    // SEC-04: target_agent_id lets the server verify that the ACK really
    // comes from the agent the command was sent to.
    struct DispatchedCommand
    {
        CommandType type;
        int64_t dispatched_at_ms;
        std::string target_agent_id;
    };

    // Drain one complete multipart message from the ROUTER and process it.
    // Undecodable frames are logged and dropped. SEC-04: with CURVE on, drops
    // frames whose ZAP identity differs from frame.agent_id. SEC-09: drops
    // frames from agents that are unknown, unapproved or rejected. SEC-07:
    // stores data under the server receive time, not the agent timestamp.
    void handle_router_frame(zmq::socket_t& router);

    // Drain one enrollment request from the REP socket and reply. Once a
    // request is read, every path sends a reply (SEC-01), so the REP socket
    // cannot stall. SEC-05: checks field formats, the enrollment token, per-agent
    // and per-peer rate limits, and a cap on pending agents. SEC-06: an existing
    // record's key is never replaced. The server key goes only to approved agents.
    void handle_enrollment(zmq::socket_t& enroll);

    // Pull commands out of the API queue and send them to agents via ROUTER.
    // Each sent command is recorded with its target agent (SEC-04). Commands
    // go to the agent's learned route (issue #141); a command for an agent
    // with no learned route, or whose send fails, is logged and dropped.
    void dispatch_commands(zmq::socket_t& router);

    // Issue #141: records that agent_id is reachable at routing_id (the
    // connection of a frame that passed the identity checks). A routing id
    // reaches at most one agent: another agent's route with the same id is
    // removed.
    void learn_route(const std::string& agent_id, const std::string& routing_id);
    // Forgets agent_id's route, if any.
    void forget_route(const std::string& agent_id);
    // Forgets routes with no frame for longer than offline_after_seconds, so
    // a closed connection's routing id cannot be reused by another peer to
    // receive that agent's commands.
    void expire_routes();
    // Rebuilds the API-visible set of agents that have a learned route.
    void publish_reachable_agents();
    // Thread-safe: true when agent_id has a learned route. Used by API threads.
    bool has_route(const std::string& agent_id) const;

    // Queue the scheduled digest report if one is due, and record the outcome
    // of earlier ones.
    void maybe_send_report(int64_t now_ms);

    ServerConfig config_;
    zmq::context_t ctx_{2}; // 2 IO threads
    // Outbound webhooks and report digests run on this worker's own thread so
    // a slow endpoint cannot stall the data plane (#48). Declared before the
    // pipeline that uses it so it is destroyed after it.
    NotificationWorker notifier_;
    std::unique_ptr<Store> store_;
    std::unique_ptr<ZapHandler> zap_;
    std::unique_ptr<ApiServer> api_;
    // The one path every alert takes (#52), and the offline-agent alerts that
    // use it (#156).
    std::unique_ptr<AlertPipeline> alerts_;
    std::unique_ptr<OfflineAlerts> offline_alerts_;
    // command_id -> dispatched command; evicted on matching ACK or TTL expiry
    std::unordered_map<std::string, DispatchedCommand> dispatched_commands_;
    // Issue #141: agent_id -> ROUTER routing id of the connection that last
    // sent an authenticated, approved frame for that agent, and when. Commands
    // go only to this learned route, never to a routing id equal to the
    // agent_id: the peer chooses its own routing id, so another agent could
    // claim it. route_owners_ is the reverse index (routing id -> agent_id).
    struct AgentRoute
    {
        std::string routing_id;
        std::chrono::steady_clock::time_point last_frame;
    };
    std::unordered_map<std::string, AgentRoute> agent_routes_;
    std::unordered_map<std::string, std::string> route_owners_;
    // Copy of agent_routes_ keys for the API threads (the maps above belong to
    // the server thread).
    mutable std::mutex reachable_mutex_;
    std::unordered_set<std::string> reachable_agents_;
    // agent_id -> last enrollment request time (ms); used for rate limiting
    std::unordered_map<std::string, int64_t> enrollment_last_request_ms_;
    // SEC-05: peer address -> last enrollment request time (ms); a second,
    // milder rate limit keyed by connection source instead of the
    // attacker-chosen agent_id.
    std::unordered_map<std::string, int64_t> enrollment_peer_last_request_ms_;
    std::atomic<bool> running_{false};
    // Scheduled digest delivery state (#49). The last-sent time itself is
    // persisted in the reports.last_sent_ms setting.
    bool report_in_flight_ = false;
    int report_failures_ = 0;
    int64_t next_report_attempt_ms_ = 0;
};

} // namespace thewatcher::server
