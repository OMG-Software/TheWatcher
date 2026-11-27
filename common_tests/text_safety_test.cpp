// Tests for the helpers that make peer-supplied text safe for logs, JSON and
// databases (issues #157 and #56).
#include "common/text_safety.hpp"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <string>

using namespace thewatcher::text;

SCENARIO("Invalid UTF-8 is detected and replaced")
{
    GIVEN("strings with well-formed and malformed UTF-8")
    {
        const std::string ascii = "/var/log/syslog";
        const std::string accented = "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x98\x80"; // e-acute, euro, emoji
        const std::string latin1 = "/mnt/caf\xE9";                                // lone 0xE9
        const std::string overlong = "a\xC0\xAF"
                                     "b"; // overlong '/'
        const std::string surrogate = "\xED\xA0\x80";
        const std::string truncated = "ok\xE2\x82"; // euro sign missing its last byte
        const std::string too_high = "\xF4\x90\x80\x80";

        WHEN("each is validated")
        {
            THEN("only the well-formed ones are valid")
            {
                REQUIRE(is_valid_utf8(ascii));
                REQUIRE(is_valid_utf8(accented));
                REQUIRE(is_valid_utf8(""));
                REQUIRE_FALSE(is_valid_utf8(latin1));
                REQUIRE_FALSE(is_valid_utf8(overlong));
                REQUIRE_FALSE(is_valid_utf8(surrogate));
                REQUIRE_FALSE(is_valid_utf8(truncated));
                REQUIRE_FALSE(is_valid_utf8(too_high));
            }
        }

        WHEN("invalid bytes are replaced")
        {
            THEN("valid text is unchanged and each bad byte becomes U+FFFD")
            {
                REQUIRE(replace_invalid_utf8(accented) == accented);
                REQUIRE(replace_invalid_utf8(latin1) == "/mnt/caf\xEF\xBF\xBD");
                REQUIRE(replace_invalid_utf8(truncated) == "ok\xEF\xBF\xBD\xEF\xBF\xBD");
            }

            AND_THEN("the result serialises to JSON without throwing")
            {
                for (const auto* bad : {&latin1, &overlong, &surrogate, &truncated, &too_high})
                {
                    REQUIRE_NOTHROW(nlohmann::json{
                        {"v", replace_invalid_utf8(*bad)}
                    }
                                        .dump());
                    REQUIRE_THROWS(nlohmann::json{
                        {"v", *bad}
                    }.dump());
                }
            }
        }

        WHEN("sanitize_utf8_in_place is used")
        {
            std::string value = latin1;
            const bool changed = sanitize_utf8_in_place(value);
            std::string clean = ascii;
            const bool clean_changed = sanitize_utf8_in_place(clean);

            THEN("it reports whether it changed the string")
            {
                REQUIRE(changed);
                REQUIRE(is_valid_utf8(value));
                REQUIRE_FALSE(clean_changed);
                REQUIRE(clean == ascii);
            }
        }
    }
}

SCENARIO("Truncation never splits a UTF-8 character")
{
    GIVEN("a string of three-byte characters")
    {
        const std::string euros = "\xE2\x82\xAC\xE2\x82\xAC\xE2\x82\xAC"; // 9 bytes

        WHEN("it is cut in the middle of a character")
        {
            THEN("the cut backs up to the previous character boundary")
            {
                REQUIRE(truncate_utf8(euros, 9) == euros);
                REQUIRE(truncate_utf8(euros, 8) == "\xE2\x82\xAC\xE2\x82\xAC");
                REQUIRE(truncate_utf8(euros, 4) == "\xE2\x82\xAC");
                REQUIRE(truncate_utf8(euros, 2).empty());
                REQUIRE(is_valid_utf8(truncate_utf8(euros, 5)));
            }
        }
    }
}

SCENARIO("Peer-supplied text is neutralised before it is logged")
{
    GIVEN("a path that tries to forge a log line")
    {
        const std::string forged = "x\n2026-11-04 12:00:00 [CRITICAL] admin login from 1.2.3.4";

        WHEN("it is sanitised for the log")
        {
            const auto safe = sanitize_log_text(forged, 256);

            THEN("no line break survives")
            {
                REQUIRE(safe.find('\n') == std::string::npos);
                REQUIRE(safe.rfind("x?2026-11-04", 0) == 0);
            }
        }
    }

    GIVEN("control characters of every kind")
    {
        WHEN("they are sanitised")
        {
            THEN("each is replaced with a question mark")
            {
                REQUIRE(sanitize_log_text("a\rb\nc\td\x1B[31me\x7F") == "a?b?c?d?[31me?");
                REQUIRE(sanitize_log_text(std::string("nul\0byte", 8)) == "nul?byte");
            }
        }

        AND_WHEN("a string holds Unicode line separators")
        {
            THEN("U+0085, U+2028 and U+2029 are replaced as well")
            {
                REQUIRE(sanitize_log_text("a\xC2\x85"
                                          "b\xE2\x80\xA8"
                                          "c\xE2\x80\xA9"
                                          "d") == "a?b?c?d");
            }
        }
    }

    GIVEN("text with valid non-ASCII characters and invalid bytes")
    {
        WHEN("it is sanitised")
        {
            THEN("valid characters are kept and invalid bytes are replaced")
            {
                REQUIRE(sanitize_log_text("caf\xC3\xA9") == "caf\xC3\xA9");
                REQUIRE(sanitize_log_text("caf\xE9!") == "caf?!");
            }
        }
    }

    GIVEN("a long string")
    {
        WHEN("it is sanitised with the default limit")
        {
            const auto safe = sanitize_log_text(std::string(500, 'a'));

            THEN("it is cut to 64 bytes")
            {
                REQUIRE(safe.size() == 64);
            }
        }

        AND_WHEN("the limit falls inside a multi-byte character")
        {
            const auto safe = sanitize_log_text("aa\xE2\x82\xAC", 4);

            THEN("the whole character is dropped")
            {
                REQUIRE(safe == "aa");
            }
        }
    }
}
