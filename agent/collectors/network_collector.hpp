#pragma once

#include "collector.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace thewatcher::agent
{

namespace detail
{
    bool is_windows_ipconfig_adapter_name(std::string_view name, std::string_view description);
    bool is_windows_reportable_if_type(unsigned int if_type);

    // Growth of a monotonically increasing counter between two samples. A
    // counter that went backwards was reset (driver reload, interface
    // re-created), so the new value is what accumulated since the reset.
    uint64_t counter_delta(uint64_t previous, uint64_t current);

    // counter_delta divided by the elapsed seconds; 0 when no time has passed.
    uint64_t per_second(uint64_t previous, uint64_t current, double elapsed_seconds);

    // Formats a hardware address as lower-case "aa:bb:cc:dd:ee:ff". Returns an
    // empty string unless it is exactly six bytes and not all zero.
    std::string format_mac(const unsigned char* address, std::size_t length);

    // Picks the address to display from an interface's textual addresses:
    // global IPv4, then global IPv6, then link-local IPv4, then link-local IPv6.
    std::string choose_primary_address(const std::vector<std::string>& addresses);

    // One interface as read from the OS: cumulative counters plus identity and
    // state. NetworkCollector::process_readings turns these into per-interval
    // NetworkMetrics.
    struct InterfaceReading
    {
        std::string name;
        uint64_t rx_bytes = 0;
        uint64_t tx_bytes = 0;
        uint64_t rx_packets = 0;
        uint64_t tx_packets = 0;
        uint64_t rx_errors = 0;
        uint64_t tx_errors = 0;
        uint64_t rx_drops = 0;
        uint64_t tx_drops = 0;
        std::string ip_address;
        std::string mac_address;
        bool is_up = false;
        bool is_loopback = false;
    };
} // namespace detail

class NetworkCollector final : public Collector
{
public:
    std::string_view name() const noexcept override
    {
        return "network";
    }
    void update(SystemMetrics& metrics) override;

    // Converts cumulative OS readings taken at `now` into metrics: byte and
    // packet rates per second, and error/drop counts as the change since the
    // previous call (0 on the first sample of an interface), then remembers
    // the readings for the next call.
    std::vector<NetworkMetrics> process_readings(const std::vector<detail::InterfaceReading>& readings,
                                                 std::chrono::steady_clock::time_point now);

private:
    struct InterfaceSample
    {
        uint64_t rx_bytes = 0;
        uint64_t tx_bytes = 0;
        uint64_t rx_packets = 0;
        uint64_t tx_packets = 0;
        uint64_t rx_errors = 0;
        uint64_t tx_errors = 0;
        uint64_t rx_drops = 0;
        uint64_t tx_drops = 0;
        std::chrono::steady_clock::time_point sampled_at{};
    };

    std::unordered_map<std::string, InterfaceSample> prev_;
};

} // namespace thewatcher::agent
