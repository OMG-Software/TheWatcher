#include "agent.hpp"

#include "collectors/log_collector.hpp"
#include "common/SingleLog.hpp"
#include "common/agent_limits.hpp"
#include "common/zmq_limits.hpp"
#include "enrollment.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <utility>

namespace thewatcher::agent
{

namespace proto = thewatcher::proto; // file-scope alias is C++11-compatible

namespace
{
    const char* frame_type_name(uint8_t type)
    {
        switch (static_cast<proto::FrameType>(type))
        {
        case proto::FrameType::HEARTBEAT:
            return "HEARTBEAT";
        case proto::FrameType::METRICS:
            return "METRICS";
        case proto::FrameType::COMMAND:
            return "COMMAND";
        case proto::FrameType::COMMAND_ACK:
            return "COMMAND_ACK";
        case proto::FrameType::CONFIG_UPDATE:
            return "CONFIG_UPDATE";
        case proto::FrameType::ENROLL_REQUEST:
            return "ENROLL_REQUEST";
        case proto::FrameType::ENROLL_RESPONSE:
            return "ENROLL_RESPONSE";
        case proto::FrameType::CONFIG_REQUEST:
            return "CONFIG_REQUEST";
        case proto::FrameType::LOG_MATCH:
            return "LOG_MATCH";
        }
        return "UNKNOWN";
    }

    void log_metrics_summary(const char* prefix, const std::string& agent_id, const SystemMetrics& metrics)
    {
        LOGF_DEBUG("%s agent_id=%s host=%s platform=%s uptime_seconds=%.0f cpu_usage=%.2f memory_usage=%.2f disks=%zu "
                   "networks=%zu processes=%zu",
                   prefix, agent_id.c_str(), metrics.hostname.c_str(), metrics.platform.c_str(), metrics.uptime_seconds,
                   metrics.cpu.usage_percent, metrics.memory.usage_percent, metrics.disks.size(),
                   metrics.networks.size(), metrics.top_processes.size());
        if (!metrics.disks.empty())
        {
            const auto& disk = metrics.disks.front();
            LOGF_TRACE("First collected disk agent_id=%s mount=%s fs=%s usage=%.2f total_bytes=%llu used_bytes=%llu",
                       agent_id.c_str(), disk.mount_point.c_str(), disk.filesystem.c_str(), disk.usage_percent,
                       static_cast<unsigned long long>(disk.total_bytes),
                       static_cast<unsigned long long>(disk.used_bytes));
        }
        if (!metrics.networks.empty())
        {
            const auto& network = metrics.networks.front();
            LOGF_TRACE("First collected network agent_id=%s interface=%s up=%d recv_per_sec=%llu sent_per_sec=%llu",
                       agent_id.c_str(), network.interface_name.c_str(), network.is_up ? 1 : 0,
                       static_cast<unsigned long long>(network.bytes_recv_per_sec),
                       static_cast<unsigned long long>(network.bytes_sent_per_sec));
        }
        if (!metrics.top_processes.empty())
        {
            const auto& process = metrics.top_processes.front();
            LOGF_TRACE("Top collected process agent_id=%s pid=%u name=%s cpu=%.2f rss_bytes=%llu", agent_id.c_str(),
                       process.pid, process.name.c_str(), process.cpu_percent,
                       static_cast<unsigned long long>(process.memory_rss_bytes));
        }
    }

    int64_t unix_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Idle wait of the collection and heartbeat loops while not connected.
    constexpr auto kIdleSlice = std::chrono::milliseconds{250};
    // IO loop poll slice: bounds how long a queued frame or stop request waits.
    constexpr auto kIoSlice = std::chrono::milliseconds{100};
    // How long a DISCONNECT acknowledgement may take to leave before exit.
    constexpr auto kDisconnectFlushGrace = std::chrono::milliseconds{500};
    // Pending frames are dropped after this long at shutdown (ZMQ_LINGER).
    constexpr int kLingerMs = 250;
    // ZeroMQ PING/PONG keepalive: detects half-open connections.
    constexpr int kZmqHeartbeatIntervalMs = 5'000;
    constexpr int kZmqHeartbeatTimeoutMs = 15'000;
    constexpr const char* kMonitorEndpoint = "inproc://thewatcher-agent-dealer-monitor";

    template <typename... Ts>
    struct Overloaded : Ts...
    {
        using Ts::operator()...;
    };
    template <typename... Ts>
    Overloaded(Ts...) -> Overloaded<Ts...>;

    // Throws std::invalid_argument naming the offending config key.
    void validate_settings(int collection_interval, int heartbeat_interval, int process_limit)
    {
        if (auto e = check_setting("COLLECTION_INTERVAL", collection_interval, kCollectionIntervalRange))
            throw std::invalid_argument(*e);
        if (auto e = check_setting("HEARTBEAT_INTERVAL", heartbeat_interval, kHeartbeatIntervalRange))
            throw std::invalid_argument(*e);
        if (auto e = check_setting("PROCESS_LIMIT", process_limit, kProcessLimitRange))
            throw std::invalid_argument(*e);
    }
} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

Agent::Agent(AgentConfig config, AgentTimings timings)
    : config_(std::move(config))
    , timings_(timings)
    , interval_seconds_(config_.collection_interval)
    , heartbeat_interval_seconds_(config_.heartbeat_interval)
    , process_limit_(config_.process_limit)
{
    LOG_FUNCTION_TRACE

    validate_settings(config_.collection_interval, config_.heartbeat_interval, config_.process_limit);

    os_name_ = get_os_name();
    os_version_ = get_os_version();
    hostname_ = get_hostname();
    platform_ = get_platform_string();
    LOGF_DEBUG("Agent constructed id=%s host=%s platform=%s os=%s version=%s interval=%d heartbeat_interval=%d "
               "process_limit=%d",
               config_.agent_id.c_str(), hostname_.c_str(), platform_.c_str(), os_name_.c_str(), os_version_.c_str(),
               interval_seconds_.load(), heartbeat_interval_seconds_.load(), process_limit_.load());

    // Set before any connect so a destroyed agent never waits on pending frames.
    dealer_.set(zmq::sockopt::linger, kLingerMs);
    monitor_.set(zmq::sockopt::linger, 0);
}

Agent::~Agent()
{
    // The jthreads follow the shared stop_source_, not their own, so request
    // the stop before the members are destroyed and joined.
    stop();
}

// ── Threading ─────────────────────────────────────────────────────────────────

// Enrolls with the server, then connects the DEALER socket and starts the IO,
// collection and heartbeat threads. Returns early if a stop was requested.
// Throws if enrollment or the socket connect fails.
void Agent::start()
{
    LOG_FUNCTION_TRACE
    // The stop state is never reset here: a signal that arrived before start()
    // (issue #83) must still win.
    if (stop_requested())
    {
        LOG_INFO("Stop requested before start");
        return;
    }
    LOGF_INFO("Starting enrollment with %s", config_.enrollment_address.c_str());
    enroll(config_, ctx_, enrollment_stop_);
    if (stop_requested())
    {
        LOG_INFO("Enrollment stopped before data connection");
        return;
    }
    if (!config_.config_path.empty())
    {
        LOGF_DEBUG("Persisting enrolled server key pin to %s", config_.config_path.c_str());
        config_.save(config_.config_path);
    }
    LOGF_INFO("Enrollment approved - connecting to %s", config_.server_address.c_str());

    // Issue #141: no explicit routing id. The server's ROUTER generates one
    // (a counter from a random start, never sent to peers, so another peer
    // would have to guess it) and learns it from this agent's authenticated
    // frames. A fixed id equal to agent_id could be claimed first by another
    // peer.
    // SEC-02: reject oversize server replies at the socket layer and bound
    // the connection handshake.
    dealer_.set(zmq::sockopt::maxmsgsize, proto::kDataPlaneMaxMessageBytes);
    dealer_.set(zmq::sockopt::handshake_ivl, proto::kHandshakeIvlMs);
    // Queue only on completed connections: with no server a send reports
    // "would block" and the bounded outbox absorbs the frames, instead of
    // ZeroMQ buffering up to its high-water mark.
    dealer_.set(zmq::sockopt::immediate, true);
    // Detect half-open TCP connections.
    dealer_.set(zmq::sockopt::heartbeat_ivl, kZmqHeartbeatIntervalMs);
    dealer_.set(zmq::sockopt::heartbeat_timeout, kZmqHeartbeatTimeoutMs);
    if (!config_.server_public_key.empty())
    {
        LOG_DEBUG("Configuring DEALER socket with CURVE keys");
        dealer_.set(zmq::sockopt::curve_serverkey, std::string_view(config_.server_public_key));
        dealer_.set(zmq::sockopt::curve_publickey, std::string_view(config_.agent_public_key));
        dealer_.set(zmq::sockopt::curve_secretkey, std::string_view(config_.agent_secret_key));
    }
    else
    {
        LOG_WARNING("Server public key is empty; data socket CURVE encryption is disabled");
    }

    // Track the real connection state (issue #89) through a socket monitor.
    try
    {
        if (zmq_socket_monitor(dealer_.handle(), kMonitorEndpoint,
                               ZMQ_EVENT_HANDSHAKE_SUCCEEDED | ZMQ_EVENT_DISCONNECTED) != 0)
            throw zmq::error_t();
        monitor_.connect(kMonitorEndpoint);
        monitor_active_ = true;
    }
    catch (const zmq::error_t& e)
    {
        // Without events fall back to "connected once connect() was called".
        LOGF_WARNING("Socket monitor unavailable (%s); assuming connected", e.what());
        connected_.store(true);
    }

    dealer_.connect(config_.server_address);
    LOGF_DEBUG("Dealer connecting agent_id=%s", config_.agent_id.c_str());

    // All threads are created before start() returns; stop() only touches
    // stop_source_ and so cannot race with these assignments.
    io_thread_ = std::jthread([this] {
        io_loop(stop_source_.get_token());
    });
    collection_thread_ = std::jthread([this] {
        collection_loop(stop_source_.get_token());
    });
    heartbeat_thread_ = std::jthread([this] {
        heartbeat_loop(stop_source_.get_token());
    });
}

void Agent::stop()
{
    LOG_FUNCTION_TRACE
    enrollment_stop_.store(true);
    stop_source_.request_stop(); // also wakes every condition-variable wait
    LOG_DEBUG("Stop requested");
}

bool Agent::stop_requested() const noexcept
{
    return stop_source_.stop_requested();
}

bool Agent::connected() const noexcept
{
    return connected_.load();
}

// ── IO thread ─────────────────────────────────────────────────────────────────

void Agent::drain_monitor_events()
{
    zmq::message_t event, address;
    while (monitor_.recv(event, zmq::recv_flags::dontwait))
    {
        (void)monitor_.recv(address, zmq::recv_flags::none); // endpoint frame
        if (event.size() < sizeof(uint16_t))
            continue;
        uint16_t id = 0;
        std::memcpy(&id, event.data(), sizeof(id));
        if (id == ZMQ_EVENT_HANDSHAKE_SUCCEEDED)
        {
            LOG_INFO("Data connection established");
            connected_.store(true);
        }
        else if (id == ZMQ_EVENT_DISCONNECTED)
        {
            LOG_WARNING("Data connection lost");
            connected_.store(false);
        }
        else
        {
            continue;
        }
        wake_collection();
        wake_heartbeat();
    }
}

// Sends one frame as [empty delimiter, payload]. Never blocks: returns false
// when the socket cannot take it now. Once the delimiter is accepted ZeroMQ
// accepts the rest of the multipart message without blocking.
bool Agent::send_one(const Outbox::Bytes& payload)
{
    try
    {
        if (!dealer_.send(zmq::message_t{}, zmq::send_flags::sndmore | zmq::send_flags::dontwait))
            return false;
        dealer_.send(zmq::message_t(payload.data(), payload.size()), zmq::send_flags::none);
        LOGF_TRACE("Sent frame encoded_size=%zu", payload.size());
    }
    catch (const zmq::error_t& e)
    {
        // A frame the socket rejects outright is dropped, not retried forever.
        LOGF_WARNING("Dropping unsendable frame encoded_size=%zu error=%s", payload.size(), e.what());
    }
    return true;
}

bool Agent::flush_outbox()
{
    for (;;)
    {
        if (!unsent_)
        {
            unsent_ = outbox_.pop();
            if (!unsent_)
                return true;
        }
        if (!send_one(*unsent_))
            return false;
        unsent_.reset();
    }
}

void Agent::io_loop(std::stop_token st)
{
    while (!st.stop_requested())
    {
        const bool blocked = !flush_outbox();

        zmq::pollitem_t items[] = {
            {static_cast<void*>(dealer_),  0, static_cast<short>(blocked ? ZMQ_POLLIN | ZMQ_POLLOUT : ZMQ_POLLIN), 0},
            {static_cast<void*>(monitor_), 0, ZMQ_POLLIN,                                                          0},
        };
        zmq::poll(items, monitor_active_ ? 2 : 1, kIoSlice);

        if (monitor_active_ && (items[1].revents & ZMQ_POLLIN))
            drain_monitor_events();

        if (items[0].revents & ZMQ_POLLIN)
        {
            zmq::message_t delim, payload;
            if (!dealer_.recv(delim, zmq::recv_flags::dontwait))
                continue;
            if (!dealer_.recv(payload, zmq::recv_flags::none))
            {
                LOG_WARNING("Agent failed to receive a complete server frame");
                continue;
            }
            try
            {
                handle_frame(proto::decode_frame(payload.data(), payload.size()));
            }
            catch (const std::exception& e)
            {
                LOGF_WARNING("Frame decode error: %s", e.what());
            }
        }
    }
}

// ── Collection thread ─────────────────────────────────────────────────────────

void Agent::post_collector_request(CollectorRequest request)
{
    {
        std::lock_guard<std::mutex> lock(collection_mutex_);
        collector_requests_.push_back(std::move(request));
        collection_wake_ = true;
    }
    collection_cv_.notify_all();
}

std::vector<Agent::CollectorRequest> Agent::take_collector_requests()
{
    std::lock_guard<std::mutex> lock(collection_mutex_);
    return std::exchange(collector_requests_, {});
}

void Agent::wake_collection()
{
    {
        std::lock_guard<std::mutex> lock(collection_mutex_);
        collection_wake_ = true;
    }
    collection_cv_.notify_all();
}

void Agent::wake_heartbeat()
{
    {
        std::lock_guard<std::mutex> lock(heartbeat_mutex_);
        heartbeat_wake_ = true;
    }
    heartbeat_cv_.notify_all();
}

// Collects one snapshot, queues it, then polls the log files and queues any
// LOG_MATCH frames. Collection thread only.
void Agent::collect_and_send(CollectorSet& collectors)
{
    LOG_TRACE("Collecting system metrics snapshot");
    SystemMetrics metrics;
    fill_static_info(metrics);
    collectors.for_each([&](Collector& c) {
        LOGF_TRACE("Updating collector: %s", std::string(c.name()).c_str());
        c.update(metrics);
    });
    log_metrics_summary("Collected metrics", config_.agent_id, metrics);
    enqueue(proto::FrameType::METRICS, proto::pack(metrics), FramePriority::Bulk);

    collectors.log().tick();
    for (const auto& m : collectors.log().take_matches())
    {
        enqueue(proto::FrameType::LOG_MATCH, proto::pack(m), FramePriority::Bulk);
        LOGF_DEBUG("Queued LOG_MATCH frame indicator=%s path=%s", m.indicator_name.c_str(), m.path.c_str());
    }
}

void Agent::collection_loop(std::stop_token st)
{
    auto collectors = std::make_unique<CollectorSet>(process_limit_.load(), parse_log_allowed_dirs_from_env());
    CollectorConfig active_config; // last config pushed by the server
    std::chrono::steady_clock::time_point last_collect{};
    bool has_collected = false;

    while (!st.stop_requested())
    {
        // Apply work posted by the IO thread: this thread is the only one that
        // ever touches the collectors (issue #87).
        for (auto& request : take_collector_requests())
        {
            std::visit(Overloaded{
                           [&](ApplyCollectorConfig& r) {
                               active_config = std::move(r.config);
                               collectors->process().set_limit(r.process_limit);
                               collectors->apply(active_config);
                           },
                           [&](SetProcessLimit& r) {
                               collectors->process().set_limit(r.limit);
                           },
                           [&](RestartCollectors& r) {
                               LOG_INFO("Restarting collectors");
                               collectors = std::make_unique<CollectorSet>(process_limit_.load(),
                                                                           parse_log_allowed_dirs_from_env());
                               collectors->apply(active_config);
                               queue_ack(r.command_id, true, "collectors restarted");
                           },
                           [&](CollectNow& r) {
                               LOG_INFO("Immediate status requested");
                               try
                               {
                                   collect_and_send(*collectors);
                                   queue_ack(r.command_id, true, "status sent");
                               }
                               catch (const std::exception& e)
                               {
                                   LOGF_WARNING("Status collection error: %s", e.what());
                                   queue_ack(r.command_id, false, "status collection failed");
                               }
                           },
                       },
                       request);
        }

        const auto now = std::chrono::steady_clock::now();
        const bool active = connected_.load() && !paused_.load();
        // The interval is re-read every slice, so a change applies at once.
        const auto interval = std::chrono::seconds{interval_seconds_.load()};
        if (active && (!has_collected || now >= last_collect + interval))
        {
            try
            {
                collect_and_send(*collectors);
            }
            catch (const std::exception& e)
            {
                LOGF_WARNING("Collection error: %s", e.what());
            }
            last_collect = now;
            has_collected = true;
        }

        const auto deadline = active ? last_collect + interval : now + kIdleSlice;
        std::unique_lock<std::mutex> lock(collection_mutex_);
        collection_cv_.wait_until(lock, st, deadline, [&] {
            return collection_wake_ || !collector_requests_.empty();
        });
        collection_wake_ = false;
    }
}

// ── Heartbeat thread ──────────────────────────────────────────────────────────

// Sends heartbeats and config requests on their own timers, independent of the
// collection interval, pause state and the metrics backlog (issue #89).
void Agent::heartbeat_loop(std::stop_token st)
{
    using Clock = std::chrono::steady_clock;
    std::optional<Clock::time_point> last_heartbeat;
    std::optional<Clock::time_point> last_config_request;
    bool was_connected = false;

    while (!st.stop_requested())
    {
        const bool is_connected = connected_.load();
        if (is_connected != was_connected)
        {
            // A fresh connection greets the server and asks for config at once.
            last_heartbeat.reset();
            last_config_request.reset();
            was_connected = is_connected;
        }

        auto deadline = Clock::now() + kIdleSlice;
        if (is_connected)
        {
            const auto now = Clock::now();
            const auto heartbeat_every = std::chrono::seconds{heartbeat_interval_seconds_.load()};
            if (!last_heartbeat || now >= *last_heartbeat + heartbeat_every)
            {
                enqueue(proto::FrameType::HEARTBEAT, {}, FramePriority::Urgent);
                last_heartbeat = now;
            }
            if (!last_config_request || now >= *last_config_request + timings_.config_poll_interval)
            {
                enqueue(proto::FrameType::CONFIG_REQUEST, {}, FramePriority::Urgent);
                last_config_request = now;
            }
            // Deadlines come from the current settings, so a lowered interval
            // takes effect on the next wake, not after the old one expires.
            deadline =
                std::min(*last_heartbeat + heartbeat_every, *last_config_request + timings_.config_poll_interval);
        }

        std::unique_lock<std::mutex> lock(heartbeat_mutex_);
        heartbeat_cv_.wait_until(lock, st, deadline, [&] {
            return heartbeat_wake_;
        });
        heartbeat_wake_ = false;
    }
}

// ── Helpers ───────────────────────────────────────────────────────────────────

void Agent::enqueue(proto::FrameType type, std::vector<uint8_t> payload, FramePriority priority)
{
    proto::Frame f;
    f.type = static_cast<uint8_t>(type);
    f.agent_id = config_.agent_id;
    f.timestamp_ms = unix_ms();
    f.payload = std::move(payload);
    auto encoded = proto::encode_frame(f);
    LOGF_TRACE("Outbox enqueue type=%s encoded_size=%zu", frame_type_name(f.type), encoded.size());
    outbox_.push(std::move(encoded), priority);
}

void Agent::queue_ack(const std::string& command_id, bool ok, const std::string& message)
{
    enqueue(proto::FrameType::COMMAND_ACK, proto::pack(AckMessage{command_id, ok, message}), FramePriority::Urgent);
    LOGF_DEBUG("Queued ACK command_id=%s ok=%d message=%s", command_id.c_str(), ok ? 1 : 0, message.c_str());
}

void Agent::fill_static_info(SystemMetrics& m) const
{
    m.os_name = os_name_;
    m.os_version = os_version_;
    m.hostname = hostname_;
    m.platform = platform_;
    m.uptime_seconds = get_uptime_seconds();
}

// ── Command handling (IO thread) ──────────────────────────────────────────────

void Agent::handle_frame(const proto::Frame& f)
{
    auto type = static_cast<proto::FrameType>(f.type);
    if (type == proto::FrameType::COMMAND)
    {
        LOGF_DEBUG("Received COMMAND frame payload_size=%zu", f.payload.size());
        handle_command(proto::unpack<CommandMessage>(f.payload));
    }
    else if (type == proto::FrameType::CONFIG_UPDATE)
    {
        LOGF_DEBUG("Received CONFIG_UPDATE payload_size=%zu", f.payload.size());
        handle_config_update(proto::unpack<ConfigUpdate>(f.payload));
    }
}

// Applies a server-pushed config. An out-of-range value rejects the whole
// update and keeps the previous settings (issue #88).
void Agent::handle_config_update(const ConfigUpdate& cfg)
{
    if (auto e = check_setting("collection interval", cfg.interval_seconds, kCollectionIntervalRange))
    {
        LOGF_WARNING("Rejected config update: %s", e->c_str());
        return;
    }
    if (auto e = check_setting("heartbeat interval", cfg.heartbeat_interval_seconds, kHeartbeatIntervalRange))
    {
        LOGF_WARNING("Rejected config update: %s", e->c_str());
        return;
    }
    if (auto e = check_setting("process limit", cfg.process_limit, kProcessLimitRange))
    {
        LOGF_WARNING("Rejected config update: %s", e->c_str());
        return;
    }

    interval_seconds_.store(cfg.interval_seconds);
    heartbeat_interval_seconds_.store(cfg.heartbeat_interval_seconds);
    process_limit_.store(cfg.process_limit);
    LOGF_INFO("Applied config update interval_seconds=%d heartbeat_interval_seconds=%d process_limit=%d "
              "disk_configs=%zu network_configs=%zu process_watches=%zu",
              cfg.interval_seconds, cfg.heartbeat_interval_seconds, cfg.process_limit,
              cfg.collector_config.disks.size(), cfg.collector_config.networks.size(),
              cfg.collector_config.processes.size());
    post_collector_request(ApplyCollectorConfig{cfg.process_limit, cfg.collector_config});
    wake_heartbeat();
}

// Waits up to `grace` for queued frames to leave the socket.
void Agent::flush_for(std::chrono::milliseconds grace)
{
    const auto deadline = std::chrono::steady_clock::now() + grace;
    while (!flush_outbox() && std::chrono::steady_clock::now() < deadline)
    {
        zmq::pollitem_t item{static_cast<void*>(dealer_), 0, ZMQ_POLLOUT, 0};
        zmq::poll(&item, 1, std::chrono::milliseconds{20});
    }
}

// Runs one server command and queues a COMMAND_ACK. Out-of-range values and
// unknown command types are acked with ok=false; collector work is posted to
// the collection thread, which acks restart and status itself once done.
void Agent::handle_command(const CommandMessage& cmd)
{
    LOGF_DEBUG("Handling command type=%u command_id=%s args_size=%zu", static_cast<unsigned>(cmd.command_type),
               cmd.command_id.c_str(), cmd.args.size());
    try
    {
        switch (static_cast<CommandType>(cmd.command_type))
        {
        case CommandType::SET_INTERVAL: {
            const auto args = proto::unpack<SetIntervalArgs>(cmd.args);
            if (auto e = check_setting("collection interval", args.interval_seconds, kCollectionIntervalRange))
            {
                queue_ack(cmd.command_id, false, *e);
                break;
            }
            interval_seconds_.store(args.interval_seconds);
            wake_collection();
            LOGF_INFO("Set collection interval to %d seconds", args.interval_seconds);
            queue_ack(cmd.command_id, true, "interval set to " + std::to_string(args.interval_seconds));
            break;
        }
        case CommandType::SET_PROCESS_LIMIT: {
            const auto args = proto::unpack<SetProcessLimitArgs>(cmd.args);
            if (auto e = check_setting("process limit", args.limit, kProcessLimitRange))
            {
                queue_ack(cmd.command_id, false, *e);
                break;
            }
            process_limit_.store(args.limit);
            post_collector_request(SetProcessLimit{args.limit});
            LOGF_INFO("Set process limit to %d", args.limit);
            queue_ack(cmd.command_id, true, "process limit set to " + std::to_string(args.limit));
            break;
        }
        case CommandType::RESTART_COLLECTORS:
            post_collector_request(RestartCollectors{cmd.command_id});
            break;
        case CommandType::PAUSE:
            LOG_INFO("Pausing metric collection");
            paused_.store(true);
            queue_ack(cmd.command_id, true, "paused");
            break;
        case CommandType::RESUME:
            LOG_INFO("Resuming metric collection");
            paused_.store(false);
            wake_collection();
            queue_ack(cmd.command_id, true, "resumed");
            break;
        case CommandType::GET_STATUS:
            post_collector_request(CollectNow{cmd.command_id});
            break;
        case CommandType::DISCONNECT:
            LOG_INFO("Disconnect command received");
            queue_ack(cmd.command_id, true, "disconnecting");
            flush_for(kDisconnectFlushGrace); // let the ack out before exiting
            stop();                           // the process owner sees stop_requested() and exits
            break;
        default:
            LOGF_WARNING("Unknown command type=%u command_id=%s", static_cast<unsigned>(cmd.command_type),
                         cmd.command_id.c_str());
            queue_ack(cmd.command_id, false, "unknown command");
            break;
        }
    }
    catch (const std::exception& e)
    {
        // Malformed or out-of-range arguments (checked narrowing in commands.hpp).
        LOGF_WARNING("Command %s rejected: %s", cmd.command_id.c_str(), e.what());
        queue_ack(cmd.command_id, false, "invalid command arguments");
    }
}

} // namespace thewatcher::agent
