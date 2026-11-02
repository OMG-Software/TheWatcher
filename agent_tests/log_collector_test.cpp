#include "../agent/collectors/log_collector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace thewatcher;
using namespace thewatcher::agent;

namespace
{

std::filesystem::path temp_log_path(const char* name)
{
    auto dir = std::filesystem::temp_directory_path() / "thewatcher-log-tests";
    std::filesystem::create_directories(dir);
    auto path = dir / name;
    std::filesystem::remove(path);
    return path;
}

LogMonitorConfig make_config(const std::string& path, const std::string& pattern,
                             const std::string& indicator = "log:test", const std::string& severity = "red",
                             bool enabled = true)
{
    LogMonitorConfig cfg;
    cfg.path = path;
    cfg.pattern = pattern;
    cfg.indicator_name = indicator;
    cfg.severity = severity;
    cfg.enabled = enabled;
    return cfg;
}

void write_lines(const std::filesystem::path& p, const std::vector<std::string>& lines)
{
    std::ofstream f(p, std::ios::app);
    for (const auto& line : lines)
        f << line << '\n';
}

void overwrite_lines(const std::filesystem::path& p, const std::vector<std::string>& lines)
{
    std::ofstream f(p, std::ios::trunc);
    for (const auto& line : lines)
        f << line << '\n';
}

// The log monitor only reads files inside its allowlist (SEC-10); grant the
// shared test directory for these behavioural tests.
void allow_test_logs_dir(LogCollector& collector)
{
    collector.set_allowed_dirs({std::filesystem::temp_directory_path() / "thewatcher-log-tests"});
}

} // namespace

// ── Empty configuration ───────────────────────────────────────────────────────

SCENARIO("LogCollector with no configs produces no matches")
{
    GIVEN("a freshly constructed LogCollector with no configs set")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);

        WHEN("tick() and take_matches() are called")
        {
            collector.tick();
            auto matches = collector.take_matches();

            THEN("take_matches returns an empty vector")
            {
                REQUIRE(matches.empty());
            }
        }

        WHEN("set_configs is called with an empty list")
        {
            collector.set_configs({});
            collector.tick();

            THEN("take_matches returns an empty vector")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── Non-existent file ─────────────────────────────────────────────────────────

SCENARIO("LogCollector handles a configured file that does not exist without crashing")
{
    GIVEN("a config pointing to a path that has never been created")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("nonexistent_file_that_should_not_exist.log");
        collector.set_configs({make_config(path.string(), "ERROR")});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("no matches are produced and the collector does not throw")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── Empty file ────────────────────────────────────────────────────────────────

SCENARIO("LogCollector produces no matches for an empty log file")
{
    GIVEN("a config pointing to an empty file")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("empty.log");
        {
            std::ofstream f(path);
        } // create empty
        collector.set_configs({make_config(path.string(), "ERROR")});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("take_matches returns an empty vector")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── Pattern matching ──────────────────────────────────────────────────────────

SCENARIO("LogCollector returns a match for every line that matches the regex pattern")
{
    GIVEN("a log file with three lines, two of which contain 'ERROR'")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("matching.log");
        write_lines(path, {"INFO  application started", "ERROR disk full", "ERROR network timeout"});
        collector.set_configs({make_config(path.string(), "ERROR", "log:disk", "amber")});

        WHEN("tick() is called")
        {
            collector.tick();
            auto matches = collector.take_matches();

            THEN("exactly two matches are returned")
            {
                REQUIRE(matches.size() == 2);
            }

            AND_THEN("each match carries the correct indicator, path, and severity")
            {
                for (const auto& m : matches)
                {
                    REQUIRE(m.indicator_name == "log:disk");
                    REQUIRE(m.path == path.string());
                    REQUIRE(m.severity == "amber");
                }
            }

            AND_THEN("the matched lines contain the pattern text")
            {
                for (const auto& m : matches)
                    REQUIRE(m.matched_line.find("ERROR") != std::string::npos);
            }
        }
    }
}

SCENARIO("LogCollector produces no matches when the pattern does not appear in the file")
{
    GIVEN("a log file whose lines do not match the configured pattern")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("no_match.log");
        write_lines(path, {"INFO  all good", "DEBUG verbose line", "TRACE startup"});
        collector.set_configs({make_config(path.string(), "CRITICAL")});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("take_matches returns an empty vector")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── take_matches drains the queue ─────────────────────────────────────────────

SCENARIO("take_matches drains all pending matches and leaves the queue empty")
{
    GIVEN("a log file that produces two matches")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("drain.log");
        write_lines(path, {"ERROR first", "ERROR second"});
        collector.set_configs({make_config(path.string(), "ERROR")});
        collector.tick();

        WHEN("take_matches is called once")
        {
            auto first = collector.take_matches();

            THEN("both matches are returned")
            {
                REQUIRE(first.size() == 2);
            }

            AND_WHEN("take_matches is called a second time without another tick")
            {
                auto second = collector.take_matches();

                THEN("the queue is empty")
                {
                    REQUIRE(second.empty());
                }
            }
        }
    }
}

// ── Incremental tailing ───────────────────────────────────────────────────────

SCENARIO("LogCollector only returns new lines on subsequent ticks — it does not re-read already-processed content")
{
    GIVEN("a log file with one initial line")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("incremental.log");
        write_lines(path, {"ERROR first line"});
        collector.set_configs({make_config(path.string(), "ERROR")});

        collector.tick();
        auto first_batch = collector.take_matches();
        REQUIRE(first_batch.size() == 1);

        WHEN("a second ERROR line is appended and tick is called again")
        {
            write_lines(path, {"ERROR second line"});
            collector.tick();
            auto second_batch = collector.take_matches();

            THEN("only the new line is returned")
            {
                REQUIRE(second_batch.size() == 1);
                REQUIRE(second_batch[0].matched_line.find("second") != std::string::npos);
            }
        }

        WHEN("no new content is added and tick is called again")
        {
            collector.tick();

            THEN("take_matches returns empty")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── File rotation (truncation) ────────────────────────────────────────────────

SCENARIO("LogCollector detects file rotation via truncation and re-reads from the beginning")
{
    GIVEN("a log file whose first batch has been fully processed")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("rotation.log");
        write_lines(path, {"ERROR stale line 1", "ERROR stale line 2"});
        collector.set_configs({make_config(path.string(), "ERROR")});

        collector.tick();
        auto before_rotation = collector.take_matches();
        REQUIRE(before_rotation.size() == 2);

        WHEN("the file is truncated (simulating log rotation) and new content is written")
        {
            overwrite_lines(path, {"ERROR new line after rotation"});
            collector.tick();
            auto after_rotation = collector.take_matches();

            THEN("the single new line is returned (file was re-read from offset 0)")
            {
                REQUIRE(after_rotation.size() == 1);
                REQUIRE(after_rotation[0].matched_line.find("new line after rotation") != std::string::npos);
            }
        }
    }
}

// ── Disabled config entry ─────────────────────────────────────────────────────

SCENARIO("LogCollector skips a config entry whose enabled flag is false")
{
    GIVEN("a log file and a config with enabled=false")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("disabled.log");
        write_lines(path, {"ERROR this should be ignored"});
        collector.set_configs({make_config(path.string(), "ERROR", "log:disabled", "red", false)});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("no matches are produced")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── Invalid regex ─────────────────────────────────────────────────────────────

SCENARIO("LogCollector silently skips a config entry with an invalid regex pattern")
{
    GIVEN("a log file and a config with a syntactically invalid regex")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("invalid_regex.log");
        write_lines(path, {"ERROR some line"});
        // '[unclosed bracket' is an invalid ECMAScript regex
        collector.set_configs({make_config(path.string(), "[unclosed bracket")});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("no matches are produced and the collector does not throw")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── set_configs removes stale file state ──────────────────────────────────────

SCENARIO("set_configs removes file state for paths that are no longer configured")
{
    GIVEN("a collector that has processed a file, so its offset is tracked")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path_a = temp_log_path("path_a.log");
        auto path_b = temp_log_path("path_b.log");

        write_lines(path_a, {"ERROR from a"});
        write_lines(path_b, {"ERROR from b"});

        collector.set_configs(
            {make_config(path_a.string(), "ERROR", "log:a"), make_config(path_b.string(), "ERROR", "log:b")});
        collector.tick();
        auto first = collector.take_matches();
        REQUIRE(first.size() == 2);

        WHEN("set_configs is called with only path_b, removing path_a")
        {
            collector.set_configs({make_config(path_b.string(), "ERROR", "log:b")});

            // Append new content to both files
            write_lines(path_a, {"ERROR new from a"});
            write_lines(path_b, {"ERROR new from b"});
            collector.tick();
            auto second = collector.take_matches();

            THEN("only path_b produces a new match (path_a state was cleared)")
            {
                // path_b contributes one new line
                bool has_b = false;
                for (const auto& m : second)
                    if (m.indicator_name == "log:b")
                        has_b = true;
                REQUIRE(has_b);

                // path_a is not configured any more
                bool has_a = false;
                for (const auto& m : second)
                    if (m.indicator_name == "log:a")
                        has_a = true;
                REQUIRE_FALSE(has_a);
            }
        }
    }
}
