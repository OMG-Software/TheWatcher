#pragma once

#include "notification_worker.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

namespace thewatcher::server
{

// Build a fleet health summary JSON payload from current store state.
nlohmann::json build_report_json(Store& store, int64_t now_ms);

// Builds the digest notification for reports.webhook_url. Returns nullopt when
// no URL is configured or it fails the webhook security check (logged).
std::optional<Notification> build_report_notification(Store& store, int64_t now_ms);

// Digest schedule read from the reports.* settings.
struct ReportSchedule
{
    bool weekly = false;
    int hour = 8;        // UTC hour of day, 0-23
    int day_of_week = 1; // 0 = Sunday ... 6 = Saturday; weekly only
};

// Reads reports.schedule, reports.hour and reports.day_of_week. A missing,
// malformed or out-of-range value falls back to its default.
ReportSchedule load_report_schedule(Store& store);

// True when a digest is due: now is inside the scheduled UTC hour (and day for
// weekly), and the last digest was sent at least one period minus an hour ago.
// last_sent_ms == 0 means never sent.
bool report_due(const ReportSchedule& schedule, int64_t last_sent_ms, int64_t now_ms);

// Reads reports.last_sent_ms. Only a whole, non-negative number counts; anything
// else (empty, "123junk", negative) is treated as never sent (0).
int64_t load_report_last_sent_ms(Store& store);

// Start of the first scheduled slot strictly after after_ms.
int64_t next_report_time_ms(const ReportSchedule& schedule, int64_t after_ms);

// Delay before retrying a failed digest, doubling from one minute to 15 minutes.
// failures is the number of consecutive failures so far (>= 1).
int64_t report_retry_delay_ms(int failures);

} // namespace thewatcher::server
