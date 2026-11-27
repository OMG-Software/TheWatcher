#include "common/SingleLog.hpp"
#include "common/log_settings.hpp"
#include "test_support/temp_dir.hpp"
#include "test_support/timing.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;
using thewatcher::test::eventually;

namespace
{

bool file_contains(const std::filesystem::path& path, const std::string& needle)
{
    std::ifstream input(path);
    if (!input)
        return false;
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return content.find(needle) != std::string::npos;
}

// Starts from a clean `path` and routes the logger's file output (all
// levels) there, with console output off.
void use_log_file(const std::filesystem::path& path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
    for (int i = 1; i <= 4; ++i)
        std::filesystem::remove(path.string() + "." + std::to_string(i), ec);

    auto& logger = thewatcher::logging::SingleLog::GetInstance();
    logger.SetConsoleLogLevel(thewatcher::logging::LogLevel::L_OFF);
    logger.SetFileLogLevel(thewatcher::logging::LogLevel::L_TRACE);
    logger.SetLogRotation(0, 0);
    logger.SetLogFilePath(path.string());
}

} // namespace

SCENARIO("SingleLog flushes low severity file output in batches")
{
    GIVEN("a configured log file and one hundred INFO messages")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "thewatcher-singlelog-batch-test.log";
        std::error_code ec;
        use_log_file(path);

        WHEN("the logger writes enough INFO entries to reach the batch threshold")
        {
            for (int i = 0; i < 100; ++i)
            {
                LOG_INFO(std::string("singlelog batched flush probe ") + std::to_string(i));
            }

            THEN("the batch is visible before process shutdown")
            {
                REQUIRE(eventually(
                    [&] {
                        return file_contains(path, "singlelog batched flush probe 99");
                    },
                    2s, 20ms));
            }
        }

        std::filesystem::remove(path, ec);
    }
}

SCENARIO("SingleLog flushes notice and higher severity file output immediately")
{
    GIVEN("a configured log file and a NOTICE message")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "thewatcher-singlelog-notice-test.log";
        std::error_code ec;
        use_log_file(path);

        WHEN("the logger writes one NOTICE entry")
        {
            LOG_NOTICE("singlelog immediate notice flush probe");

            THEN("the entry is visible before process shutdown")
            {
                REQUIRE(eventually(
                    [&] {
                        return file_contains(path, "singlelog immediate notice flush probe");
                    },
                    2s, 20ms));
            }
        }

        std::filesystem::remove(path, ec);
    }
}

SCENARIO("SingleLog appends to an existing log file instead of truncating it")
{
    GIVEN("a log file that already holds an entry from a previous run")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "thewatcher-singlelog-append-test.log";
        std::error_code ec;
        use_log_file(path);
        {
            std::ofstream previous(path);
            previous << "previous run evidence\n";
        }

        WHEN("the logger is pointed at that file and writes a new entry")
        {
            auto& logger = thewatcher::logging::SingleLog::GetInstance();
            logger.SetLogFilePath(path.string());
            LOG_NOTICE("singlelog append probe");

            THEN("both the old and the new entry are in the file")
            {
                REQUIRE(eventually(
                    [&] {
                        return file_contains(path, "singlelog append probe");
                    },
                    2s, 20ms));
                REQUIRE(file_contains(path, "previous run evidence"));
            }
        }

        std::filesystem::remove(path, ec);
    }
}

SCENARIO("SingleLog rotates the log file when it reaches the size limit")
{
    GIVEN("a log file limited to 2000 bytes with two backups")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "thewatcher-singlelog-rotate-test.log";
        std::error_code ec;
        use_log_file(path);
        auto& logger = thewatcher::logging::SingleLog::GetInstance();
        logger.SetLogRotation(2000, 2);

        WHEN("far more than the limit is written")
        {
            const std::string filler(80, 'x');
            for (int i = 0; i < 200; ++i)
                LOG_NOTICE("rotation probe " + std::to_string(i) + " " + filler);

            THEN("the newest entry is in the live file and old files are kept up to the backup count")
            {
                REQUIRE(eventually(
                    [&] {
                        return file_contains(path, "rotation probe 199");
                    },
                    2s, 20ms));
                REQUIRE(std::filesystem::exists(path.string() + ".1"));
                REQUIRE(std::filesystem::exists(path.string() + ".2"));
                REQUIRE_FALSE(std::filesystem::exists(path.string() + ".3"));
                REQUIRE(std::filesystem::file_size(path) <= 2000);
                REQUIRE(std::filesystem::file_size(path.string() + ".1") <= 2000);
                REQUIRE(std::filesystem::file_size(path.string() + ".2") <= 2000);
            }
        }

        logger.SetLogRotation(0, 0);
        std::filesystem::remove(path, ec);
        for (int i = 1; i <= 3; ++i)
            std::filesystem::remove(path.string() + "." + std::to_string(i), ec);
    }
}

SCENARIO("SingleLog without a backup count discards rotated content")
{
    GIVEN("a log file limited to 1000 bytes with no backups")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "thewatcher-singlelog-nobackup-test.log";
        std::error_code ec;
        use_log_file(path);
        auto& logger = thewatcher::logging::SingleLog::GetInstance();
        logger.SetLogRotation(1000, 0);

        WHEN("more than the limit is written")
        {
            const std::string filler(80, 'y');
            for (int i = 0; i < 60; ++i)
                LOG_NOTICE("nobackup probe " + std::to_string(i) + " " + filler);

            THEN("no backup file is created and the live file stays within the limit")
            {
                REQUIRE(eventually(
                    [&] {
                        return file_contains(path, "nobackup probe 59");
                    },
                    2s, 20ms));
                REQUIRE_FALSE(std::filesystem::exists(path.string() + ".1"));
                REQUIRE(std::filesystem::file_size(path) <= 1000);
            }
        }

        logger.SetLogRotation(0, 0);
        std::filesystem::remove(path, ec);
    }
}

SCENARIO("LogSettings parses and validates operator supplied values")
{
    GIVEN("default settings")
    {
        thewatcher::logging::LogSettings settings;

        THEN("they log at INFO, rotate at 10 MiB and keep 5 backups")
        {
            REQUIRE(settings.level == thewatcher::logging::LogLevel::L_INFO);
            REQUIRE(settings.max_size_mib == 10);
            REQUIRE(settings.max_files == 5);
        }

        WHEN("a level name is set in any letter case")
        {
            settings.set_level("DeBuG");

            THEN("the matching level is used")
            {
                REQUIRE(settings.level == thewatcher::logging::LogLevel::L_DEBUG);
            }
        }

        WHEN("an unknown level name is set")
        {
            THEN("it is rejected and the level is unchanged")
            {
                REQUIRE_THROWS_AS(settings.set_level("loud"), std::invalid_argument);
                REQUIRE(settings.level == thewatcher::logging::LogLevel::L_INFO);
            }
        }

        WHEN("sizes and counts are not non-negative whole numbers")
        {
            THEN("they are rejected and the values are unchanged")
            {
                REQUIRE_THROWS_AS(settings.set_max_size_mib("-1"), std::invalid_argument);
                REQUIRE_THROWS_AS(settings.set_max_size_mib("ten"), std::invalid_argument);
                REQUIRE_THROWS_AS(settings.set_max_size_mib("99999999999999999999999"), std::invalid_argument);
                REQUIRE_THROWS_AS(settings.set_max_files("2x"), std::invalid_argument);
                REQUIRE_THROWS_AS(settings.set_max_files(""), std::invalid_argument);
                REQUIRE(settings.max_size_mib == 10);
                REQUIRE(settings.max_files == 5);
            }
        }

        WHEN("rotation is turned off with zero")
        {
            settings.set_max_size_mib("0");

            THEN("the size limit is zero")
            {
                REQUIRE(settings.max_size_mib == 0);
            }
        }
    }
}

SCENARIO("Default log settings keep TRACE lines out of the file")
{
    GIVEN("a log file configured from default settings")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "thewatcher-singlelog-level-test.log";
        std::error_code ec;
        use_log_file(path);
        auto& logger = thewatcher::logging::SingleLog::GetInstance();
        thewatcher::logging::LogSettings{}.apply(logger);
        logger.SetConsoleLogLevel(thewatcher::logging::LogLevel::L_OFF);

        WHEN("a TRACE and a NOTICE entry are written")
        {
            LOG_TRACE("level probe trace line");
            LOG_NOTICE("level probe notice line");

            THEN("only the NOTICE entry reaches the file")
            {
                REQUIRE(eventually(
                    [&] {
                        return file_contains(path, "level probe notice line");
                    },
                    2s, 20ms));
                REQUIRE_FALSE(file_contains(path, "level probe trace line"));
            }
        }

        logger.SetLogRotation(0, 0);
        std::filesystem::remove(path, ec);
    }
}

SCENARIO("SingleLog keeps the live log when a rotation cannot move it aside")
{
    GIVEN("a log limited to 600 bytes whose first backup name is a non-empty directory")
    {
        const auto path = thewatcher::test::process_temp_dir().path() / "thewatcher-singlelog-rotatefail-test.log";
        std::error_code ec;
        use_log_file(path);
        const auto blocker = std::filesystem::path(path.string() + ".1");
        std::filesystem::remove_all(blocker, ec);
        std::filesystem::create_directories(blocker / "inner");
        auto& logger = thewatcher::logging::SingleLog::GetInstance();
        logger.SetLogRotation(600, 1);

        WHEN("enough is written to trigger rotation, which fails")
        {
            const std::string filler(80, 'z');
            for (int i = 0; i < 20; ++i)
                LOG_NOTICE("rotatefail probe " + std::to_string(i) + " " + filler);

            THEN("no entry is lost and logging continues")
            {
                REQUIRE(eventually(
                    [&] {
                        return file_contains(path, "rotatefail probe 19");
                    },
                    2s, 20ms));
                REQUIRE(file_contains(path, "rotatefail probe 0 "));
                REQUIRE(file_contains(path, "rotatefail probe 10 "));
            }
        }

        logger.SetLogRotation(0, 0);
        std::filesystem::remove_all(blocker, ec);
        std::filesystem::remove(path, ec);
    }
}
