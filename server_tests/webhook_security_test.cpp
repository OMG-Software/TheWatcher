#include "../server/webhook_security.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using thewatcher::server::parse_webhook_url;

SCENARIO("Webhook URL parsing accepts only public HTTP webhook targets")
{
    GIVEN("a public HTTP webhook URL")
    {
        WHEN("the URL uses the default port")
        {
            const auto target = parse_webhook_url("http://hooks.example.com/alert");

            THEN("the target host, port, and path are parsed")
            {
                REQUIRE(target.has_value());
                REQUIRE(target->host == "hooks.example.com");
                REQUIRE(target->port == 80);
                REQUIRE(target->path == "/alert");
            }
        }

        WHEN("the URL includes an explicit port and query string")
        {
            const auto target = parse_webhook_url("http://hooks.example.com:8080/a?severity=red");

            THEN("the explicit port and full request path are preserved")
            {
                REQUIRE(target.has_value());
                REQUIRE(target->host == "hooks.example.com");
                REQUIRE(target->port == 8080);
                REQUIRE(target->path == "/a?severity=red");
            }
        }
    }

    GIVEN("webhook URLs that point at restricted or malformed targets")
    {
        const std::vector<std::string> rejected = {
            "https://hooks.example.com/alert",
            "http://localhost/alert",
            "http://127.0.0.1/alert",
            "http://10.0.0.5/alert",
            "http://100.64.0.1/alert",
            "http://169.254.169.254/latest/meta-data",
            "http://172.20.10.2/alert",
            "http://192.168.1.2/alert",
            "http://198.18.0.1/alert",
            "http://224.0.0.1/alert",
            "http://[::1]/alert",
            "http://[0:0:0:0:0:0:0:1]/alert",
            "http://[::]/alert",
            "http://[0:0:0:0:0:0:0:0]/alert",
            "http://[::ffff:127.0.0.1]/alert",
            "http://[fd00::1]/alert",
            "http://[fe80::1]/alert",
            "http://[2001:db8::1]/alert",
            "http://[2001:2::1]/alert",
            "http://user@hooks.example.com/alert",
            "http://hooks.example.com:0/alert",
            "http://hooks.example.com:70000/alert",
            "http://hooks.example.com:notaport/alert",
        };

        WHEN("each URL is parsed")
        {
            THEN("the parser rejects it")
            {
                for (const auto& url : rejected)
                {
                    REQUIRE_FALSE(parse_webhook_url(url).has_value());
                }
            }
        }
    }
}

SCENARIO("Webhook URL parsing rejects legacy IPv4 forms and IPv6 transition prefixes (issue #142)")
{
    GIVEN("hosts that getaddrinfo() resolves to restricted addresses")
    {
        // Each entry resolves (glibc) to loopback, private, link-local or
        // "this host" addresses although it is not a canonical dotted quad.
        const std::vector<std::string> rejected = {
            "http://0251.0376.0251.0376/",  // octal 169.254.169.254
            "http://012.0.0.1/",            // octal 10.0.0.1
            "http://0177.0.0.1/",           // octal 127.0.0.1
            "http://0x7f.0.0.1/",           // hex part
            "http://0x7f000001/",           // single hex number
            "http://0X7F000001/",           // upper-case hex prefix
            "http://2130706433/",           // single decimal number
            "http://127.1/",                // two-part form
            "http://10.1/",                 // two-part form
            "http://0/",                    // 0.0.0.0
            "http://0:9000/",               // 0.0.0.0 with port
            "http://127.0.0.1./",           // trailing dot
            "http://8.8.8.8./",             // trailing dot (not canonical either)
            "http://[::127.0.0.1]/",        // IPv4-compatible
            "http://[::8.8.8.8]/",          // IPv4-compatible, deprecated
            "http://[::ffff:0:7f00:1]/",    // IPv4-translated
            "http://[64:ff9b::7f00:1]/",    // NAT64 to 127.0.0.1
            "http://[64:ff9b::a9fe:a9fe]/", // NAT64 to 169.254.169.254
            "http://[64:ff9b:1::1]/",       // NAT64 local-use prefix
            "http://[2002:7f00:1::]/",      // 6to4 for 127.0.0.1
            "http://[2002:a00:1::1]/",      // 6to4 for 10.0.0.1
            "http://[fec0::1]/",            // site-local
            "http://[100::1]/",             // discard-only
        };

        THEN("the parser rejects each one")
        {
            for (const auto& url : rejected)
            {
                INFO(url);
                REQUIRE_FALSE(parse_webhook_url(url).has_value());
            }
        }
    }

    GIVEN("public targets that only look similar")
    {
        const std::vector<std::string> accepted = {
            "http://8.8.8.8/",
            "http://1.0.0.1/",
            "http://100.63.0.1/",
            "http://123.example.com/",
            "http://0x.example.com/",
            "http://hooks.example.com./",
            "http://[2001:4860:4860::8888]/",
            "http://[64:ff9b::808:808]/", // NAT64 to 8.8.8.8
            "http://[2002:808:808::1]/",  // 6to4 for 8.8.8.8
        };

        THEN("the parser accepts each one")
        {
            for (const auto& url : accepted)
            {
                INFO(url);
                REQUIRE(parse_webhook_url(url).has_value());
            }
        }
    }
}
