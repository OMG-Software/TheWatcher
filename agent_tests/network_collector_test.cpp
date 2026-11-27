#include "agent/collectors/network_collector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::agent;
using namespace std::chrono_literals;

namespace
{
detail::InterfaceReading reading(uint64_t rx_bytes, uint64_t tx_bytes, uint64_t errors = 0, uint64_t drops = 0)
{
    detail::InterfaceReading r;
    r.name = "eth0";
    r.rx_bytes = rx_bytes;
    r.tx_bytes = tx_bytes;
    r.rx_packets = rx_bytes / 100;
    r.tx_packets = tx_bytes / 100;
    r.rx_errors = errors;
    r.tx_errors = errors;
    r.rx_drops = drops;
    r.tx_drops = drops;
    r.ip_address = "192.0.2.10";
    r.mac_address = "aa:bb:cc:dd:ee:ff";
    r.is_up = true;
    return r;
}
} // namespace

SCENARIO("Counter deltas survive counter resets")
{
    GIVEN("a counter that grew between two samples")
    {
        THEN("the delta is the difference")
        {
            REQUIRE(detail::counter_delta(100, 150) == 50);
        }
    }

    GIVEN("a counter that went backwards because the interface was reset")
    {
        THEN("the delta is the value counted since the reset, not zero and not a huge unsigned wrap")
        {
            REQUIRE(detail::counter_delta(150, 40) == 40);
        }
    }

    GIVEN("a 64-bit octet counter that crosses the 32-bit boundary")
    {
        const uint64_t previous = 0xFFFF'FF00ULL;
        const uint64_t current = 0x1'0000'0100ULL;

        THEN("the delta is the true difference, not a 32-bit wrapped value")
        {
            REQUIRE(detail::counter_delta(previous, current) == 0x200);
        }
    }
}

SCENARIO("NetworkCollector turns cumulative counters into per-interval figures")
{
    GIVEN("a collector that has seen one sample of an interface")
    {
        NetworkCollector collector;
        const auto t0 = std::chrono::steady_clock::now();
        const auto first = collector.process_readings({reading(1'000, 2'000, 7, 3)}, t0);

        THEN("the first sample reports zero rates and zero error and drop deltas")
        {
            REQUIRE(first.size() == 1);
            REQUIRE(first[0].bytes_recv_per_sec == 0);
            REQUIRE(first[0].bytes_sent_per_sec == 0);
            REQUIRE(first[0].errors_in == 0);
            REQUIRE(first[0].drops_out == 0);
        }

        WHEN("a second sample arrives two seconds later with more traffic, errors and drops")
        {
            const auto second = collector.process_readings({reading(5'000, 2'400, 12, 4)}, t0 + 2s);

            THEN("rates are the byte delta divided by the elapsed time")
            {
                REQUIRE(second[0].bytes_recv_per_sec == 2'000);
                REQUIRE(second[0].bytes_sent_per_sec == 200);
            }

            AND_THEN("errors and drops are the change since the previous sample, not the running total")
            {
                REQUIRE(second[0].errors_in == 5);
                REQUIRE(second[0].errors_out == 5);
                REQUIRE(second[0].drops_in == 1);
                REQUIRE(second[0].drops_out == 1);
            }

            AND_WHEN("a third sample has no new errors")
            {
                const auto third = collector.process_readings({reading(5'000, 2'400, 12, 4)}, t0 + 4s);

                THEN("the error and drop deltas return to zero")
                {
                    REQUIRE(third[0].errors_in == 0);
                    REQUIRE(third[0].drops_in == 0);
                    REQUIRE(third[0].bytes_recv_per_sec == 0);
                }
            }
        }

        WHEN("the interface counters reset below their previous values")
        {
            const auto second = collector.process_readings({reading(400, 600, 1, 0)}, t0 + 2s);

            THEN("the rate counts what accumulated since the reset instead of reporting zero")
            {
                REQUIRE(second[0].bytes_recv_per_sec == 200);
                REQUIRE(second[0].bytes_sent_per_sec == 300);
                REQUIRE(second[0].errors_in == 1);
            }
        }
    }

    GIVEN("a reading with address, MAC, link state and loopback flag")
    {
        NetworkCollector collector;
        auto lo = reading(0, 0);
        lo.name = "lo0";
        lo.is_loopback = true;
        lo.is_up = false;

        WHEN("it is processed")
        {
            const auto out = collector.process_readings({lo, reading(0, 0)}, std::chrono::steady_clock::now());

            THEN("identity and state fields pass through unchanged")
            {
                REQUIRE(out.size() == 2);
                REQUIRE(out[0].interface_name == "lo0");
                REQUIRE(out[0].is_loopback);
                REQUIRE_FALSE(out[0].is_up);
                REQUIRE(out[1].ip_address == "192.0.2.10");
                REQUIRE(out[1].mac_address == "aa:bb:cc:dd:ee:ff");
                REQUIRE(out[1].is_up);
                REQUIRE_FALSE(out[1].is_loopback);
            }
        }
    }
}

SCENARIO("NetworkCollector keeps history across renames and unreadable counters")
{
    GIVEN("a Windows-style adapter identified by a stable key whose friendly name later changes")
    {
        NetworkCollector collector;
        const auto t0 = std::chrono::steady_clock::now();
        auto first = reading(1'000, 0);
        first.key = "12";
        first.name = "Ethernet";
        collector.process_readings({first}, t0);

        WHEN("the adapter is renamed between samples")
        {
            auto second = reading(3'000, 0);
            second.key = "12";
            second.name = "Ethernet (renamed)";
            const auto out = collector.process_readings({second}, t0 + 2s);

            THEN("its rate is computed against the earlier sample and the new name is shown")
            {
                REQUIRE(out[0].interface_name == "Ethernet (renamed)");
                REQUIRE(out[0].bytes_recv_per_sec == 1'000);
            }
        }
    }

    GIVEN("a collector with a baseline sample")
    {
        NetworkCollector collector;
        const auto t0 = std::chrono::steady_clock::now();
        collector.process_readings({reading(1'000, 0, 5, 5)}, t0);

        WHEN("one sample has unreadable counters (zeros that are not real)")
        {
            auto bad = reading(0, 0);
            bad.counters_valid = false;
            const auto skipped = collector.process_readings({bad}, t0 + 2s);
            const auto next = collector.process_readings({reading(5'000, 0, 5, 5)}, t0 + 4s);

            THEN("the bad sample reports no rates or deltas")
            {
                REQUIRE(skipped[0].bytes_recv_per_sec == 0);
                REQUIRE(skipped[0].errors_in == 0);
            }

            AND_THEN("the next good sample is measured against the old baseline, not against zero")
            {
                REQUIRE(next[0].bytes_recv_per_sec == 1'000);
                REQUIRE(next[0].errors_in == 0);
            }
        }
    }
}

SCENARIO("Link-layer and IP addresses are formatted for display")
{
    GIVEN("a six-byte hardware address")
    {
        const unsigned char mac[] = {0x00, 0x1A, 0x2b, 0xCC, 0x0d, 0xFF};

        THEN("it renders as lower-case colon separated hex")
        {
            REQUIRE(detail::format_mac(mac, sizeof(mac)) == "00:1a:2b:cc:0d:ff");
        }
    }

    GIVEN("an all-zero or wrong-length hardware address")
    {
        const unsigned char zero[] = {0, 0, 0, 0, 0, 0};
        const unsigned char short_addr[] = {1, 2, 3};

        THEN("no MAC is reported")
        {
            REQUIRE(detail::format_mac(zero, sizeof(zero)).empty());
            REQUIRE(detail::format_mac(short_addr, sizeof(short_addr)).empty());
        }
    }

    GIVEN("several addresses on one interface")
    {
        THEN("a global IPv4 address is preferred over IPv6 and link-local addresses")
        {
            const std::vector<std::string> addresses = {"fe80::1", "2001:db8::5", "192.0.2.10"};
            REQUIRE(detail::choose_primary_address(addresses) == "192.0.2.10");
        }

        THEN("a global IPv6 address is preferred over a link-local one when there is no IPv4")
        {
            const std::vector<std::string> addresses = {"fe80::1", "2001:db8::5"};
            REQUIRE(detail::choose_primary_address(addresses) == "2001:db8::5");
        }

        THEN("a link-local address is used when it is all there is")
        {
            const std::vector<std::string> addresses = {"fe80::1", "169.254.3.4"};
            REQUIRE(detail::choose_primary_address(addresses) == "169.254.3.4");
        }

        THEN("only fe80::/10 counts as IPv6 link-local, including febf:: but not fec0:: or fe8:: lookalikes")
        {
            // febf::1 is inside fe80::/10 (link-local); fec0::1 is outside it.
            REQUIRE(detail::choose_primary_address({"febf::1", "2001:db8::7"}) == "2001:db8::7");
            REQUIRE(detail::choose_primary_address({"fec0::1", "febf::1"}) == "fec0::1");
            REQUIRE(detail::choose_primary_address({"fe8::1", "febf::1"}) == "fe8::1");
        }

        THEN("no addresses yields an empty string")
        {
            REQUIRE(detail::choose_primary_address({}).empty());
        }
    }
}

#if !defined(_WIN32)

SCENARIO("On Linux and BSD the network collector reports loopback with its address and state")
{
    GIVEN("a fresh NetworkCollector on a POSIX host")
    {
        NetworkCollector collector;
        SystemMetrics metrics;

        WHEN("the collector runs")
        {
            collector.update(metrics);

            THEN("a loopback interface is reported as up with 127.0.0.1")
            {
                bool found = false;
                for (const auto& net : metrics.networks)
                {
                    if (!net.is_loopback)
                        continue;
                    found = true;
                    REQUIRE(net.is_up);
                    REQUIRE(net.ip_address == "127.0.0.1");
                }
                REQUIRE(found);
            }
        }
    }
}

#endif
