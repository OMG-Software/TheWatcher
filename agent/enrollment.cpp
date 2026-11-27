#include "enrollment.hpp"

#include "common/SingleLog.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"
#include "common/zmq_limits.hpp"
#include "config.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <zmq.hpp>

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>
#else
#include <fstream>

#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace thewatcher::agent
{

using namespace thewatcher::proto;

std::string get_hostname()
{
    char buf[256] = {};
#ifdef _WIN32
    DWORD sz = sizeof(buf);
    if (!::GetComputerNameA(buf, &sz))
        return "unknown";
#else
    if (::gethostname(buf, sizeof(buf)) != 0)
        return "unknown";
#endif
    return buf;
}

std::string get_platform_string()
{
#if defined(__linux__)
    return "linux";
#elif defined(_WIN32)
    return "windows";
#elif defined(__FreeBSD__) || defined(__DragonFly__)
    return "freebsd";
#elif defined(__NetBSD__)
    return "netbsd";
#elif defined(__OpenBSD__)
    return "openbsd";
#else
    return "unknown";
#endif
}

std::string get_os_name()
{
#ifdef _WIN32
    return "Windows";
#elif defined(__linux__)
    std::ifstream f("/etc/os-release");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("PRETTY_NAME=", 0) == 0)
        {
            std::string name = line.substr(12);
            if (!name.empty() && name.front() == '"')
                name = name.substr(1);
            if (!name.empty() && name.back() == '"')
                name.pop_back();
            return name;
        }
    }
    return "Linux";
#else
    struct utsname u{};
    ::uname(&u);
    return u.sysname;
#endif
}

std::string get_os_version()
{
#ifndef _WIN32
    struct utsname u{};
    ::uname(&u);
    return u.release;
#else
    return "Windows NT";
#endif
}

double get_uptime_seconds()
{
#ifdef __linux__
    std::ifstream f("/proc/uptime");
    double secs = 0.0;
    f >> secs;
    return secs;
#elif defined(_WIN32)
    return static_cast<double>(::GetTickCount64()) / 1000.0;
#elif defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__) || defined(__DragonFly__)
    struct timeval boottime{};
    size_t len = sizeof(boottime);
    ::sysctlbyname("kern.boottime", &boottime, &len, nullptr, 0);
    struct timeval now{};
    ::gettimeofday(&now, nullptr);
    return static_cast<double>(now.tv_sec - boottime.tv_sec) +
           static_cast<double>(now.tv_usec - boottime.tv_usec) / 1e6;
#else
    return 0.0;
#endif
}

namespace
{
    // Longest uninterrupted wait: bounds how late a stop request is noticed.
    constexpr auto kStopCheckSlice = std::chrono::milliseconds{50};

    // Sleeps for `seconds`, returning early once `stop_flag` is set (issue #83).
    void sleep_unless_stopped(const std::atomic<bool>& stop_flag, int seconds)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        while (!stop_flag.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(kStopCheckSlice);
    }

    // Receives one message, waiting at most `timeout_ms` and giving up early once
    // `stop_flag` is set. Returns an empty result on timeout or stop.
    zmq::recv_result_t recv_unless_stopped(zmq::socket_t& socket, zmq::message_t& message,
                                           const std::atomic<bool>& stop_flag, int timeout_ms)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!stop_flag.load())
        {
            zmq::pollitem_t item{static_cast<void*>(socket), 0, ZMQ_POLLIN, 0};
            zmq::poll(&item, 1, kStopCheckSlice);
            if (item.revents & ZMQ_POLLIN)
                return socket.recv(message, zmq::recv_flags::dontwait);
            if (std::chrono::steady_clock::now() >= deadline)
                break;
        }
        return std::nullopt;
    }

    // True if key is exactly 40 Z85 characters that decode to a 32-byte CURVE key.
    // The enrollment reply is untrusted text that ends up in the config file, so
    // anything else must never be stored (issue #148).
    bool is_valid_curve_public_key(const std::string& key)
    {
        try
        {
            thewatcher::crypto::z85_decode(key);
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

    // Makes one request/reply exchange on a fresh REQ socket. A REQ socket that
    // has sent without receiving a reply, or received a bad one, cannot send again
    // (EFSM), so every attempt gets its own socket instead of reusing one with
    // ZMQ_REQ_RELAXED. That also drops any stale reply a late server sends to an
    // earlier attempt, which ZMQ_REQ_CORRELATE would otherwise have to filter.
    // Returns the decoded reply, or nullopt after logging why there is none
    // (send failure, timeout or a reply that does not decode).
    std::optional<EnrollResponse> request_enrollment(const AgentConfig& config, zmq::context_t& ctx,
                                                     const std::atomic<bool>& stop_flag,
                                                     const std::vector<uint8_t>& encoded, int recv_timeout_ms)
    {
        try
        {
            zmq::socket_t req(ctx, ZMQ_REQ);
            req.set(zmq::sockopt::linger, 0);
            req.set(zmq::sockopt::rcvtimeo, recv_timeout_ms);
            req.set(zmq::sockopt::sndtimeo, recv_timeout_ms);
            // SEC-02: enrollment replies are tiny; reject anything oversize at the
            // socket layer and bound the handshake against a stalling server.
            req.set(zmq::sockopt::maxmsgsize, kEnrollMaxMessageBytes);
            req.set(zmq::sockopt::handshake_ivl, kHandshakeIvlMs);
            req.connect(config.enrollment_address);

            LOGF_TRACE("Sending enrollment request to %s", config.enrollment_address.c_str());
            req.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);

            zmq::message_t resp;
            // Interruptible: a stop request ends the wait within one slice.
            if (!recv_unless_stopped(req, resp, stop_flag, recv_timeout_ms))
            {
                LOG_DEBUG("Enrollment response timed out");
                return std::nullopt;
            }

            // The reply comes from an unauthenticated peer: a malformed one is
            // dropped and retried, never fatal (issue #160).
            const auto resp_frame = decode_frame(resp.data(), resp.size());
            return unpack<EnrollResponse>(resp_frame.payload);
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("Enrollment attempt failed: %s", e.what());
            return std::nullopt;
        }
    }

} // namespace

// Sends an EnrollRequest and polls until approved or stop_flag is set. Retries on
// send failure, receive timeout and malformed replies, each on a fresh socket.
// Throws std::runtime_error if rejected, if an approved reply lacks a valid
// server key pin (unless insecure mode is on), carries an invalid key, or if a
// fingerprint or configured key pin does not match. Stores the server key on success.
void enroll(AgentConfig& config, zmq::context_t& ctx, const std::atomic<bool>& stop_flag, int poll_interval_seconds,
            int recv_timeout_ms)
{
    LOG_FUNCTION_TRACE
    LOGF_INFO("Enrollment started agent_id=%s endpoint=%s poll_interval=%d", config.agent_id.c_str(),
              config.enrollment_address.c_str(), poll_interval_seconds);

    EnrollRequest er;
    er.agent_id = config.agent_id;
    er.hostname = get_hostname();
    er.platform = get_platform_string();
    er.curve_public_key_z85 = config.agent_public_key;
    er.enrollment_token = config.enrollment_token;
    LOGF_DEBUG("Enrollment request prepared hostname=%s platform=%s public_key=%s", er.hostname.c_str(),
               er.platform.c_str(), er.curve_public_key_z85.c_str());

    Frame f;
    f.type = static_cast<uint8_t>(FrameType::ENROLL_REQUEST);
    f.agent_id = config.agent_id;
    f.timestamp_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    f.payload = pack(er);

    auto encoded = encode_frame(f);
    LOGF_TRACE("Encoded enrollment request bytes=%zu timestamp_ms=%lld", encoded.size(),
               static_cast<long long>(f.timestamp_ms));

    while (!stop_flag.load())
    {
        const auto reply = request_enrollment(config, ctx, stop_flag, encoded, recv_timeout_ms);
        if (!reply)
        {
            LOGF_DEBUG("No usable enrollment reply; retrying in %d seconds", poll_interval_seconds);
            sleep_unless_stopped(stop_flag, poll_interval_seconds);
            continue;
        }
        const auto& er_resp = *reply;
        LOGF_DEBUG("Enrollment response approved=%d message=%s", er_resp.approved ? 1 : 0, er_resp.message.c_str());

        if (er_resp.approved)
        {
            // SEC-08: the enrollment reply travels on the unauthenticated REP
            // socket, so nothing in it is trusted by default. An approved
            // reply must present a key plus a matching fingerprint; a keyless
            // reply is a hard error unless the operator explicitly opted into
            // insecure mode — the data plane is never silently downgraded to
            // plaintext.
            if (er_resp.server_public_key_z85.empty() || er_resp.server_public_key_fingerprint.empty())
            {
                if (!config.allow_insecure && !config.allow_insecure_cli)
                {
                    LOG_WARNING("Approved enrollment response did not include a server key pin");
                    throw std::runtime_error("Approved enrollment response missing server key pin");
                }
                LOG_WARNING(
                    "Insecure mode: enrollment approved without server key; data socket encryption is disabled");
                LOG_INFO("Enrollment approved");
                return;
            }

            if (!is_valid_curve_public_key(er_resp.server_public_key_z85))
            {
                LOG_WARNING("Enrollment server key is not a valid Z85 CURVE public key");
                throw std::runtime_error("Enrollment server key is not a valid CURVE public key");
            }

            const auto calculated_fingerprint =
                thewatcher::crypto::server_public_key_fingerprint(er_resp.server_public_key_z85);
            if (calculated_fingerprint != er_resp.server_public_key_fingerprint)
            {
                LOGF_WARNING("Enrollment server key fingerprint mismatch calculated=%s received=%s",
                             calculated_fingerprint.c_str(), er_resp.server_public_key_fingerprint.c_str());
                throw std::runtime_error("Enrollment server key fingerprint does not match public key");
            }

            // SEC-08: a configured server key is a pin, not a default — the
            // unauthenticated reply must confirm it, never replace it.
            if (!config.server_public_key.empty() && config.server_public_key != er_resp.server_public_key_z85)
            {
                LOGF_WARNING("Pinned server key mismatch pinned=%s received=%s", config.server_public_key.c_str(),
                             er_resp.server_public_key_z85.c_str());
                throw std::runtime_error("Enrollment server key does not match configured key");
            }

            if (!config.server_public_key_fingerprint.empty() &&
                config.server_public_key_fingerprint != er_resp.server_public_key_fingerprint)
            {
                LOGF_WARNING("Pinned server fingerprint mismatch pinned=%s received=%s",
                             config.server_public_key_fingerprint.c_str(),
                             er_resp.server_public_key_fingerprint.c_str());
                throw std::runtime_error("Pinned server fingerprint mismatch");
            }

            config.server_public_key = er_resp.server_public_key_z85;
            config.server_public_key_fingerprint = er_resp.server_public_key_fingerprint;
            LOG_INFO("Enrollment approved");
            return;
        }

        if (er_resp.message.find("rejected") != std::string::npos)
        {
            LOGF_WARNING("Enrollment rejected message=%s", er_resp.message.c_str());
            throw std::runtime_error("Enrollment rejected: " + er_resp.message);
        }

        LOGF_INFO("Enrollment pending; retrying in %d seconds", poll_interval_seconds);
        sleep_unless_stopped(stop_flag, poll_interval_seconds);
    }

    LOG_WARNING("Enrollment stopped before approval");
}

} // namespace thewatcher::agent
