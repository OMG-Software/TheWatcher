#include "report_generator.hpp"

#include "common/SingleLog.hpp"
#include "status_engine.hpp"
#include "webhook_security.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace thewatcher::server
{

using json = nlohmann::json;

json build_report_json(Store& store, int64_t now_ms)
{
    const auto agents = store.list_approved_agents();
    const auto alerts = store.list_unacknowledged_alerts();

    int total = static_cast<int>(agents.size());
    int offline = 0;
    int maintenance = 0;
    for (const auto& a : agents)
    {
        if (a.maintenance)
            ++maintenance;
        else if (!a.connected)
            ++offline;
    }

    // Alert counts by severity
    int yellow_count = 0, amber_count = 0, red_count = 0;
    for (const auto& al : alerts)
    {
        const auto s = status_from_string(al.new_status);
        if (s == IndicatorStatus::Red)
            ++red_count;
        else if (s == IndicatorStatus::Amber)
            ++amber_count;
        else if (s == IndicatorStatus::Yellow)
            ++yellow_count;
    }

    // Top-5 agents by oldest unacknowledged alert
    struct AgentAge
    {
        std::string agent_id;
        int64_t oldest_alert_ms = 0;
        int alert_count = 0;
    };
    std::unordered_map<std::string, AgentAge> agent_age_map;
    for (const auto& al : alerts)
    {
        auto& aa = agent_age_map[al.agent_id];
        aa.agent_id = al.agent_id;
        ++aa.alert_count;
        if (aa.oldest_alert_ms == 0 || al.created_at < aa.oldest_alert_ms)
            aa.oldest_alert_ms = al.created_at;
    }
    std::vector<AgentAge> sorted_agents;
    sorted_agents.reserve(agent_age_map.size());
    for (auto& [id, aa] : agent_age_map)
        sorted_agents.push_back(aa);
    std::sort(sorted_agents.begin(), sorted_agents.end(), [](const AgentAge& a, const AgentAge& b) {
        return a.oldest_alert_ms < b.oldest_alert_ms;
    });
    if (sorted_agents.size() > 5)
        sorted_agents.resize(5);

    // Build agent lookup for names
    std::unordered_map<std::string, std::string> agent_names;
    for (const auto& a : agents)
        agent_names[a.agent_id] = a.hostname.empty() ? a.agent_id : a.hostname;

    json top5 = json::array();
    for (const auto& aa : sorted_agents)
    {
        const int64_t age_seconds = (now_ms - aa.oldest_alert_ms) / 1000;
        top5.push_back({
            {"agent_id",                 aa.agent_id                                                               },
            {"name",                     agent_names.count(aa.agent_id) ? agent_names.at(aa.agent_id) : aa.agent_id},
            {"alert_count",              aa.alert_count                                                            },
            {"oldest_alert_age_seconds", age_seconds                                                               }
        });
    }

    return json{
        {"generated_at",            now_ms},
        {"agents",
         {{"total", total},
          {"online", total - offline - maintenance},
          {"offline", offline},
          {"maintenance", maintenance}}   },
        {"alerts",
         {{"total", static_cast<int>(alerts.size())},
          {"red", red_count},
          {"amber", amber_count},
          {"yellow", yellow_count}}       },
        {"top_agents_by_alert_age", top5  }
    };
}

std::optional<Notification> build_report_notification(Store& store, int64_t now_ms)
{
    const auto webhook_url = store.get_setting("reports.webhook_url", "");
    if (webhook_url.empty())
    {
        LOG_DEBUG("No reports.webhook_url configured, skipping report");
        return std::nullopt;
    }

    const auto target = parse_webhook_url(webhook_url);
    if (!target)
    {
        LOGF_WARNING("Reports: invalid or private webhook URL: %s", webhook_url.c_str());
        return std::nullopt;
    }

    Notification notification;
    notification.kind = NotificationKind::ReportDigest;
    notification.url = webhook_url;
    notification.target = *target;
    // Invalid UTF-8 from an agent (#56) must not throw out of the report.
    notification.body = build_report_json(store, now_ms).dump(-1, ' ', false, json::error_handler_t::replace);
    notification.tag = now_ms;
    notification.timeout_seconds = 5;
    notification.max_attempts = 1; // the scheduler retries with backoff
    return notification;
}

namespace
{
    constexpr int64_t kHourMs = 3'600'000LL;
    constexpr int64_t kDayMs = 24 * kHourMs;

    // Parses a setting that must be one whole number within [min, max];
    // otherwise (empty, trailing junk, out of range) the fallback.
    int64_t whole_number_setting(Store& store, const std::string& key, int64_t fallback, int64_t min, int64_t max)
    {
        const auto text = store.get_setting(key, "");
        try
        {
            std::size_t used = 0;
            const long long value = std::stoll(text, &used);
            if (used == text.size() && value >= min && value <= max)
                return value;
        }
        catch (const std::exception&)
        {
            // empty or not a number: use the fallback
        }
        return fallback;
    }

    int int_setting(Store& store, const std::string& key, int fallback, int min, int max)
    {
        return static_cast<int>(whole_number_setting(store, key, fallback, min, max));
    }

    // Floor division so times before 1970 still land in the right day.
    int64_t floor_div(int64_t a, int64_t b)
    {
        const int64_t q = a / b;
        return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
    }

    int hour_of_day(int64_t ms)
    {
        return static_cast<int>(floor_div(ms, kHourMs) - floor_div(ms, kDayMs) * 24);
    }

    // 0 = Sunday. 1970-01-01 was a Thursday (4).
    int day_of_week(int64_t ms)
    {
        const int64_t days = floor_div(ms, kDayMs);
        return static_cast<int>(((days % 7) + 7 + 4) % 7);
    }

    bool in_slot(const ReportSchedule& schedule, int64_t ms)
    {
        return hour_of_day(ms) == schedule.hour && (!schedule.weekly || day_of_week(ms) == schedule.day_of_week);
    }
} // namespace

ReportSchedule load_report_schedule(Store& store)
{
    ReportSchedule schedule;
    schedule.weekly = store.get_setting("reports.schedule", "daily") == "weekly";
    schedule.hour = int_setting(store, "reports.hour", 8, 0, 23);
    schedule.day_of_week = int_setting(store, "reports.day_of_week", 1, 0, 6);
    return schedule;
}

int64_t load_report_last_sent_ms(Store& store)
{
    return whole_number_setting(store, "reports.last_sent_ms", 0, 0, std::numeric_limits<int64_t>::max());
}

bool report_due(const ReportSchedule& schedule, int64_t last_sent_ms, int64_t now_ms)
{
    if (!in_slot(schedule, now_ms))
        return false;
    // One hour short of the period, so a digest sent late in its slot does not
    // push the next one out of the following slot.
    const int64_t min_gap_ms = (schedule.weekly ? 7 * kDayMs : kDayMs) - kHourMs;
    return last_sent_ms <= 0 || now_ms - last_sent_ms >= min_gap_ms;
}

int64_t next_report_time_ms(const ReportSchedule& schedule, int64_t after_ms)
{
    // Slots start on the hour, so walk hour starts: at most 7 days of hours.
    int64_t candidate = (floor_div(after_ms, kHourMs) + 1) * kHourMs;
    while (!in_slot(schedule, candidate))
        candidate += kHourMs;
    return candidate;
}

int64_t report_retry_delay_ms(int failures)
{
    constexpr int64_t base_ms = 60'000;
    constexpr int64_t max_ms = 15 * 60'000;
    int64_t delay = base_ms;
    for (int i = 1; i < failures && delay < max_ms; ++i)
        delay *= 2;
    return std::min(delay, max_ms);
}

} // namespace thewatcher::server
