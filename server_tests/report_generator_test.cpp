// ReportGenerator: JSON shape, schedule, next-run calculation and the
// notification handed to the worker (issues #49 and #137).
#include "../server/report_generator.hpp"
#include "../server/store_sqlite.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <ctime>

using namespace thewatcher::server;

namespace
{
constexpr int64_t kHour = 3'600'000LL;
constexpr int64_t kDay = 24 * kHour;

// 2026-11-23 is a Monday. Returns that day at the given UTC hour.
int64_t monday_at(int hour, int day_offset = 0)
{
    // days since 1970-01-01 of 2026-11-23
    constexpr int64_t days = 20780;
    return (days + day_offset) * kDay + hour * kHour;
}

void add_agent(SqliteStore& store, const std::string& id, bool connected, bool maintenance)
{
    AgentRecord agent;
    agent.agent_id = id;
    agent.hostname = id + "-host";
    agent.approved = true;
    agent.connected = connected;
    agent.maintenance = maintenance;
    agent.last_seen = 1;
    store.upsert_agent(agent);
}

void add_alert(SqliteStore& store, const std::string& agent, const std::string& status, int64_t created)
{
    AlertRecord alert;
    alert.agent_id = agent;
    alert.indicator = "cpu";
    alert.old_status = "green";
    alert.new_status = status;
    alert.created_at = created;
    store.insert_alert(alert);
}
} // namespace

SCENARIO("The report JSON summarises fleet health")
{
    GIVEN("four agents (one offline, one in maintenance) and three open alerts")
    {
        SqliteStore store(":memory:");
        add_agent(store, "online1", true, false);
        add_agent(store, "online2", true, false);
        add_agent(store, "gone", false, false);
        add_agent(store, "paused", false, true);
        add_alert(store, "gone", "red", 1000);
        add_alert(store, "gone", "amber", 2000);
        add_alert(store, "online1", "yellow", 3000);

        WHEN("the report is built 10 seconds after the oldest alert")
        {
            const auto report = build_report_json(store, 11'000);

            THEN("counts, severities and the oldest-alert ranking are reported")
            {
                REQUIRE(report["generated_at"] == 11'000);
                REQUIRE(report["agents"]["total"] == 4);
                REQUIRE(report["agents"]["online"] == 2);
                REQUIRE(report["agents"]["offline"] == 1);
                REQUIRE(report["agents"]["maintenance"] == 1);
                REQUIRE(report["alerts"]["total"] == 3);
                REQUIRE(report["alerts"]["red"] == 1);
                REQUIRE(report["alerts"]["amber"] == 1);
                REQUIRE(report["alerts"]["yellow"] == 1);

                const auto& top = report["top_agents_by_alert_age"];
                REQUIRE(top.size() == 2);
                REQUIRE(top[0]["agent_id"] == "gone");
                REQUIRE(top[0]["name"] == "gone-host");
                REQUIRE(top[0]["alert_count"] == 2);
                REQUIRE(top[0]["oldest_alert_age_seconds"] == 10);
                REQUIRE(top[1]["agent_id"] == "online1");
            }
        }
    }

    GIVEN("an empty fleet")
    {
        SqliteStore store(":memory:");

        THEN("the report has zero counts and an empty ranking")
        {
            const auto report = build_report_json(store, 1000);
            REQUIRE(report["agents"]["total"] == 0);
            REQUIRE(report["alerts"]["total"] == 0);
            REQUIRE(report["top_agents_by_alert_age"].empty());
        }
    }
}

SCENARIO("The report notification honours the webhook settings")
{
    GIVEN("no report webhook")
    {
        SqliteStore store(":memory:");

        THEN("there is nothing to send")
        {
            REQUIRE_FALSE(build_report_notification(store, 1000).has_value());
        }
    }

    GIVEN("a private report webhook URL")
    {
        SqliteStore store(":memory:");
        store.set_setting("reports.webhook_url", "http://127.0.0.1/hook");

        THEN("the security check refuses it")
        {
            REQUIRE_FALSE(build_report_notification(store, 1000).has_value());
        }
    }

    GIVEN("a valid report webhook URL")
    {
        SqliteStore store(":memory:");
        store.set_setting("reports.webhook_url", "http://hooks.example.net:8080/digest");

        WHEN("the notification is built")
        {
            const auto n = build_report_notification(store, 1234);

            THEN("it targets the URL, carries the report and makes a single attempt")
            {
                REQUIRE(n.has_value());
                REQUIRE(n->kind == NotificationKind::ReportDigest);
                REQUIRE(n->target.host == "hooks.example.net");
                REQUIRE(n->target.port == 8080);
                REQUIRE(n->target.path == "/digest");
                REQUIRE(n->tag == 1234);
                REQUIRE(n->max_attempts == 1);
                REQUIRE(n->body.find("\"generated_at\":1234") != std::string::npos);
            }
        }
    }
}

SCENARIO("The report schedule is read from settings with safe defaults")
{
    GIVEN("no schedule settings")
    {
        SqliteStore store(":memory:");

        THEN("the defaults are daily at 08:00 UTC")
        {
            const auto schedule = load_report_schedule(store);
            REQUIRE_FALSE(schedule.weekly);
            REQUIRE(schedule.hour == 8);
            REQUIRE(schedule.day_of_week == 1);
        }
    }

    GIVEN("weekly settings")
    {
        SqliteStore store(":memory:");
        store.set_setting("reports.schedule", "weekly");
        store.set_setting("reports.hour", "17");
        store.set_setting("reports.day_of_week", "5");

        THEN("they are used")
        {
            const auto schedule = load_report_schedule(store);
            REQUIRE(schedule.weekly);
            REQUIRE(schedule.hour == 17);
            REQUIRE(schedule.day_of_week == 5);
        }
    }

    GIVEN("malformed or out-of-range values")
    {
        SqliteStore store(":memory:");
        store.set_setting("reports.hour", "banana");
        store.set_setting("reports.day_of_week", "9");

        THEN("they fall back to the defaults instead of throwing")
        {
            const auto schedule = load_report_schedule(store);
            REQUIRE(schedule.hour == 8);
            REQUIRE(schedule.day_of_week == 1);
        }

        AND_GIVEN("an hour of 24")
        {
            store.set_setting("reports.hour", "24");
            REQUIRE(load_report_schedule(store).hour == 8);
        }
    }
}

SCENARIO("A daily digest is due once, in its hour")
{
    GIVEN("a daily 08:00 UTC schedule")
    {
        const ReportSchedule schedule{false, 8, 1};

        THEN("it is due inside the hour and not outside it")
        {
            REQUIRE(report_due(schedule, 0, monday_at(8)));
            REQUIRE(report_due(schedule, 0, monday_at(8) + kHour - 1));
            REQUIRE_FALSE(report_due(schedule, 0, monday_at(7) + kHour - 1));
            REQUIRE_FALSE(report_due(schedule, 0, monday_at(9)));
        }

        THEN("a digest already sent this hour is not sent again after a restart")
        {
            const auto sent = monday_at(8) + 5 * 60'000;
            REQUIRE_FALSE(report_due(schedule, sent, sent + 10 * 60'000));
        }

        THEN("it is due again the next day")
        {
            const auto sent = monday_at(8) + 30 * 60'000;
            REQUIRE(report_due(schedule, sent, monday_at(8, 1)));
        }
    }
}

SCENARIO("A weekly digest is due only on its weekday")
{
    GIVEN("a weekly Monday 08:00 UTC schedule")
    {
        const ReportSchedule schedule{true, 8, 1};

        THEN("Monday 08:00 is due, Tuesday 08:00 is not")
        {
            REQUIRE(report_due(schedule, 0, monday_at(8)));
            REQUIRE_FALSE(report_due(schedule, 0, monday_at(8, 1)));
        }

        THEN("a digest sent last Monday makes the next Monday due, but not a few days later")
        {
            REQUIRE(report_due(schedule, monday_at(8), monday_at(8, 7)));
            REQUIRE_FALSE(report_due(schedule, monday_at(8), monday_at(8, 0) + 10 * 60'000));
        }
    }
}

SCENARIO("The next scheduled run is calculated in UTC")
{
    GIVEN("a daily 08:00 schedule")
    {
        const ReportSchedule schedule{false, 8, 1};

        THEN("before 08:00 the next run is today, from 08:00 on it is tomorrow")
        {
            REQUIRE(next_report_time_ms(schedule, monday_at(6)) == monday_at(8));
            REQUIRE(next_report_time_ms(schedule, monday_at(8) - 1) == monday_at(8));
            REQUIRE(next_report_time_ms(schedule, monday_at(8)) == monday_at(8, 1));
            REQUIRE(next_report_time_ms(schedule, monday_at(20)) == monday_at(8, 1));
        }
    }

    GIVEN("a weekly Friday 17:00 schedule")
    {
        const ReportSchedule schedule{true, 17, 5};

        THEN("the next run is the coming Friday, a week later once it has passed")
        {
            REQUIRE(next_report_time_ms(schedule, monday_at(8)) == monday_at(17, 4));
            REQUIRE(next_report_time_ms(schedule, monday_at(17, 4)) == monday_at(17, 11));
        }
    }
}

SCENARIO("A failed digest is retried with growing backoff (issue #49)")
{
    THEN("the delay doubles from one minute and is capped at fifteen")
    {
        REQUIRE(report_retry_delay_ms(1) == 60'000);
        REQUIRE(report_retry_delay_ms(2) == 120'000);
        REQUIRE(report_retry_delay_ms(3) == 240'000);
        REQUIRE(report_retry_delay_ms(4) == 480'000);
        REQUIRE(report_retry_delay_ms(5) == 900'000);
        REQUIRE(report_retry_delay_ms(50) == 900'000);
    }
}

SCENARIO("The last-sent time is read strictly (issue #49)")
{
    GIVEN("a store with reports.last_sent_ms set")
    {
        SqliteStore store(":memory:");

        THEN("only a whole non-negative number counts")
        {
            REQUIRE(load_report_last_sent_ms(store) == 0);
            store.set_setting("reports.last_sent_ms", "1700000000000");
            REQUIRE(load_report_last_sent_ms(store) == 1'700'000'000'000LL);
            store.set_setting("reports.last_sent_ms", "123junk");
            REQUIRE(load_report_last_sent_ms(store) == 0);
            store.set_setting("reports.last_sent_ms", "-5");
            REQUIRE(load_report_last_sent_ms(store) == 0);
            store.set_setting("reports.last_sent_ms", "");
            REQUIRE(load_report_last_sent_ms(store) == 0);
        }
    }
}
