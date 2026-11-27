#include "network_collector.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#if !defined(_WIN32)
// clang-format off
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
// clang-format on
#endif

#if defined(__linux__)
#include <netpacket/packet.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__) ||                 \
    defined(__APPLE__)
#define THEWATCHER_NETWORK_BSD 1
#include <net/if_dl.h>
#endif

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
// clang-format on
#endif

namespace thewatcher::agent
{

namespace detail
{
    namespace
    {
        std::string lowercase_ascii(std::string_view value)
        {
            std::string out(value);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
                return static_cast<char>(std::tolower(ch));
            });
            return out;
        }

        bool contains(std::string_view haystack, std::string_view needle)
        {
            return haystack.find(needle) != std::string_view::npos;
        }
    } // namespace

    bool is_windows_ipconfig_adapter_name(std::string_view name, std::string_view description)
    {
        const auto haystack = lowercase_ascii(std::string(name) + " " + std::string(description));
        return !contains(haystack, "packet scheduler") && !contains(haystack, "wfp native mac layer") &&
               !contains(haystack, "lightweight filter") && !contains(haystack, "hyper-v virtual switch") &&
               !contains(haystack, "network bridge") && !contains(haystack, "6to4 adapter") &&
               !contains(haystack, "isatap") && !contains(haystack, "teredo") && !contains(haystack, "wan miniport");
    }

    bool is_windows_reportable_if_type(unsigned int if_type)
    {
        constexpr unsigned int ethernet_csmacd = 6;
        constexpr unsigned int ppp = 23;
        constexpr unsigned int ieee80211 = 71;
        return if_type == ethernet_csmacd || if_type == ppp || if_type == ieee80211;
    }

    uint64_t counter_delta(uint64_t previous, uint64_t current)
    {
        // current < previous means the counter was reset; what it now holds
        // is the amount accumulated since the reset.
        return current >= previous ? current - previous : current;
    }

    uint64_t per_second(uint64_t previous, uint64_t current, double elapsed_seconds)
    {
        if (elapsed_seconds <= 0.0)
        {
            return 0;
        }
        return static_cast<uint64_t>(static_cast<double>(counter_delta(previous, current)) / elapsed_seconds);
    }

    std::string format_mac(const unsigned char* address, std::size_t length)
    {
        if (address == nullptr || length != 6 || std::all_of(address, address + length, [](unsigned char b) {
                return b == 0;
            }))
        {
            return {};
        }
        std::string out;
        char byte[3];
        for (std::size_t i = 0; i < length; ++i)
        {
            std::snprintf(byte, sizeof(byte), "%02x", address[i]);
            if (i != 0)
                out += ':';
            out += byte;
        }
        return out;
    }

    std::string choose_primary_address(const std::vector<std::string>& addresses)
    {
        // Lower rank wins; ties keep the first address seen.
        const auto rank = [](const std::string& address) {
            const bool v6 = address.find(':') != std::string::npos;
            const bool link_local =
                v6 ? lowercase_ascii(address).rfind("fe80", 0) == 0 : address.rfind("169.254.", 0) == 0;
            return (link_local ? 2 : 0) + (v6 ? 1 : 0);
        };
        const std::string* best = nullptr;
        for (const auto& address : addresses)
        {
            if (best == nullptr || rank(address) < rank(*best))
                best = &address;
        }
        return best != nullptr ? *best : std::string{};
    }
} // namespace detail

namespace
{

#ifdef __linux__

    // Cumulative counters from /proc/net/dev. Linux exposes 64-bit values
    // here; getifaddrs only offers the 32-bit rtnl_link_stats.
    struct ProcCounters
    {
        uint64_t rx_bytes = 0;
        uint64_t rx_packets = 0;
        uint64_t rx_errors = 0;
        uint64_t rx_drops = 0;
        uint64_t tx_bytes = 0;
        uint64_t tx_packets = 0;
        uint64_t tx_errors = 0;
        uint64_t tx_drops = 0;
    };

    std::unordered_map<std::string, ProcCounters> read_proc_net_dev()
    {
        std::unordered_map<std::string, ProcCounters> interfaces;
        std::ifstream netdev("/proc/net/dev");
        std::string line;
        std::getline(netdev, line);
        std::getline(netdev, line);

        while (std::getline(netdev, line))
        {
            const auto colon = line.find(':');
            if (colon == std::string::npos)
            {
                continue;
            }

            std::string name = line.substr(0, colon);
            name.erase(std::remove_if(name.begin(), name.end(), ::isspace), name.end());

            ProcCounters counters;
            std::istringstream row(line.substr(colon + 1));
            row >> counters.rx_bytes >> counters.rx_packets >> counters.rx_errors >> counters.rx_drops;

            uint64_t ignored = 0;
            row >> ignored >> ignored >> ignored >> ignored;

            row >> counters.tx_bytes >> counters.tx_packets >> counters.tx_errors >> counters.tx_drops;

            if (!name.empty())
            {
                interfaces[name] = counters;
            }
        }

        return interfaces;
    }

#endif

#if !defined(_WIN32)

    struct IfAddrsDeleter
    {
        void operator()(ifaddrs* list) const noexcept
        {
            ::freeifaddrs(list);
        }
    };
    using IfAddrsList = std::unique_ptr<ifaddrs, IfAddrsDeleter>;

    std::string address_to_string(const sockaddr& address)
    {
        std::array<char, INET6_ADDRSTRLEN> buffer{};
        if (address.sa_family == AF_INET)
        {
            const auto& in = reinterpret_cast<const sockaddr_in&>(address);
            return ::inet_ntop(AF_INET, &in.sin_addr, buffer.data(), buffer.size()) ? buffer.data() : "";
        }
        if (address.sa_family == AF_INET6)
        {
            const auto& in6 = reinterpret_cast<const sockaddr_in6&>(address);
            return ::inet_ntop(AF_INET6, &in6.sin6_addr, buffer.data(), buffer.size()) ? buffer.data() : "";
        }
        return {};
    }

    // Reads every interface through getifaddrs: link state from the flags,
    // IPv4/IPv6 addresses from AF_INET/AF_INET6 entries, and the MAC from the
    // link-layer entry (AF_PACKET on Linux, AF_LINK on the BSDs). On the BSDs
    // the AF_LINK entry also carries the if_data counters; on Linux the
    // counters are filled in from /proc/net/dev by the caller.
    std::vector<detail::InterfaceReading> read_getifaddrs()
    {
        std::vector<detail::InterfaceReading> readings;
        std::unordered_map<std::string, std::vector<std::string>> addresses;

        ifaddrs* raw = nullptr;
        if (::getifaddrs(&raw) != 0)
        {
            return readings;
        }
        const IfAddrsList list(raw);

        std::unordered_map<std::string, std::size_t> index;
        for (const ifaddrs* entry = list.get(); entry != nullptr; entry = entry->ifa_next)
        {
            if (entry->ifa_name == nullptr)
                continue;
            const std::string name = entry->ifa_name;

            auto [slot, inserted] = index.try_emplace(name, readings.size());
            if (inserted)
            {
                detail::InterfaceReading reading;
                reading.name = name;
                reading.is_up = (entry->ifa_flags & IFF_UP) != 0 && (entry->ifa_flags & IFF_RUNNING) != 0;
                reading.is_loopback = (entry->ifa_flags & IFF_LOOPBACK) != 0;
                readings.push_back(std::move(reading));
            }
            [[maybe_unused]] auto& reading = readings[slot->second];

            if (entry->ifa_addr == nullptr)
                continue;
            const int family = entry->ifa_addr->sa_family;
            if (family == AF_INET || family == AF_INET6)
            {
                const auto text = address_to_string(*entry->ifa_addr);
                if (!text.empty())
                    addresses[name].push_back(text);
            }
#if defined(__linux__)
            else if (family == AF_PACKET)
            {
                const auto& link = reinterpret_cast<const sockaddr_ll&>(*entry->ifa_addr);
                reading.mac_address = detail::format_mac(link.sll_addr, link.sll_halen);
            }
#elif defined(THEWATCHER_NETWORK_BSD)
            else if (family == AF_LINK)
            {
                const auto& link = reinterpret_cast<const sockaddr_dl&>(*entry->ifa_addr);
                reading.mac_address =
                    detail::format_mac(reinterpret_cast<const unsigned char*>(LLADDR(&link)), link.sdl_alen);
                if (entry->ifa_data != nullptr)
                {
                    const auto& data = *static_cast<const if_data*>(entry->ifa_data);
                    reading.rx_bytes = data.ifi_ibytes;
                    reading.tx_bytes = data.ifi_obytes;
                    reading.rx_packets = data.ifi_ipackets;
                    reading.tx_packets = data.ifi_opackets;
                    reading.rx_errors = data.ifi_ierrors;
                    reading.tx_errors = data.ifi_oerrors;
                    // Input queue drops only: if_data has no portable output drop counter.
                    reading.rx_drops = data.ifi_iqdrops;
                }
            }
#endif
        }

        for (auto& reading : readings)
        {
            const auto found = addresses.find(reading.name);
            if (found != addresses.end())
                reading.ip_address = detail::choose_primary_address(found->second);
        }
        return readings;
    }

    std::vector<detail::InterfaceReading> read_interfaces()
    {
        auto readings = read_getifaddrs();
#ifdef __linux__
        const auto counters = read_proc_net_dev();
        if (readings.empty())
        {
            // getifaddrs failed: fall back to the interfaces /proc knows about.
            for (const auto& entry : counters)
            {
                detail::InterfaceReading reading;
                reading.name = entry.first;
                reading.is_up = true;
                readings.push_back(std::move(reading));
            }
        }
        for (auto& reading : readings)
        {
            const auto found = counters.find(reading.name);
            if (found == counters.end())
                continue;
            reading.rx_bytes = found->second.rx_bytes;
            reading.rx_packets = found->second.rx_packets;
            reading.rx_errors = found->second.rx_errors;
            reading.rx_drops = found->second.rx_drops;
            reading.tx_bytes = found->second.tx_bytes;
            reading.tx_packets = found->second.tx_packets;
            reading.tx_errors = found->second.tx_errors;
            reading.tx_drops = found->second.tx_drops;
        }
#endif
        return readings;
    }

#endif

#ifdef _WIN32
    struct WindowsAdapterInfo
    {
        unsigned long if_index = 0;
        unsigned long if_type = 0;
        std::string name;
        std::string description;
        std::string ip_address;
        std::string mac_address;
    };

    std::string wide_to_utf8(const wchar_t* value)
    {
        if (value == nullptr || *value == L'\0')
            return {};
        const int needed = ::WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
        if (needed <= 1)
            return {};
        std::string out(static_cast<size_t>(needed - 1), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), needed, nullptr, nullptr);
        return out;
    }

    std::string sockaddr_to_string(const SOCKADDR* address)
    {
        char buffer[INET6_ADDRSTRLEN] = {};
        if (address == nullptr)
            return {};
        if (address->sa_family == AF_INET)
        {
            const auto* in = reinterpret_cast<const sockaddr_in*>(address);
            return ::InetNtopA(AF_INET, const_cast<IN_ADDR*>(&in->sin_addr), buffer, sizeof(buffer)) ? buffer : "";
        }
        if (address->sa_family == AF_INET6)
        {
            const auto* in6 = reinterpret_cast<const sockaddr_in6*>(address);
            return ::InetNtopA(AF_INET6, const_cast<IN6_ADDR*>(&in6->sin6_addr), buffer, sizeof(buffer)) ? buffer : "";
        }
        return {};
    }

    std::string first_reportable_ip(const IP_ADAPTER_UNICAST_ADDRESS* first_unicast)
    {
        std::vector<std::string> addresses;
        for (auto* unicast = first_unicast; unicast != nullptr; unicast = unicast->Next)
        {
            const auto ip = sockaddr_to_string(unicast->Address.lpSockaddr);
            if (!ip.empty() && ip != "127.0.0.1" && ip != "::1")
                addresses.push_back(ip);
        }
        return detail::choose_primary_address(addresses);
    }

    std::vector<WindowsAdapterInfo> read_windows_ipconfig_adapters()
    {
        std::vector<WindowsAdapterInfo> result;
        ULONG size = 15 * 1024;
        std::vector<unsigned char> buffer(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        ULONG rc = ::GetAdaptersAddresses(AF_UNSPEC,
                                          GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                          nullptr, adapters, &size);
        if (rc == ERROR_BUFFER_OVERFLOW)
        {
            buffer.resize(size);
            adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
            rc = ::GetAdaptersAddresses(AF_UNSPEC,
                                        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                        nullptr, adapters, &size);
        }
        if (rc != NO_ERROR)
            return result;

        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next)
        {
            WindowsAdapterInfo info;
            info.if_index = adapter->IfIndex;
            info.if_type = adapter->IfType;
            info.name = wide_to_utf8(adapter->FriendlyName);
            info.description = wide_to_utf8(adapter->Description);
            if (info.name.empty())
                info.name = info.description;

            if (!detail::is_windows_reportable_if_type(adapter->IfType) ||
                !detail::is_windows_ipconfig_adapter_name(info.name, info.description))
            {
                continue;
            }

            info.ip_address = first_reportable_ip(adapter->FirstUnicastAddress);
            info.mac_address = detail::format_mac(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
            result.push_back(std::move(info));
        }
        return result;
    }

    // Reads 64-bit counters with GetIfEntry2 (MIB_IF_ROW2). The legacy
    // GetIfEntry/MIB_IFROW counters are 32-bit and wrap every ~34 s at 1 Gbit/s.
    std::vector<detail::InterfaceReading> read_interfaces()
    {
        constexpr unsigned long if_type_software_loopback = 24;
        std::vector<detail::InterfaceReading> readings;
        for (const auto& adapter : read_windows_ipconfig_adapters())
        {
            MIB_IF_ROW2 row{};
            row.InterfaceIndex = adapter.if_index;
            if (::GetIfEntry2(&row) != NO_ERROR)
                continue;

            detail::InterfaceReading reading;
            reading.name = adapter.name;
            reading.ip_address = adapter.ip_address;
            reading.mac_address = adapter.mac_address;
            reading.is_up = row.OperStatus == IfOperStatusUp;
            reading.is_loopback = adapter.if_type == if_type_software_loopback;
            reading.rx_bytes = row.InOctets;
            reading.tx_bytes = row.OutOctets;
            // Include non-unicast (broadcast/multicast) packets.
            reading.rx_packets = row.InUcastPkts + row.InNUcastPkts;
            reading.tx_packets = row.OutUcastPkts + row.OutNUcastPkts;
            reading.rx_errors = row.InErrors;
            reading.tx_errors = row.OutErrors;
            reading.rx_drops = row.InDiscards;
            reading.tx_drops = row.OutDiscards;
            readings.push_back(std::move(reading));
        }
        return readings;
    }
#endif

} // namespace

std::vector<NetworkMetrics> NetworkCollector::process_readings(const std::vector<detail::InterfaceReading>& readings,
                                                               std::chrono::steady_clock::time_point now)
{
    std::vector<NetworkMetrics> result;
    result.reserve(readings.size());
    std::unordered_map<std::string, InterfaceSample> current;

    for (const auto& reading : readings)
    {
        NetworkMetrics network;
        network.interface_name = reading.name;
        network.ip_address = reading.ip_address;
        network.mac_address = reading.mac_address;
        network.is_up = reading.is_up;
        network.is_loopback = reading.is_loopback;

        const auto previous = prev_.find(reading.name);
        if (previous != prev_.end())
        {
            const auto& before = previous->second;
            const auto elapsed = std::chrono::duration<double>(now - before.sampled_at).count();
            network.bytes_recv_per_sec = detail::per_second(before.rx_bytes, reading.rx_bytes, elapsed);
            network.bytes_sent_per_sec = detail::per_second(before.tx_bytes, reading.tx_bytes, elapsed);
            network.packets_recv_per_sec = detail::per_second(before.rx_packets, reading.rx_packets, elapsed);
            network.packets_sent_per_sec = detail::per_second(before.tx_packets, reading.tx_packets, elapsed);
            // Errors and drops are reported as the change since the last
            // sample so a historic blip does not look like a live fault (#152).
            network.errors_in = detail::counter_delta(before.rx_errors, reading.rx_errors);
            network.errors_out = detail::counter_delta(before.tx_errors, reading.tx_errors);
            network.drops_in = detail::counter_delta(before.rx_drops, reading.rx_drops);
            network.drops_out = detail::counter_delta(before.tx_drops, reading.tx_drops);
        }

        InterfaceSample sample;
        sample.rx_bytes = reading.rx_bytes;
        sample.tx_bytes = reading.tx_bytes;
        sample.rx_packets = reading.rx_packets;
        sample.tx_packets = reading.tx_packets;
        sample.rx_errors = reading.rx_errors;
        sample.tx_errors = reading.tx_errors;
        sample.rx_drops = reading.rx_drops;
        sample.tx_drops = reading.tx_drops;
        sample.sampled_at = now;
        current[reading.name] = sample;
        result.push_back(std::move(network));
    }

    // Replacing the map also forgets interfaces that have disappeared.
    prev_ = std::move(current);
    return result;
}

void NetworkCollector::update(SystemMetrics& metrics)
{
    LOG_FUNCTION_TRACE
    metrics.networks.clear();

#if defined(__linux__) || defined(_WIN32) || defined(THEWATCHER_NETWORK_BSD)
    metrics.networks = process_readings(read_interfaces(), std::chrono::steady_clock::now());
    LOGF_TRACE("Network collector updated interfaces=%zu", metrics.networks.size());
#else
    LOG_DEBUG("Network collector has no implementation for this platform");
#endif
}

} // namespace thewatcher::agent
