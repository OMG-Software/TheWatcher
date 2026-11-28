#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace thewatcher::server
{

// Reduces a ZeroMQ "Peer-Address" message property ("ip:port" or
// "[ipv6]:port") to a bare IP literal. Returns an empty string for anything
// that is not a valid IPv4 or IPv6 address, so the result is safe to store,
// log and put in JSON without further escaping. An IPv6 zone ("%eth0") is
// dropped. The value is the address the server saw: behind a NAT or proxy it
// is that device's address, not the agent's own.
inline std::string parse_peer_ip(std::string_view peer)
{
    constexpr std::size_t max_peer_length = 64;
    if (peer.empty() || peer.size() > max_peer_length)
        return {};

    std::string_view host = peer;
    std::string_view port;
    if (peer.front() == '[')
    {
        const auto close = peer.find(']');
        if (close == std::string_view::npos)
            return {};
        host = peer.substr(1, close - 1);
        const auto rest = peer.substr(close + 1);
        if (!rest.empty())
        {
            if (rest.front() != ':')
                return {};
            port = rest.substr(1);
        }
    }
    else if (const auto colon = peer.find(':');
             colon != std::string_view::npos && peer.find(':', colon + 1) == std::string_view::npos)
    {
        // Exactly one colon: IPv4 with a port. More colons is a bare IPv6 literal.
        host = peer.substr(0, colon);
        port = peer.substr(colon + 1);
    }

    // The port is not kept, but a malformed one means the value is not what
    // ZeroMQ produces, so reject it rather than guess.
    if (port.size() > 5)
        return {};
    for (const char c : port)
    {
        if (c < '0' || c > '9')
            return {};
    }

    std::string address(host);
    if (const auto zone = address.find('%'); zone != std::string::npos)
        address.erase(zone);
    if (address.empty() || address.find('\0') != std::string::npos)
        return {};

    unsigned char buffer[sizeof(in6_addr)];
    if (inet_pton(AF_INET, address.c_str(), buffer) == 1 || inet_pton(AF_INET6, address.c_str(), buffer) == 1)
        return address;
    return {};
}

} // namespace thewatcher::server
