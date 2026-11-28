#include "common/version.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <string>

namespace
{
// True when `text` is exactly MAJOR.MINOR.PATCH with numeric components.
bool is_semantic_version(const std::string& text)
{
    int dots = 0;
    bool digit_seen = false;
    for (char c : text)
    {
        if (c == '.')
        {
            if (!digit_seen)
                return false;
            ++dots;
            digit_seen = false;
        }
        else if (std::isdigit(static_cast<unsigned char>(c)) != 0)
        {
            digit_seen = true;
        }
        else
        {
            return false;
        }
    }
    return dots == 2 && digit_seen;
}
} // namespace

SCENARIO("The build embeds the project version", "[version]")
{
    GIVEN("a binary built from meson.build")
    {
        WHEN("the embedded version is read")
        {
            const std::string version{thewatcher::kVersion};

            THEN("it is a non-empty MAJOR.MINOR.PATCH string")
            {
                REQUIRE_FALSE(version.empty());
                REQUIRE(is_semantic_version(version));
            }
        }
    }
}
