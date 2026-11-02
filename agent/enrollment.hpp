#pragma once

#include <atomic>
#include <string>
#include <zmq.hpp>

namespace thewatcher::agent
{

struct AgentConfig;

// Sends an EnrollRequest (with config.enrollment_token, SEC-05) to the server
// enrollment port and polls until approved or stop_flag is set. Send failures
// and receive timeouts are retried. Throws std::runtime_error if rejected.
// SEC-08: the reply is untrusted. An approved reply must carry a server key
// whose fingerprint matches, and any configured key or fingerprint pin must
// match; otherwise it throws. An approval missing the key or fingerprint is
// accepted only when allow_insecure or allow_insecure_cli is set. On success it stores the
// server key and fingerprint in config.
void enroll(AgentConfig& config, zmq::context_t& ctx, const std::atomic<bool>& stop_flag,
            int poll_interval_seconds = 10, int recv_timeout_ms = 5000);

std::string get_hostname();
std::string get_platform_string();
std::string get_os_name();
std::string get_os_version();
double get_uptime_seconds();

} // namespace thewatcher::agent
