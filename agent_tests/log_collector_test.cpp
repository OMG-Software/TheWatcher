#include "../agent/collectors/log_collector.hpp"
#include "test_support/temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace thewatcher;
using namespace thewatcher::agent;

namespace
{

// Per-process scratch directory (unique, owner-only, removed at process exit).
const std::filesystem::path& scratch_root()
{
    return thewatcher::test::process_temp_dir().path();
}

std::filesystem::path temp_log_path(const char* name)
{
    auto dir = scratch_root() / "logs";
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

// Appends bytes exactly as given (no trailing newline added).
void append_raw(const std::filesystem::path& p, const std::string& text)
{
    std::ofstream f(p, std::ios::app | std::ios::binary);
    f << text;
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
    collector.set_allowed_dirs({scratch_root() / "logs"});
}

// Creates the (empty) file, configures the collector and polls once. A watch
// starts at the end of an existing file (#90), so content written after this
// call is what the tests expect to see.
void start_tailing(LogCollector& collector, const std::filesystem::path& path, std::vector<LogMonitorConfig> configs)
{
    append_raw(path, "");
    collector.set_configs(std::move(configs));
    collector.tick();
    collector.take_matches();
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
        start_tailing(collector, path, {make_config(path.string(), "ERROR", "log:disk", "amber")});
        write_lines(path, {"INFO  application started", "ERROR disk full", "ERROR network timeout"});

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
        start_tailing(collector, path, {make_config(path.string(), "CRITICAL")});
        write_lines(path, {"INFO  all good", "DEBUG verbose line", "TRACE startup"});

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
        start_tailing(collector, path, {make_config(path.string(), "ERROR")});
        write_lines(path, {"ERROR first", "ERROR second"});
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
        start_tailing(collector, path, {make_config(path.string(), "ERROR")});
        write_lines(path, {"ERROR first line"});

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
        start_tailing(collector, path, {make_config(path.string(), "ERROR")});
        write_lines(path, {"ERROR stale line 1", "ERROR stale line 2"});

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
        start_tailing(collector, path, {make_config(path.string(), "ERROR", "log:disabled", "red", false)});
        write_lines(path, {"ERROR this should be ignored"});

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

        append_raw(path_b, "");
        start_tailing(collector, path_a,
                      {make_config(path_a.string(), "ERROR", "log:a"), make_config(path_b.string(), "ERROR", "log:b")});
        write_lines(path_a, {"ERROR from a"});
        write_lines(path_b, {"ERROR from b"});
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

// ── Partial lines ─────────────────────────────────────────────────────────────
//
// Only '\n'-terminated lines are matched. An unterminated trailing line is
// held back and read again, whole, once the writer finishes it.

SCENARIO("LogCollector does not lose a line that was still being written when it was polled")
{
    GIVEN("a log file ending in an unterminated line")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("partial.log");
        write_lines(path, {"INFO started"});
        append_raw(path, "ERROR partial");
        collector.set_configs({make_config(path.string(), "ERROR")});

        WHEN("tick() is called before the line is completed")
        {
            collector.tick();

            THEN("the incomplete line is not matched yet")
            {
                REQUIRE(collector.take_matches().empty());
            }

            AND_WHEN("the writer finishes the line and tick() is called again")
            {
                append_raw(path, " match\n");
                collector.tick();
                auto matches = collector.take_matches();

                THEN("the full line is matched exactly once")
                {
                    REQUIRE(matches.size() == 1);
                    REQUIRE(matches[0].matched_line == "ERROR partial match");
                }

                AND_THEN("a further tick does not match it again")
                {
                    collector.tick();
                    REQUIRE(collector.take_matches().empty());
                }
            }
        }

        WHEN("tick() is called repeatedly while the line stays unterminated")
        {
            collector.tick();
            collector.tick();
            collector.tick();

            THEN("nothing is matched and nothing breaks")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

SCENARIO("LogCollector matches a line that straddles the per-tick byte cap exactly once")
{
    GIVEN("1023 filler lines of 1 KiB followed by a 2000-byte matching line and a short matching line")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("straddle.log");

        // 1023 * 1024 bytes = 1 MiB - 1 KiB, so the 1 MiB cap lands inside the long line.
        std::string contents;
        for (int i = 0; i < 1023; ++i)
            contents += std::string(1023, 'x') + "\n";
        const std::string straddling = "ERROR straddle" + std::string(1986, 'y');
        contents += straddling + "\nERROR after\n";
        start_tailing(collector, path, {make_config(path.string(), "ERROR")});
        append_raw(path, contents);

        WHEN("tick() is called twice")
        {
            collector.tick();
            auto first = collector.take_matches();
            collector.tick();
            auto second = collector.take_matches();

            THEN("the first tick stops before the straddling line")
            {
                REQUIRE(first.empty());
            }

            AND_THEN("the second tick matches the whole straddling line and the next line")
            {
                REQUIRE(second.size() == 2);
                REQUIRE(second[0].matched_line == straddling);
                REQUIRE(second[1].matched_line == "ERROR after");
            }

            AND_THEN("a third tick produces nothing further")
            {
                collector.tick();
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

// ── Per-watch offsets, first sight, CRLF, rotation (issue #90) ────────────────

SCENARIO("Two indicators on the same file each see every matching line")
{
    GIVEN("two watches with different patterns on one log file")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("two_watches.log");
        start_tailing(collector, path,
                      {make_config(path.string(), "ERROR", "log:errors"),
                       make_config(path.string(), "disk", "log:disk", "amber")});

        WHEN("a line matching both patterns and a line matching one are appended")
        {
            write_lines(path, {"ERROR disk full", "ERROR network down"});
            collector.tick();
            auto matches = collector.take_matches();

            THEN("the first watch reports both lines and the second reports only the disk line")
            {
                std::size_t errors = 0;
                std::size_t disk = 0;
                for (const auto& m : matches)
                {
                    if (m.indicator_name == "log:errors")
                        ++errors;
                    if (m.indicator_name == "log:disk")
                        ++disk;
                }
                REQUIRE(errors == 2);
                REQUIRE(disk == 1);
            }
        }
    }
}

SCENARIO("A watch starts at the end of an existing file and reports only new lines")
{
    GIVEN("a log file that already holds matching history")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("history.log");
        write_lines(path, {"ERROR old 1", "ERROR old 2"});

        WHEN("monitoring is enabled and polled")
        {
            collector.set_configs({make_config(path.string(), "ERROR")});
            collector.tick();

            THEN("no historical line is reported")
            {
                REQUIRE(collector.take_matches().empty());
            }

            AND_WHEN("a new line is appended")
            {
                write_lines(path, {"ERROR fresh"});
                collector.tick();
                auto matches = collector.take_matches();

                THEN("only the new line is reported")
                {
                    REQUIRE(matches.size() == 1);
                    REQUIRE(matches[0].matched_line == "ERROR fresh");
                }
            }
        }
    }

    GIVEN("a file that ends in a line the writer has not finished")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("history_partial.log");
        write_lines(path, {"ERROR old"});
        append_raw(path, "ERROR half");
        collector.set_configs({make_config(path.string(), "ERROR")});
        collector.tick();
        REQUIRE(collector.take_matches().empty());

        WHEN("the writer finishes that line")
        {
            append_raw(path, " done\n");
            collector.tick();
            auto matches = collector.take_matches();

            THEN("the line is evaluated whole, not as a fragment")
            {
                REQUIRE(matches.size() == 1);
                REQUIRE(matches[0].matched_line == "ERROR half done");
            }
        }
    }

    GIVEN("a watch added on a file that another watch already tails")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("added_watch.log");
        start_tailing(collector, path, {make_config(path.string(), "ERROR", "log:first")});
        write_lines(path, {"ERROR before second watch"});
        collector.tick();
        REQUIRE(collector.take_matches().size() == 1);

        WHEN("a second watch is configured")
        {
            collector.set_configs(
                {make_config(path.string(), "ERROR", "log:first"), make_config(path.string(), "ERROR", "log:second")});
            collector.tick();

            THEN("the second watch does not replay the existing lines and the first is not disturbed")
            {
                REQUIRE(collector.take_matches().empty());
            }

            AND_WHEN("another line is appended")
            {
                write_lines(path, {"ERROR after second watch"});
                collector.tick();

                THEN("both watches report it once")
                {
                    REQUIRE(collector.take_matches().size() == 2);
                }
            }
        }
    }
}

SCENARIO("A file created after monitoring began is read from its first byte")
{
    GIVEN("a watch on a path that does not exist yet")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("created_later.log");
        collector.set_configs({make_config(path.string(), "ERROR")});
        collector.tick();
        REQUIRE(collector.take_matches().empty());

        WHEN("the file appears with content and is polled")
        {
            write_lines(path, {"ERROR first", "ERROR second"});
            collector.tick();

            THEN("every line in it is new and is reported")
            {
                REQUIRE(collector.take_matches().size() == 2);
            }
        }
    }
}

SCENARIO("CRLF line endings do not defeat end-anchored patterns")
{
    GIVEN("a Windows-style log and a pattern anchored at the end of the line")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("crlf.log");
        start_tailing(collector, path, {make_config(path.string(), "ERROR$")});

        WHEN("lines ending in CRLF are appended")
        {
            append_raw(path, "disk ERROR\r\nall fine\r\nnet ERROR\r\n");
            collector.tick();
            auto matches = collector.take_matches();

            THEN("both ERROR lines match and the reported text has no carriage return")
            {
                REQUIRE(matches.size() == 2);
                REQUIRE(matches[0].matched_line == "disk ERROR");
                REQUIRE(matches[1].matched_line == "net ERROR");
            }
        }
    }
}

SCENARIO("A replaced log file is read from its start even if the new file is larger")
{
    GIVEN("a tailed file that is rotated by renaming it and creating a new file")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("replaced.log");
        auto rotated = path;
        rotated += ".1";
        std::filesystem::remove(rotated);
        start_tailing(collector, path, {make_config(path.string(), "ERROR")});
        write_lines(path, {"ERROR a"});
        collector.tick();
        REQUIRE(collector.take_matches().size() == 1);

        WHEN("the new file is already longer than the old read offset when polled")
        {
            std::filesystem::rename(path, rotated);
            write_lines(path, {"ERROR new 1", "ERROR new 2", "ERROR new 3"});
            collector.tick();

            THEN("all its lines are reported, from byte 0")
            {
                REQUIRE(collector.take_matches().size() == 3);
            }
        }
    }
}

SCENARIO("A file that is deleted and recreated is read from its start")
{
    GIVEN("a tailed file that disappears between polls")
    {
        LogCollector collector;
        allow_test_logs_dir(collector);
        auto path = temp_log_path("recreated.log");
        start_tailing(collector, path, {make_config(path.string(), "ERROR")});
        write_lines(path, {"ERROR before delete"});
        collector.tick();
        REQUIRE(collector.take_matches().size() == 1);
        std::filesystem::remove(path);
        collector.tick();

        WHEN("it is recreated with new content")
        {
            write_lines(path, {"ERROR recreated"});
            collector.tick();
            auto matches = collector.take_matches();

            THEN("the new content is reported")
            {
                REQUIRE(matches.size() == 1);
                REQUIRE(matches[0].matched_line == "ERROR recreated");
            }
        }
    }
}
