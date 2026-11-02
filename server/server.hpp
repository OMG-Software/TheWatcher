#pragma once

#include "api.hpp"
#include "common/protocol.hpp"
#include "config.hpp"
#include "startup_signal.hpp"
#include "store.hpp"
#include "zap_handler.hpp"

#include <atomic>
#include <memory>
#include <unordered_map>

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
    // Each sent command is recorded with its target agent (SEC-04). A send
    // failure is logged and that command is dropped.
    void dispatch_commands(zmq::socket_t& router);

    // Send a scheduled digest report if one is due.
    void maybe_send_report(int64_t now_ms);

    ServerConfig config_;
    zmq::context_t ctx_{2}; // 2 IO threads
    std::unique_ptr<Store> store_;
    std::unique_ptr<ZapHandler> zap_;
    std::unique_ptr<ApiServer> api_;
    // command_id -> dispatched command; evicted on matching ACK or TTL expiry
    std::unordered_map<std::string, DispatchedCommand> dispatched_commands_;
    // agent_id -> last enrollment request time (ms); used for rate limiting
    std::unordered_map<std::string, int64_t> enrollment_last_request_ms_;
    // SEC-05: peer address -> last enrollment request time (ms); a second,
    // milder rate limit keyed by connection source instead of the
    // attacker-chosen agent_id.
    std::unordered_map<std::string, int64_t> enrollment_peer_last_request_ms_;
    std::atomic<bool> running_{false};
    int64_t last_report_ms_ = 0;
};

} // namespace thewatcher::server
