#pragma once

#include "collector_set.hpp"
#include "common/commands.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "config.hpp"
#include "outbox.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token> // C++20: std::stop_token, std::stop_source
#include <string>
#include <thread> // C++20: std::jthread
#include <variant>
#include <vector>

#include <zmq.hpp>

namespace thewatcher::agent
{

// Tunables that tests shorten; production uses the defaults.
struct AgentTimings
{
    // How often the agent asks the server for its configuration, independent
    // of the collection interval and of pause (issue #89).
    std::chrono::milliseconds config_poll_interval{10'000};
};

// Threading model (issues #83, #87, #89):
//  - the IO thread is the only user of the DEALER socket and never blocks:
//    sends are non-blocking and the outbox lock is never held across a zmq call;
//  - the collection thread is the sole owner of the collectors; the IO thread
//    posts CollectorRequests that it applies between collection passes;
//  - the heartbeat thread sends heartbeats and config requests on their own
//    timers, recomputing deadlines from the current settings each slice;
//  - one std::stop_source, created with the Agent and never reset, stops
//    everything, so a stop requested at any moment is never lost.
class Agent
{
public:
    // Throws std::invalid_argument if an interval or the process limit is out
    // of range (common/agent_limits.hpp).
    explicit Agent(AgentConfig config, AgentTimings timings = {});
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // Enrolls with the server, then configures the DEALER socket and starts the
    // IO, collection and heartbeat threads. Returns without connecting if a
    // stop was requested first (also during enrollment). Throws if enrollment
    // fails (SEC-08). Persists the enrolled key pin to the config file. Applies
    // SEC-02 socket limits and uses CURVE only when a server key is known.
    void start();

    // Requests a stop of enrollment and all threads. Safe from any thread,
    // including the agent's own IO thread; never joins. Threads are joined by
    // the destructor.
    void stop();

    // True once stop() was called, e.g. by a DISCONNECT command. The process
    // owner polls this to exit instead of idling.
    bool stop_requested() const noexcept;

    // True while the data-plane handshake with the server is complete.
    bool connected() const noexcept;

private:
    // Work posted by the IO thread for the collection thread to apply.
    struct ApplyCollectorConfig
    {
        int process_limit;
        CollectorConfig config;
    };
    struct SetProcessLimit
    {
        int limit;
    };
    struct RestartCollectors
    {
        std::string command_id;
    };
    struct CollectNow
    {
        std::string command_id;
    };
    using CollectorRequest = std::variant<ApplyCollectorConfig, SetProcessLimit, RestartCollectors, CollectNow>;

    void io_loop(std::stop_token st);
    void collection_loop(std::stop_token st);
    void heartbeat_loop(std::stop_token st);

    // Reads and routes the connection monitor events; flips connected_.
    void drain_monitor_events();
    // Sends queued frames without blocking. Returns false when the socket is
    // full (the unsent frame stays pending and is retried).
    bool flush_outbox();
    bool send_one(const Outbox::Bytes& payload);

    void handle_frame(const proto::Frame& f);
    // Runs one server command and queues a COMMAND_ACK. Out-of-range values
    // and unknown command types are acked with ok=false.
    void handle_command(const CommandMessage& cmd);
    void handle_config_update(const ConfigUpdate& update);

    void post_collector_request(CollectorRequest request);
    std::vector<CollectorRequest> take_collector_requests();
    void wake_collection();
    void wake_heartbeat();
    // Waits up to `grace` for queued frames to leave the socket (IO thread).
    void flush_for(std::chrono::milliseconds grace);
    void collect_and_send(CollectorSet& collectors);

    void enqueue(proto::FrameType type, std::vector<uint8_t> payload, FramePriority priority);
    void queue_ack(const std::string& command_id, bool ok, const std::string& message);
    void fill_static_info(SystemMetrics& m) const;

    AgentConfig config_;
    AgentTimings timings_;
    std::stop_source stop_source_;
    std::atomic<bool> enrollment_stop_{false}; // mirrors stop_source_ for enroll()

    zmq::context_t ctx_{1};
    zmq::socket_t dealer_{ctx_, ZMQ_DEALER};
    zmq::socket_t monitor_{ctx_, ZMQ_PAIR};
    bool monitor_active_ = false; // set in start() before the IO thread exists
    Outbox outbox_;
    std::optional<Outbox::Bytes> unsent_; // IO thread only: frame the socket refused

    std::atomic<int> interval_seconds_{5};
    std::atomic<int> heartbeat_interval_seconds_{5};
    std::atomic<int> process_limit_{100};
    std::atomic<bool> paused_{false};
    std::atomic<bool> connected_{false};

    std::mutex collection_mutex_;
    std::condition_variable_any collection_cv_;
    std::vector<CollectorRequest> collector_requests_;
    bool collection_wake_ = false;

    std::mutex heartbeat_mutex_;
    std::condition_variable_any heartbeat_cv_;
    bool heartbeat_wake_ = false;

    // Cached static system info
    std::string os_name_;
    std::string os_version_;
    std::string hostname_;
    std::string platform_;

    // Declared last so they are joined before anything they use is destroyed.
    std::jthread io_thread_;
    std::jthread collection_thread_;
    std::jthread heartbeat_thread_;
};

} // namespace thewatcher::agent
