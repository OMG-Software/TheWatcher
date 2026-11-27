// Behavioural tests for the windows_service helpers: command line quoting
// (CommandLineToArgvW rules, compiled on every platform) and the non-Windows
// stubs, which must refuse service operations with a non-zero status.
#include "common/windows_service.hpp"

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <utility>
#include <vector>

using thewatcher::service::build_service_command_line;
using thewatcher::service::quote_windows_argument;

namespace
{

// Reference parser for the CommandLineToArgvW / MSVC CRT rules (2008+), used
// to prove that a quoted command line splits back into the original values.
std::vector<std::string> split_windows_command_line(const std::string& line)
{
    std::vector<std::string> args;
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
            ++i;
        if (i >= line.size())
            break;

        std::string arg;
        bool in_quotes = false;
        while (i < line.size() && (in_quotes || (line[i] != ' ' && line[i] != '\t')))
        {
            std::size_t backslashes = 0;
            while (i < line.size() && line[i] == '\\')
            {
                ++backslashes;
                ++i;
            }
            if (i < line.size() && line[i] == '"')
            {
                arg.append(backslashes / 2, '\\');
                if (backslashes % 2 == 1)
                    arg += '"';
                else
                    in_quotes = !in_quotes;
                ++i;
            }
            else
            {
                arg.append(backslashes, '\\');
                if (i < line.size())
                    arg += line[i++];
            }
        }
        args.push_back(arg);
    }
    return args;
}

} // namespace

SCENARIO("quote_windows_argument follows the CommandLineToArgvW backslash rules (issue #161)")
{
    GIVEN("argument values and the quoted form Windows expects")
    {
        const std::vector<std::pair<std::string, std::string>> table{
            {"",                       R"("")"                    },
            {"plain",                  R"("plain")"               },
            {"two words",              R"("two words")"           },
            {R"(C:\Data\TheWatcher)",  R"("C:\Data\TheWatcher")"  },
            {R"(C:\Data\TheWatcher\)", R"("C:\Data\TheWatcher\\")"},
            {R"(C:\Data\\)",           R"("C:\Data\\\\")"         },
            {R"(say "hi")",            R"("say \"hi\"")"          },
            {R"(a\"b)",                R"("a\\\"b")"              },
            {R"(a\\"b)",               R"("a\\\\\"b")"            },
            {R"(")",                   R"("\"")"                  },
            {R"(\)",                   R"("\\")"                  },
            {R"(\\server\share dir\)", R"("\\server\share dir\\")"},
        };

        for (const auto& [value, expected] : table)
        {
            WHEN("the value [" + value + "] is quoted")
            {
                const auto quoted = quote_windows_argument(value);

                THEN("it matches the documented quoting")
                {
                    REQUIRE(quoted == expected);
                }

                AND_THEN("parsing the quoted text returns the original value")
                {
                    const auto parsed = split_windows_command_line(quoted);
                    REQUIRE(parsed.size() == 1);
                    REQUIRE(parsed.front() == value);
                }
            }
        }
    }
}

SCENARIO("build_service_command_line keeps every argument separate (issue #161)")
{
    GIVEN("arguments including a config path with a trailing backslash")
    {
        const std::vector<std::string> args{R"(C:\Program Files\TheWatcher\agent.exe)", "--service", "--config",
                                            R"(C:\Data\TheWatcher\)", "--insecure"};

        WHEN("the service command line is built")
        {
            const auto line = build_service_command_line(args);

            THEN("splitting it returns the same arguments, so --insecure is not swallowed by the path")
            {
                REQUIRE(split_windows_command_line(line) == args);
            }
        }
    }

    GIVEN("no arguments")
    {
        THEN("the command line is empty")
        {
            REQUIRE(build_service_command_line({}).empty());
        }
    }
}

#ifndef _WIN32
SCENARIO("The windows_service stubs refuse to act on non-Windows platforms")
{
    GIVEN("a platform without a Service Control Manager")
    {
        WHEN("a service is installed")
        {
            const auto status = thewatcher::service::install_windows_service({"svc", "Svc", "desc", "cmd"});

            THEN("a non-zero status is returned")
            {
                REQUIRE(status != 0);
            }
        }

        WHEN("a service is uninstalled")
        {
            THEN("a non-zero status is returned")
            {
                REQUIRE(thewatcher::service::uninstall_windows_service("svc") != 0);
            }
        }

        WHEN("service mode is requested")
        {
            bool ran = false;
            bool stopped = false;
            const auto status = thewatcher::service::run_windows_service(
                "svc",
                [&] {
                    ran = true;
                },
                [&] {
                    stopped = true;
                });

            THEN("a non-zero status is returned and neither callback runs")
            {
                REQUIRE(status != 0);
                REQUIRE_FALSE(ran);
                REQUIRE_FALSE(stopped);
            }
        }

        WHEN("the executable path is queried")
        {
            THEN("it is empty")
            {
                REQUIRE(thewatcher::service::current_executable_path().empty());
            }
        }
    }
}
#endif
