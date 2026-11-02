#include "enrollment.hpp"

#include "common/SingleLog.hpp"
#include "common/crypto.hpp"
#include "common/protocol.hpp"
#include "common/zmq_limits.hpp"
#include "config.hpp"

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

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

// Sends an EnrollRequest and polls until approved or stop_flag is set. Retries on
// send failure and receive timeout. Throws std::runtime_error if rejected, if an
// approved reply lacks a valid server key pin (unless insecure mode is on), or if
// a fingerprint or configured key pin does not match. Stores the server key on success.
void enroll(AgentConfig& config, zmq::context_t& ctx, const std::atomic<bool>& stop_flag, int poll_interval_seconds,
            int recv_timeout_ms)
{
    LOG_FUNCTION_TRACE
    LOGF_INFO("Enrollment started agent_id=%s endpoint=%s poll_interval=%d", config.agent_id.c_str(),
              config.enrollment_address.c_str(), poll_interval_seconds);

    zmq::socket_t req(ctx, ZMQ_REQ);
    req.set(zmq::sockopt::linger, 0);
    req.set(zmq::sockopt::rcvtimeo, recv_timeout_ms);
    // SEC-02: enrollment replies are tiny; reject anything oversize at the
    // socket layer and bound the handshake against a stalling server.
    req.set(zmq::sockopt::maxmsgsize, kEnrollMaxMessageBytes);
    req.set(zmq::sockopt::handshake_ivl, kHandshakeIvlMs);
    req.connect(config.enrollment_address);

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
        try
        {
            LOGF_TRACE("Sending enrollment request to %s", config.enrollment_address.c_str());
            req.send(zmq::message_t(encoded.data(), encoded.size()), zmq::send_flags::none);
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("Enrollment send failed: %s", e.what());
            std::this_thread::sleep_for(std::chrono::seconds(poll_interval_seconds));
            continue;
        }

        zmq::message_t resp;
        auto rc = req.recv(resp, zmq::recv_flags::none);
        if (!rc)
        {
            LOGF_DEBUG("Enrollment response timed out; retrying in %d seconds", poll_interval_seconds);
            std::this_thread::sleep_for(std::chrono::seconds(poll_interval_seconds));
            continue;
        }

        auto resp_frame = decode_frame(resp.data(), resp.size());
        auto er_resp = unpack<EnrollResponse>(resp_frame.payload);
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
        std::this_thread::sleep_for(std::chrono::seconds(poll_interval_seconds));
    }

    LOG_WARNING("Enrollment stopped before approval");
}

} // namespace thewatcher::agent
