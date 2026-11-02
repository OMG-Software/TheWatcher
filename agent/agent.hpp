#pragma once

#include "collectors/collector.hpp"
#include "collectors/log_collector.hpp"
#include "common/commands.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "config.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <queue>
#include <stop_token> // C++20: std::stop_token, std::stop_source
#include <string>
#include <thread> // C++20: std::jthread
#include <vector>

#include <zmq.hpp>

namespace thewatcher::agent
{

class Agent
{
public:
    // Constructor now initializes collectors and starts threads.
    explicit Agent(AgentConfig config);
    ~Agent();

    // Enrolls with the server, then configures the DEALER socket and starts the
    // IO, collection and heartbeat threads. Returns without connecting if
    // enrollment is stopped first. Throws if enrollment fails (SEC-08). Persists
    // the enrolled key pin to the config file. Applies SEC-02 socket limits and
    // uses CURVE only when a server key is known.
    void start(); // Now responsible for starting the background loops.
    void stop();  // Signals all threads to stop.

private:
    // ── ZMQ IO thread (owns dealer_) ────────────────────────────────────────
    void io_loop(std::stop_token st);

    // ── Metric collection thread ─────────────────────────────────────────────
    void collection_loop(std::stop_token st);

    // ── Heartbeat thread ─────────────────────────────────────────────────────
    void heartbeat_loop(std::stop_token st);

    // ── Command handlers (called from IO thread) ─────────────────────────────
    void handle_frame(const proto::Frame& f);
    // Runs one server command (interval, process limit, restart, pause, resume,
    // status, disconnect) and sends a COMMAND_ACK. An unknown command type is
    // acked with ok=false. Restarting collectors re-applies the SEC-10 log
    // allowlist to the new LogCollector.
    void handle_command(const CommandMessage& cmd);

    // ── Helpers ───────────────────────────────────────────────────────────────
    void enqueue(std::vector<uint8_t> encoded_frame);
    SystemMetrics collect_metrics();
    void fill_static_info(SystemMetrics& m) const;

    // ── State ─────────────────────────────────────────────────────────────────
    AgentConfig config_;
    zmq::context_t ctx_{1};
    zmq::socket_t dealer_{ctx_, ZMQ_DEALER};
    std::vector<std::unique_ptr<Collector>> collectors_;
    LogCollector* log_collector_ = nullptr; // raw pointer into collectors_, owned by collectors_

    // Outbox: frames queued by collection/heartbeat threads, drained by IO thread
    std::queue<std::vector<uint8_t>> outbox_;
    std::mutex outbox_mutex_;

    std::atomic<int> interval_seconds_{5}; // Default to 5 seconds
    std::atomic<int> heartbeat_interval_seconds_{5};
    std::atomic<int> process_limit_{100}; // Default limit of 100 processes
    CollectorConfig collector_config_;
    std::atomic<bool> enrollment_stop_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> connected_{false};

    // Cached static system info
    std::string os_name_;
    std::string os_version_;
    std::string hostname_;
    std::string platform_;

    std::jthread io_thread_;
    std::jthread collection_thread_;
    std::jthread heartbeat_thread_;
};

} // namespace thewatcher::agent
