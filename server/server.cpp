#include "server.hpp"

#include "common/SingleLog.hpp"
#include "common/commands.hpp"
#include "common/crypto.hpp"
#include "common/metrics.hpp"
#include "common/zmq_limits.hpp"
#include "report_generator.hpp"
#include "status_engine.hpp"

#include <chrono>
#include <ctime>
#include <stdexcept>

namespace thewatcher::server
{

using namespace thewatcher::proto;

namespace
{

    int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // SEC-05: attacker-controlled strings may carry CR/LF or ANSI control
    // characters used for log forging; replace them before logging.
    std::string sanitize_log_text(std::string_view text)
    {
        constexpr std::size_t max_length = 64;
        std::string out;
        out.reserve(std::min<std::size_t>(text.size(), max_length));
        for (char c : text)
        {
            const auto uc = static_cast<unsigned char>(c);
            out.push_back((uc < 0x20 || uc == 0x7F) ? '?' : c);
            if (out.size() >= max_length)
                break;
        }
        return out;
    }

    const char* frame_type_name(uint8_t type)
    {
        switch (static_cast<FrameType>(type))
        {
        case FrameType::HEARTBEAT:
            return "HEARTBEAT";
        case FrameType::METRICS:
            return "METRICS";
        case FrameType::COMMAND:
            return "COMMAND";
        case FrameType::COMMAND_ACK:
            return "COMMAND_ACK";
        case FrameType::CONFIG_UPDATE:
            return "CONFIG_UPDATE";
        case FrameType::ENROLL_REQUEST:
            return "ENROLL_REQUEST";
        case FrameType::ENROLL_RESPONSE:
            return "ENROLL_RESPONSE";
        case FrameType::CONFIG_REQUEST:
            return "CONFIG_REQUEST";
        case FrameType::LOG_MATCH:
            return "LOG_MATCH";
        }
        return "UNKNOWN";
    }

    void log_received_metrics_summary(const std::string& agent_id, const SystemMetrics& metrics, int64_t timestamp_ms,
                                      std::size_t payload_size)
    {
        LOGF_DEBUG(
            "Received metrics agent_id=%s host=%s platform=%s timestamp_ms=%lld payload_size=%zu uptime_seconds=%.0f "
            "cpu_usage=%.2f memory_usage=%.2f disks=%zu networks=%zu temperatures=%zu processes=%zu",
            agent_id.c_str(), metrics.hostname.c_str(), metrics.platform.c_str(), static_cast<long long>(timestamp_ms),
            payload_size, metrics.uptime_seconds, metrics.cpu.usage_percent, metrics.memory.usage_percent,
            metrics.disks.size(), metrics.networks.size(), metrics.temperatures.size(), metrics.top_processes.size());
        if (!metrics.disks.empty())
        {
            const auto& disk = metrics.disks.front();
            LOGF_TRACE("Received first disk agent_id=%s mount=%s fs=%s usage=%.2f total_bytes=%llu used_bytes=%llu",
                       agent_id.c_str(), disk.mount_point.c_str(), disk.filesystem.c_str(), disk.usage_percent,
                       static_cast<unsigned long long>(disk.total_bytes),
                       static_cast<unsigned long long>(disk.used_bytes));
        }
        if (!metrics.networks.empty())
        {
            const auto& network = metrics.networks.front();
            LOGF_TRACE("Received first network agent_id=%s interface=%s up=%d recv_per_sec=%llu sent_per_sec=%llu",
                       agent_id.c_str(), network.interface_name.c_str(), network.is_up ? 1 : 0,
                       static_cast<unsigned long long>(network.bytes_recv_per_sec),
                       static_cast<unsigned long long>(network.bytes_sent_per_sec));
        }
        if (!metrics.top_processes.empty())
        {
            const auto& process = metrics.top_processes.front();
            LOGF_TRACE("Received top process agent_id=%s pid=%u name=%s cpu=%.2f rss_bytes=%llu", agent_id.c_str(),
                       process.pid, process.name.c_str(), process.cpu_percent,
                       static_cast<unsigned long long>(process.memory_rss_bytes));
        }
    }

} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

Server::Server(ServerConfig config)
    : config_(std::move(config))
{
    LOG_FUNCTION_TRACE
    LOGF_DEBUG("Creating server bind=%s enrollment=%s api=%s:%d db_type=%s db_path=%s offline_after_seconds=%d",
               config_.bind_address.c_str(), config_.enrollment_address.c_str(), config_.api_host.c_str(),
               config_.api_port, config_.db_type.c_str(), config_.db_path.c_str(), config_.offline_after_seconds);
    const auto& db_target = (config_.db_type == "postgres") ? config_.postgres_dsn : config_.db_path;
    store_ = make_store(config_.db_type, db_target);
    zap_ = std::make_unique<ZapHandler>(ctx_);
    api_ = std::make_unique<ApiServer>(*store_, *zap_, config_.api_host, config_.api_port);

    // Pre-load all already-approved agents into the ZAP key set so they can
    // reconnect after a server restart without needing re-approval. SEC-04:
    // each key is bound to its agent_id so ZAP can report the authenticated
    // identity for every CURVE peer.
    for (auto& agent : store_->list_agents())
    {
        if (agent.approved)
        {
            zap_->add_key(agent.curve_public_key_z85, agent.agent_id);
            LOGF_DEBUG("Loaded approved agent key into ZAP id=%s", agent.agent_id.c_str());
        }
    }
}

Server::~Server()
{
    stop();
}

// ── Main loop ─────────────────────────────────────────────────────────────────

void Server::run(StartupSignal* startup)
{
    LOG_FUNCTION_TRACE
    // ROUTER: encrypted data plane for metric/heartbeat/ack traffic
    zmq::socket_t router(ctx_, ZMQ_ROUTER);
    router.set(zmq::sockopt::linger, 0);
    // Silently drop frames addressed to an unknown identity rather than erroring.
    router.set(zmq::sockopt::router_mandatory, 0);
    // SEC-02: reject oversize messages at the socket layer and bound the
    // connection handshake so hostile peers cannot exhaust server memory.
    router.set(zmq::sockopt::maxmsgsize, kDataPlaneMaxMessageBytes);
    router.set(zmq::sockopt::handshake_ivl, kHandshakeIvlMs);

    if (!config_.server_public_key.empty())
    {
        LOG_DEBUG("Configuring ROUTER socket as CURVE server");
        router.set(zmq::sockopt::curve_server, true);
        router.set(zmq::sockopt::curve_secretkey, std::string_view(config_.server_secret_key));
    }
    else
    {
        LOG_WARNING("Server public key is empty; ROUTER socket CURVE encryption is disabled");
    }
    router.bind(config_.bind_address);
    LOGF_INFO("ROUTER bound to %s", config_.bind_address.c_str());

    // REP: plain-text enrollment handshake (no CURVE)
    zmq::socket_t enroll(ctx_, ZMQ_REP);
    enroll.set(zmq::sockopt::linger, 0);
    // SEC-02: enrollment frames are tiny; 64 KiB is a generous ceiling. The
    // handshake and send timeouts keep unauthenticated peers from stalling
    // the single data-plane thread.
    enroll.set(zmq::sockopt::maxmsgsize, kEnrollMaxMessageBytes);
    enroll.set(zmq::sockopt::handshake_ivl, kHandshakeIvlMs);
    enroll.set(zmq::sockopt::sndtimeo, kEnrollSendTimeoutMs);
    enroll.bind(config_.enrollment_address);
    LOGF_INFO("Enrollment REP bound to %s", config_.enrollment_address.c_str());

    api_->start();
    LOGF_INFO("REST API on %s:%d", config_.api_host.c_str(), config_.api_port);

    running_.store(true);
    if (startup)
        startup->succeed();
    auto next_offline_scan = std::chrono::steady_clock::now();

    zmq::pollitem_t items[] = {
        {static_cast<void*>(router), 0, ZMQ_POLLIN, 0},
        {static_cast<void*>(enroll), 0, ZMQ_POLLIN, 0},
    };

    while (running_.load())
    {
        zmq::poll(items, 2, std::chrono::milliseconds{100});

        if (items[0].revents & ZMQ_POLLIN)
        {
            // SEC-01: no handler exception may escape run() — the ROUTER has
            // no reply state, so logging and continuing is always safe.
            try
            {
                handle_router_frame(router);
            }
            catch (const std::exception& e)
            {
                LOGF_ERROR("ROUTER handler error: %s", e.what());
            }
        }

        if (items[1].revents & ZMQ_POLLIN)
        {
            // handle_enrollment guarantees a reply on every path; this is a
            // last-resort backstop for failures before its internal try block.
            try
            {
                handle_enrollment(enroll);
            }
            catch (const std::exception& e)
            {
                LOGF_ERROR("Enrollment handler error: %s", e.what());
            }
        }

        dispatch_commands(router);
        if (std::chrono::steady_clock::now() >= next_offline_scan)
        {
            const auto now = now_ms();
            if (config_.offline_after_seconds > 0)
            {
                const auto cutoff = now - (static_cast<int64_t>(config_.offline_after_seconds) * 1000);
                LOGF_TRACE("Scanning for offline agents cutoff_ms=%lld", static_cast<long long>(cutoff));
                store_->mark_agents_offline_before(cutoff);
                const auto ts = now_ms();
                for (const auto& agent_id : store_->get_offline_unalerted_agent_ids())
                {
                    AlertRecord alert;
                    alert.agent_id = agent_id;
                    alert.indicator = "Heartbeat";
                    alert.old_status = "green";
                    alert.new_status = "red";
                    alert.message = "Agent has gone offline (no heartbeat received)";
                    alert.created_at = ts;
                    store_->insert_alert(alert);
                    LOGF_INFO("Created dead-agent alert for agent_id=%s", agent_id.c_str());
                }
            }

            // Escalate unacknowledged alerts older than the configurable timeout.
            const auto escalation_seconds_str = store_->get_setting("escalation_timeout_seconds", "3600");
            const auto escalation_seconds = std::stoi(escalation_seconds_str);
            if (escalation_seconds > 0)
            {
                const auto esc_cutoff = now - (static_cast<int64_t>(escalation_seconds) * 1000LL);
                store_->escalate_old_alerts(esc_cutoff, now);
            }

            // Apply scheduled maintenance windows: start or end maintenance for affected agents.
            const auto active_windows = store_->active_maintenance_windows(now);
            for (const auto& agent : store_->list_approved_agents())
            {
                bool in_window = false;
                std::string window_reason;
                for (const auto& win : active_windows)
                {
                    if (win.agent_id == "*" || win.agent_id == agent.agent_id)
                    {
                        in_window = true;
                        window_reason = win.reason;
                        break;
                    }
                }
                if (in_window && !agent.maintenance)
                {
                    store_->set_agent_maintenance(agent.agent_id, true, "mw:" + window_reason, 0);
                    store_->clear_active_alerts_for_agent(agent.agent_id, now);
                    LOGF_INFO("Scheduled maintenance started agent_id=%s reason=%s", agent.agent_id.c_str(),
                              window_reason.c_str());
                }
                else if (!in_window && agent.maintenance && agent.maintenance_reason.rfind("mw:", 0) == 0)
                {
                    // Only auto-resume agents put into maintenance by a window (prefix "mw:").
                    store_->set_agent_maintenance(agent.agent_id, false, "", 0);
                    LOGF_INFO("Scheduled maintenance ended agent_id=%s", agent.agent_id.c_str());
                }
            }

            // Evict dispatched commands that never received an ACK (M-8).
            constexpr int64_t command_ttl_ms = 5LL * 60LL * 1000LL;
            for (auto it = dispatched_commands_.begin(); it != dispatched_commands_.end();)
            {
                if (now - it->second.dispatched_at_ms > command_ttl_ms)
                {
                    LOGF_DEBUG("Evicting stale dispatched command id=%s", it->first.c_str());
                    it = dispatched_commands_.erase(it);
                }
                else
                {
                    ++it;
                }
            }

            maybe_send_report(now);

            next_offline_scan = std::chrono::steady_clock::now() + std::chrono::seconds{1};
        }
    }

    api_->stop();
    LOG_INFO("Stopped.");
}

void Server::stop()
{
    LOG_FUNCTION_TRACE
    running_.store(false);
}

// ── ROUTER handler ────────────────────────────────────────────────────────────

void Server::handle_router_frame(zmq::socket_t& router)
{
    // ROUTER prepends the sender's routing-id (set by agent to its agent_id).
    // The agent DEALER explicitly sends [empty delimiter][payload], so the
    // server receives exactly: [identity][empty][payload].
    zmq::message_t identity_msg, delim_msg, payload_msg;
    if (!router.recv(identity_msg, zmq::recv_flags::none))
        return;
    if (!router.recv(delim_msg, zmq::recv_flags::none))
        return;
    if (!router.recv(payload_msg, zmq::recv_flags::none))
        return;

    // Drain unexpected extra frames so the socket state stays clean.
    while (router.get(zmq::sockopt::rcvmore))
    {
        zmq::message_t extra;
        if (!router.recv(extra, zmq::recv_flags::none))
            break;
    }

    if (payload_msg.size() == 0)
        return;

    Frame frame;
    try
    {
        frame = decode_frame(payload_msg.data(), payload_msg.size());
    }
    catch (const std::exception& e)
    {
        LOGF_WARNING("Frame decode error: %s", e.what());
        return;
    }

    const std::string& agent_id = frame.agent_id;
    LOGF_TRACE("Received frame identity_size=%zu agent_id=%s type=%s raw_type=%u payload_size=%zu frame_timestamp=%lld",
               identity_msg.size(), agent_id.c_str(), frame_type_name(frame.type), static_cast<unsigned>(frame.type),
               frame.payload.size(), static_cast<long long>(frame.timestamp_ms));

    // SEC-04: when CURVE is enabled, the ZAP-authenticated identity is
    // authoritative. frame.agent_id is attacker-controlled payload; a peer
    // whose key belongs to a different agent must not be able to poison
    // another agent's metrics, alerts or config.
    if (!config_.server_public_key.empty())
    {
        std::string authenticated_id;
        try
        {
            // libzmq stamps the ZAP user_id onto the peer's messages as
            // the "User-Id" metadata property.
            authenticated_id = identity_msg.gets("User-Id");
        }
        catch (const zmq::error_t&)
        {
            // Property absent (e.g. peer connected without a ZAP identity).
        }
        if (authenticated_id != agent_id)
        {
            LOGF_WARNING("Dropping frame from authenticated agent_id=%s claiming agent_id=%s type=%s",
                         authenticated_id.c_str(), agent_id.c_str(), frame_type_name(frame.type));
            return;
        }
    }

    // SEC-09: approval is enforced per frame, not only at handshake time.
    // ZAP removal only affects new handshakes, so a rejected or deleted
    // agent would otherwise keep feeding data on its live session.
    auto rec = store_->get_agent(agent_id);
    if (!rec || !rec->approved || rec->rejected)
    {
        LOGF_WARNING("Dropping frame from non-approved agent_id=%s type=%s", agent_id.c_str(),
                     frame_type_name(frame.type));
        return;
    }

    // Update last_seen timestamp for every inbound frame.
    const bool was_offline = !rec->connected;
    rec->connected = true;
    rec->last_seen = now_ms();
    store_->upsert_agent(*rec);
    if (was_offline)
    {
        store_->archive_heartbeat_alerts_for_agent(agent_id, now_ms());
        LOGF_INFO("Agent reconnected, archived heartbeat alerts for agent_id=%s", agent_id.c_str());
    }
    LOGF_TRACE("Updated last_seen for agent_id=%s", agent_id.c_str());

    auto ftype = static_cast<FrameType>(frame.type);

    if (ftype == FrameType::METRICS)
    {
        try
        {
            auto metrics = unpack<SystemMetrics>(frame.payload);
            log_received_metrics_summary(agent_id, metrics, frame.timestamp_ms, frame.payload.size());
            // SEC-07: the agent-supplied timestamp is untrusted. Storage,
            // status evaluation and — critically — the retention prune gate
            // all run on the server receive time; a future-dated frame can
            // no longer delete every agent's metric history.
            const int64_t server_now = now_ms();
            MetricsRow row;
            row.agent_id = agent_id;
            row.timestamp_ms = server_now;
            row.metrics_cbor = frame.payload; // already CBOR-encoded SystemMetrics; store verbatim.
            store_->insert_metrics(row);
            StatusEngine status_engine(*store_);
            status_engine.evaluate_metrics(agent_id, metrics, server_now);
            LOGF_DEBUG("Stored metrics agent_id=%s timestamp_ms=%lld payload_size=%zu", agent_id.c_str(),
                       static_cast<long long>(server_now), row.metrics_cbor.size());
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("Metrics error for %s: %s", agent_id.c_str(), e.what());
        }
    }
    else if (ftype == FrameType::HEARTBEAT)
    {
        // last_seen already updated; no further action needed.
        LOGF_DEBUG("Heartbeat processed agent_id=%s timestamp_ms=%lld payload_size=%zu", agent_id.c_str(),
                   static_cast<long long>(frame.timestamp_ms), frame.payload.size());
    }
    else if (ftype == FrameType::CONFIG_REQUEST)
    {
        auto agent = store_->get_agent(agent_id);
        if (agent)
        {
            ConfigUpdate cfg{agent->collection_interval, agent->process_limit, agent->collector_config,
                             agent->heartbeat_interval};
            LOGF_DEBUG("Sending config update agent_id=%s interval_seconds=%d heartbeat_interval_seconds=%d "
                       "process_limit=%d disk_configs=%zu "
                       "network_configs=%zu process_watches=%zu",
                       agent_id.c_str(), cfg.interval_seconds, cfg.heartbeat_interval_seconds, cfg.process_limit,
                       cfg.collector_config.disks.size(), cfg.collector_config.networks.size(),
                       cfg.collector_config.processes.size());
            Frame response;
            response.type = static_cast<uint8_t>(FrameType::CONFIG_UPDATE);
            response.agent_id = agent_id;
            response.timestamp_ms = now_ms();
            response.payload = pack(cfg);

            auto encoded = encode_frame(response);
            // SEC-04: reply to the peer's real routing identity, not to the
            // agent_id claimed in the request payload.
            router.send(zmq::message_t(identity_msg.data(), identity_msg.size()), zmq::send_flags::sndmore);
            router.send(zmq::message_t{}, zmq::send_flags::sndmore);
            router.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
        }
        else
        {
            LOGF_DEBUG("Ignoring config request from unknown agent_id=%s", agent_id.c_str());
        }
    }
    else if (ftype == FrameType::COMMAND_ACK)
    {
        try
        {
            auto ack = unpack<AckMessage>(frame.payload);
            auto command = dispatched_commands_.find(ack.command_id);
            if (command != dispatched_commands_.end())
            {
                // SEC-04: the ACK must come from the agent the command was
                // dispatched to. A different agent guessing the command_id
                // must neither clear the pending command nor have its state
                // changed by it.
                if (command->second.target_agent_id != agent_id)
                {
                    LOGF_WARNING("Ignoring ACK from agent_id=%s for command_id=%s targeted at agent_id=%s",
                                 agent_id.c_str(), ack.command_id.c_str(), command->second.target_agent_id.c_str());
                }
                else
                {
                    if (ack.success &&
                        (command->second.type == CommandType::DISCONNECT ||
                         command->second.type == CommandType::PAUSE || command->second.type == CommandType::RESUME))
                    {
                        auto command_agent = store_->get_agent(agent_id);
                        if (command_agent)
                        {
                            if (command->second.type == CommandType::DISCONNECT)
                                command_agent->connected = false;
                            else if (command->second.type == CommandType::PAUSE)
                            {
                                command_agent->maintenance = true;
                                command_agent->maintenance_reason = "command";
                                command_agent->maintenance_until = 0;
                            }
                            else if (command->second.type == CommandType::RESUME)
                            {
                                command_agent->maintenance = false;
                                command_agent->maintenance_reason = "";
                                command_agent->maintenance_until = 0;
                            }
                            command_agent->last_seen = now_ms();
                            store_->upsert_agent(*command_agent);
                            if (command->second.type == CommandType::PAUSE)
                            {
                                StatusEngine status_engine(*store_);
                                status_engine.enter_maintenance(agent_id, "command", 0, command_agent->last_seen);
                            }
                            else if (command->second.type == CommandType::RESUME)
                            {
                                StatusEngine status_engine(*store_);
                                status_engine.exit_maintenance(agent_id);
                            }
                        }
                    }
                    dispatched_commands_.erase(command);
                }
            }
            LOGF_INFO("ACK from %s cmd=%s ok=%d msg=%s", agent_id.c_str(), ack.command_id.c_str(), ack.success ? 1 : 0,
                      ack.message.c_str());
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("Failed to unpack command ACK from %s: %s", agent_id.c_str(), e.what());
        }
    }
    else if (ftype == FrameType::LOG_MATCH)
    {
        try
        {
            auto match = unpack<LogMatch>(frame.payload);
            // SEC-07: record and alert times come from the server clock; the
            // agent-supplied frame timestamp is untrusted.
            const int64_t server_now = now_ms();
            LogMatchRecord log_match_record;
            log_match_record.agent_id = agent_id;
            log_match_record.indicator_name = match.indicator_name;
            log_match_record.path = match.path;
            log_match_record.matched_line = match.matched_line;
            log_match_record.severity = match.severity;
            log_match_record.created_at = server_now;
            store_->insert_log_match(log_match_record);

            // Raise an alert on the named indicator so it shows up on the dashboard.
            if (!store_->is_silenced(agent_id, match.indicator_name, server_now))
            {
                AlertRecord alert;
                alert.agent_id = agent_id;
                alert.indicator = match.indicator_name;
                alert.old_status = "green";
                alert.new_status = match.severity;
                alert.message = "Log match in " + match.path + ": " + match.matched_line.substr(0, 200);
                alert.created_at = server_now;
                store_->insert_alert(alert);
                LOGF_INFO("Log match alert agent_id=%s indicator=%s severity=%s path=%s", agent_id.c_str(),
                          match.indicator_name.c_str(), match.severity.c_str(), match.path.c_str());
            }
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("LOG_MATCH error for %s: %s", agent_id.c_str(), e.what());
        }
    }
    else
    {
        LOGF_DEBUG("Ignoring unsupported frame type=%u from agent_id=%s", static_cast<unsigned>(frame.type),
                   agent_id.c_str());
    }
}

// ── Enrollment handler ────────────────────────────────────────────────────────

void Server::handle_enrollment(zmq::socket_t& enroll)
{
    LOG_FUNCTION_TRACE

    // Helper: send an error response and return.
    auto send_error = [&](const std::string& reason) {
        LOGF_DEBUG("Sending enrollment error response reason=%s", reason.c_str());
        EnrollResponse resp;
        resp.approved = false;
        resp.message = reason;
        Frame f;
        f.type = static_cast<uint8_t>(FrameType::ENROLL_RESPONSE);
        f.agent_id = "";
        f.timestamp_ms = now_ms();
        f.payload = pack(resp);
        auto encoded = encode_frame(f);
        enroll.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
    };

    zmq::message_t msg;
    // Poll-driven receive: use a non-blocking read. zmq_poll reported POLLIN,
    // but REP can still swallow a malformed envelope (no leading delimiter) at
    // the socket layer and leave nothing to deliver — a blocking recv would
    // then wedge the single data-plane thread forever waiting for a message
    // that already arrived and was discarded (SEC-01 stall variant).
    if (!enroll.recv(msg, zmq::recv_flags::dontwait))
        return;

    // SEC-01: a REP socket may only send a reply after the LAST request
    // frame has been read; replying mid-message fails with EFSM and used to
    // crash the whole server via an unhandled cppzmq exception. Drain any
    // trailing parts so the reply below can never hit that state, and
    // reject multipart requests outright.
    bool multipart = false;
    while (enroll.get(zmq::sockopt::rcvmore))
    {
        zmq::message_t extra;
        // Non-blocking: the next part is already buffered when rcvmore is
        // set. If the read fails anyway the request is truncated — reply
        // with an error rather than staying in the receive state.
        if (!enroll.recv(extra, zmq::recv_flags::dontwait))
        {
            multipart = true;
            break;
        }
        multipart = true;
    }
    if (multipart)
    {
        LOGF_WARNING("Rejected multipart enrollment message size=%zu", msg.size());
        send_error("multipart enrollment not supported");
        return;
    }

    LOGF_TRACE("Enrollment message received size=%zu", msg.size());

    // SEC-01: every failure path below must still answer — an unanswered REP
    // request wedges the socket state machine and stalls all future
    // enrollments, so the catch-all makes a best-effort error reply.
    try
    {

        Frame frame;
        try
        {
            frame = decode_frame(msg.data(), msg.size());
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("Enrollment decode error: %s", e.what());
            send_error("bad frame");
            return;
        }

        EnrollRequest req;
        try
        {
            req = unpack<EnrollRequest>(frame.payload);
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("EnrollRequest unpack error: %s", e.what());
            send_error("bad payload");
            return;
        }
        // SEC-05: every field below is attacker-controlled. Validate format
        // and bounds before touching storage, the rate-limit maps or the
        // log. agent_id doubles as a routing identity and DB key, so its
        // charset is deliberately strict.
        static constexpr std::size_t max_agent_id_len = 64;
        static constexpr std::size_t max_hostname_len = 255;
        static constexpr std::size_t max_platform_len = 64;
        static constexpr std::size_t max_enroll_token_len = 256;
        if (req.agent_id.empty() || req.agent_id.size() > max_agent_id_len ||
            req.agent_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") !=
                std::string::npos)
        {
            LOGF_WARNING("Enrollment rejected: invalid agent_id format length=%zu", req.agent_id.size());
            send_error("invalid agent_id");
            return;
        }
        if (req.hostname.empty() || req.hostname.size() > max_hostname_len)
        {
            LOGF_WARNING("Enrollment rejected: invalid hostname length=%zu agent_id=%s", req.hostname.size(),
                         req.agent_id.c_str());
            send_error("invalid hostname");
            return;
        }
        if (req.platform.empty() || req.platform.size() > max_platform_len)
        {
            LOGF_WARNING("Enrollment rejected: invalid platform length=%zu agent_id=%s", req.platform.size(),
                         req.agent_id.c_str());
            send_error("invalid platform");
            return;
        }
        if (req.enrollment_token.size() > max_enroll_token_len)
        {
            LOGF_WARNING("Enrollment rejected: oversized enrollment token agent_id=%s", req.agent_id.c_str());
            send_error("invalid enrollment token");
            return;
        }

        // M-9: Validate z85 public key — must be exactly 40 chars, z85 alphabet only.
        static constexpr std::string_view z85_alphabet =
            "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.-:+=^!/*?&<>()[]{}@%$#";
        if (req.curve_public_key_z85.size() != 40 ||
            req.curve_public_key_z85.find_first_not_of(z85_alphabet) != std::string::npos)
        {
            LOGF_WARNING("Enrollment rejected: invalid public key format agent_id=%s", req.agent_id.c_str());
            send_error("invalid public key");
            return;
        }

        // SEC-05: when a pre-shared enrollment token is configured, require
        // it on every request so arbitrary hosts cannot create pending
        // records at will.
        if (!config_.enrollment_token.empty())
        {
            const bool token_matches = req.enrollment_token.size() == config_.enrollment_token.size() &&
                                       ::sodium_memcmp(req.enrollment_token.data(), config_.enrollment_token.data(),
                                                       req.enrollment_token.size()) == 0;
            if (!token_matches)
            {
                LOGF_WARNING("Enrollment rejected: bad enrollment token agent_id=%s", req.agent_id.c_str());
                send_error("invalid enrollment token");
                return;
            }
        }

        LOGF_INFO("Enrollment request agent_id=%s hostname=%s platform=%s", req.agent_id.c_str(),
                  sanitize_log_text(req.hostname).c_str(), sanitize_log_text(req.platform).c_str());

        // H-1 + SEC-05: per-agent and per-source rate limiting. Both maps are
        // pruned on every request so attacker-chosen ids cannot grow them
        // without bound. The per-source limit is milder than the per-agent
        // one so fleets sharing a NAT can still roll out together.
        constexpr int64_t enroll_rate_limit_ms = 10'000;
        constexpr int64_t enroll_peer_rate_limit_ms = 1'000;
        constexpr int64_t rate_entry_ttl_ms = 60'000;
        const auto enroll_now = now_ms();

        std::string peer_address = "unknown";
        try
        {
            peer_address = msg.gets("Peer-Address");
        }
        catch (const zmq::error_t&)
        {
        }

        for (auto it = enrollment_last_request_ms_.begin(); it != enrollment_last_request_ms_.end();)
            it = (enroll_now - it->second > rate_entry_ttl_ms) ? enrollment_last_request_ms_.erase(it) : std::next(it);
        for (auto it = enrollment_peer_last_request_ms_.begin(); it != enrollment_peer_last_request_ms_.end();)
            it = (enroll_now - it->second > rate_entry_ttl_ms) ? enrollment_peer_last_request_ms_.erase(it)
                                                               : std::next(it);

        auto& last_enroll = enrollment_last_request_ms_[req.agent_id];
        auto& peer_last_enroll = enrollment_peer_last_request_ms_[peer_address];
        if ((last_enroll != 0 && enroll_now - last_enroll < enroll_rate_limit_ms) ||
            (peer_last_enroll != 0 && enroll_now - peer_last_enroll < enroll_peer_rate_limit_ms))
        {
            LOGF_DEBUG("Enrollment rate-limited agent_id=%s", req.agent_id.c_str());
            send_error("rate limited");
            return;
        }
        last_enroll = enroll_now;
        peer_last_enroll = enroll_now;

        auto existing = store_->get_agent(req.agent_id);
        bool approved = false;

        if (existing)
        {
            if (existing->rejected)
            {
                LOGF_NOTICE("Rejected enrollment retry agent_id=%s", req.agent_id.c_str());
                send_error("rejected");
                return;
            }

            approved = existing->approved;
            if (approved)
            {
                // SEC-06: an approved agent re-enrolling is answered but
                // nothing is updated. The record is authoritative; an
                // unauthenticated request must not refresh last_seen (which
                // would suppress dead-agent detection) or rewrite display
                // fields.
                LOGF_DEBUG("Re-enrollment of already-approved agent_id=%s", req.agent_id.c_str());
            }
            else
            {
                // SEC-06: a pending record's public key is immutable from
                // first sight — overwriting it would let an attacker hijack
                // the pending slot before an admin approves it.
                if (!existing->curve_public_key_z85.empty() &&
                    existing->curve_public_key_z85 != req.curve_public_key_z85)
                {
                    LOGF_WARNING("Enrollment public key mismatch for pending agent_id=%s", req.agent_id.c_str());
                    send_error("public key mismatch: delete the pending record to enroll a new key");
                    return;
                }
                existing->hostname = req.hostname;
                existing->platform = req.platform;
                store_->upsert_agent(*existing);
            }
        }
        else
        {
            // SEC-05: bound the pending queue so enrollment floods cannot
            // create unbounded rows or bury real agents in the dashboard.
            constexpr std::size_t max_pending_agents = 1'000;
            if (store_->list_pending_agents().size() >= max_pending_agents)
            {
                LOGF_WARNING("Enrollment rejected: pending agent queue is full max=%zu", max_pending_agents);
                send_error("pending queue full");
                return;
            }

            AgentRecord rec;
            rec.agent_id = req.agent_id;
            rec.hostname = req.hostname;
            rec.platform = req.platform;
            rec.curve_public_key_z85 = req.curve_public_key_z85;
            rec.approved = false;
            rec.rejected = false;
            rec.first_seen = now_ms();
            rec.last_seen = rec.first_seen;
            store_->upsert_agent(rec);
            LOGF_NOTICE("New agent: %s (%s/%s) - pending approval", req.agent_id.c_str(),
                        sanitize_log_text(req.hostname).c_str(), sanitize_log_text(req.platform).c_str());
        }

        EnrollResponse resp;
        resp.approved = approved;
        resp.message = approved ? "approved" : "pending approval";
        if (approved && !config_.server_public_key.empty())
        {
            resp.server_public_key_z85 = config_.server_public_key;
            resp.server_public_key_fingerprint =
                thewatcher::crypto::server_public_key_fingerprint(config_.server_public_key);
        }

        Frame resp_frame;
        resp_frame.type = static_cast<uint8_t>(FrameType::ENROLL_RESPONSE);
        resp_frame.agent_id = req.agent_id;
        resp_frame.timestamp_ms = now_ms();
        resp_frame.payload = pack(resp);

        auto encoded = encode_frame(resp_frame);
        enroll.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
        LOGF_DEBUG("Enrollment response agent_id=%s approved=%d message=%s", req.agent_id.c_str(),
                   resp.approved ? 1 : 0, resp.message.c_str());
    }
    catch (const std::exception& e)
    {
        LOGF_ERROR("Enrollment handler error: %s", e.what());
        try
        {
            send_error("internal error");
        }
        catch (const std::exception& reply_error)
        {
            LOGF_ERROR("Enrollment error reply failed: %s", reply_error.what());
        }
    }
}

// ── Command dispatch ──────────────────────────────────────────────────────────

void Server::dispatch_commands(zmq::socket_t& router)
{
    std::vector<PendingCommand> cmds;
    api_->drain_commands(cmds);
    if (!cmds.empty())
        LOGF_DEBUG("Dispatching %zu pending command(s)", cmds.size());

    for (auto& pc : cmds)
    {
        Frame f;
        f.type = static_cast<uint8_t>(FrameType::COMMAND);
        f.agent_id = pc.agent_id;
        f.timestamp_ms = now_ms();
        f.payload = pack(pc.cmd);

        auto encoded = encode_frame(f);

        // ROUTER send: [identity][empty delimiter][payload]
        // The agent DEALER receives [empty][payload] (identity is stripped).
        try
        {
            router.send(zmq::message_t(pc.agent_id.data(), pc.agent_id.size()), zmq::send_flags::sndmore);
            router.send(zmq::message_t{}, zmq::send_flags::sndmore);
            router.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
            dispatched_commands_[pc.cmd.command_id] = {static_cast<CommandType>(pc.cmd.command_type), now_ms(),
                                                       pc.agent_id};
            LOGF_DEBUG("Dispatched command agent_id=%s command_id=%s type=%u", pc.agent_id.c_str(),
                       pc.cmd.command_id.c_str(), static_cast<unsigned>(pc.cmd.command_type));
        }
        catch (const std::exception& e)
        {
            LOGF_ERROR("Failed to dispatch command to %s: %s", pc.agent_id.c_str(), e.what());
        }
    }
}

void Server::maybe_send_report(int64_t now_ms)
{
    if (store_->get_setting("reports.enabled", "false") != "true")
        return;

    const auto schedule = store_->get_setting("reports.schedule", "daily"); // "daily" or "weekly"
    const int target_hour = std::stoi(store_->get_setting("reports.hour", "8"));
    const int target_dow = std::stoi(store_->get_setting("reports.day_of_week", "1")); // 0=Sun

    // Minimum gap: 23 h for daily, 6 days 23 h for weekly.
    const int64_t min_gap_ms = (schedule == "weekly") ? 6LL * 86'400'000LL + 23LL * 3'600'000LL : 23LL * 3'600'000LL;
    if (last_report_ms_ > 0 && now_ms - last_report_ms_ < min_gap_ms)
        return;

    // Convert now_ms to UTC calendar fields.
    const time_t now_t = static_cast<time_t>(now_ms / 1000);
    struct tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now_t);
#else
    gmtime_r(&now_t, &utc);
#endif
    if (utc.tm_hour != target_hour)
        return;
    if (schedule == "weekly" && utc.tm_wday != target_dow)
        return;

    LOG_INFO("Sending scheduled fleet digest report");
    if (generate_and_send_report(*store_, now_ms))
    {
        last_report_ms_ = now_ms;
        store_->set_setting("reports.last_sent_ms", std::to_string(now_ms));
    }
}

} // namespace thewatcher::server
