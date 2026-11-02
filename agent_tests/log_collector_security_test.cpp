// Log monitor security tests (SEC-10).
//
// Dashboard operators choose which files the agent reads and which lines
// surface on the dashboard. These tests pin the agent-side defence: an
// allowlist the server cannot change, symlink resolution, regular-files-
// only, and hard caps on reads and matches per tick.
#include "../agent/collectors/log_collector.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace thewatcher;
using namespace thewatcher::agent;

namespace
{

// Per-process scratch directory: unique random name (mkdtemp on POSIX) and
// owner-only permissions, so the tests never trust a fixed, guessable path in
// the publicly writable system temp directory. Removed at process exit.
const std::filesystem::path& scratch_root()
{
    struct Scratch
    {
        std::filesystem::path dir;

        Scratch()
        {
#ifdef _WIN32
            std::random_device rd;
            std::ostringstream name;
            name << "thewatcher-log-sec-tests-" << std::hex << rd() << rd();
            dir = std::filesystem::temp_directory_path() / name.str();
            std::filesystem::create_directories(dir);
#else
            auto tmpl = (std::filesystem::temp_directory_path() / "thewatcher-log-sec-tests-XXXXXX").string();
            if (::mkdtemp(tmpl.data()) == nullptr)
                throw std::runtime_error("mkdtemp failed");
            dir = tmpl;
#endif
            std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
            // Canonical so expectations hold where the temp dir is itself a symlink (macOS /var).
            dir = std::filesystem::canonical(dir);
        }

        ~Scratch()
        {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        }
    };
    static const Scratch scratch;
    return scratch.dir;
}

std::filesystem::path make_dir(const char* name)
{
    auto dir = scratch_root() / name;
    std::filesystem::create_directories(dir);
    return dir;
}

LogMonitorConfig make_config(const std::filesystem::path& path, const std::string& pattern = "ERROR",
                             const std::string& indicator = "log:sec")
{
    LogMonitorConfig cfg;
    cfg.path = path.string();
    cfg.pattern = pattern;
    cfg.indicator_name = indicator;
    cfg.severity = "red";
    cfg.enabled = true;
    return cfg;
}

void write_file(const std::filesystem::path& p, const std::string& contents)
{
    std::ofstream f(p, std::ios::trunc | std::ios::binary);
    f << contents;
}

} // namespace

SCENARIO("LogCollector is disabled without an allowlist")
{
    GIVEN("a collector with no allowlist configured and a matching log file")
    {
        LogCollector collector;
        auto dir = make_dir("fail-closed");
        auto path = dir / "app.log";
        write_file(path, "ERROR leaked\n");
        collector.set_configs({make_config(path)});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("nothing is read — the default is fail closed")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

SCENARIO("LogCollector refuses paths outside the allowlist")
{
    GIVEN("a collector allowed to read dir A, with files in A and dir B")
    {
        LogCollector collector;
        auto dir_a = make_dir("inside");
        auto dir_b = make_dir("outside");
        auto path_a = dir_a / "inside.log";
        auto path_b = dir_b / "outside.log";
        write_file(path_a, "ERROR inside\n");
        write_file(path_b, "ERROR outside\n");
        collector.set_allowed_dirs({dir_a});
        collector.set_configs({make_config(path_a), make_config(path_b)});

        WHEN("tick() is called")
        {
            collector.tick();
            auto matches = collector.take_matches();

            THEN("only the allowed file is read")
            {
                REQUIRE(matches.size() == 1);
                REQUIRE(matches[0].path == path_a.string());
            }
        }
    }
}

SCENARIO("LogCollector refuses symlinks pointing outside the allowlist")
{
    GIVEN("a collector allowed to read dir A and a symlink in A pointing at a file in dir B")
    {
        LogCollector collector;
        auto dir_a = make_dir("link-src");
        auto dir_b = make_dir("link-dst");
        auto real_file = dir_b / "secret.log";
        write_file(real_file, "ERROR secret\n");
        auto link = dir_a / "link.log";
        std::filesystem::remove(link);
        std::error_code ec;
        std::filesystem::create_symlink(real_file, link, ec);

        if (ec)
        {
            // Symlink creation can be unavailable (e.g. Windows without
            // developer mode); this scenario cannot run.
            SUCCEED("symlinks not available on this host");
            return;
        }

        collector.set_allowed_dirs({dir_a});
        collector.set_configs({make_config(link)});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("the symlink is not followed")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

SCENARIO("set_allowed_dirs canonicalizes its entries")
{
    GIVEN("a log file and an allowlist entry spelled with '..'")
    {
        LogCollector collector;
        auto dir = make_dir("canon-dotdot");
        auto path = dir / "app.log";
        write_file(path, "ERROR reachable\n");
        std::filesystem::create_directories(dir / "child");
        collector.set_allowed_dirs({dir / "child" / ".."});
        collector.set_configs({make_config(path)});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("the file is still read")
            {
                REQUIRE(collector.take_matches().size() == 1);
            }
        }
    }

    GIVEN("an allowlist entry that is a symlink to the real log directory")
    {
        LogCollector collector;
        auto real_dir = make_dir("canon-real");
        auto alias = real_dir.parent_path() / "canon-alias";
        std::filesystem::remove(alias);
        std::error_code ec;
        std::filesystem::create_directory_symlink(real_dir, alias, ec);

        if (ec)
        {
            SUCCEED("symlinks not available on this host");
            return;
        }

        auto path = real_dir / "app.log";
        write_file(path, "ERROR via alias\n");
        collector.set_allowed_dirs({alias});
        collector.set_configs({make_config(path)});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("the file under the resolved directory is read")
            {
                REQUIRE(collector.take_matches().size() == 1);
            }
        }
    }
}

SCENARIO("LogCollector refuses a log file replaced by a symlink to a file outside the allowlist")
{
    GIVEN("a log file in the allowlist that has already been tailed once")
    {
        LogCollector collector;
        auto dir_a = make_dir("swap-inside");
        auto dir_b = make_dir("swap-outside");
        auto path = dir_a / "app.log";
        auto secret = dir_b / "secret.log";
        write_file(path, "ERROR before swap\n");
        write_file(secret, "ERROR secret\n");
        collector.set_allowed_dirs({dir_a});
        collector.set_configs({make_config(path)});
        collector.tick();
        REQUIRE(collector.take_matches().size() == 1);

        WHEN("the file is replaced by a symlink to the outside file")
        {
            std::filesystem::remove(path);
            std::error_code ec;
            std::filesystem::create_symlink(secret, path, ec);

            if (ec)
            {
                SUCCEED("symlinks not available on this host");
                return;
            }
            collector.tick();

            THEN("the outside file is not read")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

#ifndef _WIN32
SCENARIO("LogCollector does not block on or read a FIFO in the allowlist")
{
    GIVEN("a FIFO named like a log file inside the allowed directory")
    {
        LogCollector collector;
        auto dir = make_dir("fifo");
        auto fifo = dir / "pipe.log";
        std::filesystem::remove(fifo);
        REQUIRE(::mkfifo(fifo.c_str(), 0600) == 0);
        collector.set_allowed_dirs({dir});
        collector.set_configs({make_config(fifo)});

        WHEN("tick() is called with no writer on the FIFO")
        {
            // A blocking open() would hang here forever.
            collector.tick();

            THEN("the FIFO is rejected and nothing is produced")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}
#endif

SCENARIO("LogCollector refuses non-regular files")
{
    GIVEN("a collector configured with a directory path")
    {
        LogCollector collector;
        auto dir = make_dir("not-a-file");
        collector.set_allowed_dirs({dir});
        collector.set_configs({make_config(dir)});

        WHEN("tick() is called")
        {
            collector.tick();

            THEN("the directory is not read and nothing is produced")
            {
                REQUIRE(collector.take_matches().empty());
            }
        }
    }
}

SCENARIO("LogCollector truncates over-long lines")
{
    GIVEN("a collector and a log file containing one line far beyond the line cap")
    {
        LogCollector collector;
        auto dir = make_dir("long-line");
        auto path = dir / "huge.log";
        // The marker sits at the start so it survives truncation; the tail
        // of the monster line is what gets dropped.
        write_file(path, "ERROR " + std::string(300 * 1024, 'x') + "\n");

        collector.set_allowed_dirs({dir});
        collector.set_configs({make_config(path)});

        WHEN("tick() is called")
        {
            collector.tick();
            auto matches = collector.take_matches();

            THEN("the matched line is capped, not the whole monster")
            {
                REQUIRE(matches.size() == 1);
                REQUIRE(matches[0].matched_line.size() <= 64 * 1024);
            }
        }
    }
}

SCENARIO("LogCollector caps matches per tick and continues next tick")
{
    GIVEN("a collector and a log file with more matching lines than one tick allows")
    {
        LogCollector collector;
        auto dir = make_dir("match-cap");
        auto path = dir / "many.log";

        std::string contents;
        for (int i = 0; i < 150; ++i)
            contents += "ERROR line " + std::to_string(i) + "\n";
        write_file(path, contents);

        collector.set_allowed_dirs({dir});
        collector.set_configs({make_config(path)});

        WHEN("tick() is called twice")
        {
            collector.tick();
            auto first = collector.take_matches();
            collector.tick();
            auto second = collector.take_matches();

            THEN("the first tick caps at 100 matches and the rest follow next tick")
            {
                REQUIRE(first.size() == 100);
                REQUIRE(second.size() == 50);
            }
        }
    }
}

SCENARIO("LogCollector drops the remainder of an over-long line across ticks without re-matching it")
{
    GIVEN("a 2 MiB line (beyond the per-tick byte cap) followed by a short matching line")
    {
        LogCollector collector;
        auto dir = make_dir("monster-line");
        auto path = dir / "monster.log";
        write_file(path, "ERROR " + std::string(2 * 1024 * 1024, 'x') + "\nERROR next\n");
        collector.set_allowed_dirs({dir});
        collector.set_configs({make_config(path)});

        WHEN("tick() is called until the file is consumed")
        {
            std::vector<proto::LogMatch> all;
            for (int i = 0; i < 4; ++i)
            {
                collector.tick();
                for (auto& m : collector.take_matches())
                    all.push_back(std::move(m));
            }

            THEN("the truncated line matches once and the following line matches once")
            {
                REQUIRE(all.size() == 2);
                REQUIRE(all[0].matched_line.size() <= 64 * 1024);
                REQUIRE(all[1].matched_line == "ERROR next");
            }
        }
    }
}

SCENARIO("parse_log_allowed_dirs_from_env parses the platform path list")
{
    GIVEN("the LOG_ALLOWED_DIRS environment variable set to two directories")
    {
        const auto temp = scratch_root();
#ifdef _WIN32
        constexpr char sep = ';';
        const auto value = (temp / "logs-a").string() + sep + (temp / "logs-b").string();
        REQUIRE(_putenv_s("LOG_ALLOWED_DIRS", value.c_str()) == 0);
#else
        constexpr char sep = ':';
        const auto value = (temp / "logs-a").string() + sep + (temp / "logs-b").string();
        REQUIRE(setenv("LOG_ALLOWED_DIRS", value.c_str(), 1) == 0);
#endif

        WHEN("the allowlist is parsed")
        {
            auto dirs = parse_log_allowed_dirs_from_env();

            THEN("both directories are present")
            {
                REQUIRE(dirs.size() == 2);
            }
        }

        // Leave the environment clean for any test that relies on the
        // allowlist being unset (an empty value counts as unset).
#ifdef _WIN32
        REQUIRE(_putenv_s("LOG_ALLOWED_DIRS", "") == 0);
#else
        REQUIRE(unsetenv("LOG_ALLOWED_DIRS") == 0);
#endif
    }
}