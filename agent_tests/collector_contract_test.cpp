#include "../agent/collectors/collector.hpp"
#include "../agent/collectors/network_collector.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace thewatcher;
using namespace thewatcher::agent;

namespace
{

class FakeCollector final : public Collector
{
public:
    std::string_view name() const noexcept override
    {
        return "fake";
    }

    void update(SystemMetrics& metrics) override
    {
        metrics.hostname = "updated-by-fake";
    }
};

} // namespace

SCENARIO("Collectors expose the agent metrics update contract")
{
    GIVEN("a collector referenced through the base interface")
    {
        FakeCollector fake;
        Collector& collector = fake;
        SystemMetrics metrics;

        WHEN("the agent asks the collector to update metrics")
        {
            collector.update(metrics);

            THEN("the collector can mutate the shared SystemMetrics snapshot")
            {
                REQUIRE(metrics.hostname == "updated-by-fake");
            }

            AND_THEN("the collector reports a stable name")
            {
                REQUIRE(collector.name() == "fake");
            }
        }
    }
}

SCENARIO("Windows network collector reports ipconfig-level adapters")
{
    GIVEN("adapter names and descriptions that appear in ipconfig output")
    {
        WHEN("the collector classifies visible Ethernet, Wi-Fi, Bluetooth, and vEthernet adapters")
        {
            THEN("they are treated as reportable adapters")
            {
                REQUIRE(detail::is_windows_ipconfig_adapter_name("Ethernet", "Realtek PCIe 5GbE Family Controller"));
                REQUIRE(detail::is_windows_ipconfig_adapter_name("vEthernet (External Switch)",
                                                                 "Hyper-V Virtual Ethernet Adapter #3"));
                REQUIRE(detail::is_windows_ipconfig_adapter_name(
                    "WiFi 2", "Qualcomm FastConnect 7800 Wi-Fi 7 High Band Simultaneous Network Adapter"));
                REQUIRE(detail::is_windows_ipconfig_adapter_name("Bluetooth Network Connection",
                                                                 "Bluetooth Device (Personal Area Network)"));
            }
        }

        WHEN("the collector classifies lower-layer Windows networking devices")
        {
            THEN("they are filtered out before metrics are reported")
            {
                REQUIRE_FALSE(
                    detail::is_windows_ipconfig_adapter_name("QoS Packet Scheduler", "Packet Scheduler Miniport"));
                REQUIRE_FALSE(detail::is_windows_ipconfig_adapter_name("WFP Native MAC Layer LightWeight Filter",
                                                                       "WFP Native MAC Layer LightWeight Filter"));
                REQUIRE_FALSE(detail::is_windows_ipconfig_adapter_name("Hyper-V Virtual Switch",
                                                                       "Hyper-V Virtual Switch Extension Adapter"));
                REQUIRE_FALSE(detail::is_windows_ipconfig_adapter_name("6to4 Adapter", "Microsoft 6to4 Adapter"));
                REQUIRE_FALSE(detail::is_windows_ipconfig_adapter_name("Network Bridge", "Microsoft Network Bridge"));
            }
        }
    }

    GIVEN("Windows interface type identifiers")
    {
        WHEN("the type is a user-visible link type")
        {
            THEN("the collector allows it")
            {
                REQUIRE(detail::is_windows_reportable_if_type(6));  // Ethernet
                REQUIRE(detail::is_windows_reportable_if_type(71)); // IEEE 802.11 Wi-Fi
            }
        }

        WHEN("the type is loopback or tunnel plumbing")
        {
            THEN("loopback is kept (flagged is_loopback) and tunnel plumbing is filtered out")
            {
                REQUIRE(detail::is_windows_reportable_if_type(24));        // Software loopback
                REQUIRE_FALSE(detail::is_windows_reportable_if_type(131)); // Tunnel
            }
        }
    }
}
