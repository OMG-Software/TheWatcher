#pragma once

#include "notification_worker.hpp"
#include "store.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>

namespace thewatcher::server
{

// True when a silence rule covers an alert on `indicator`: a rule for the exact
// name, for its kind ("disk" covers "disk:/") or for '*'. See indicators.hpp.
// The pipeline applies it once per alert; nothing else checks silences.
bool is_alert_silenced(Store& store, const std::string& agent_id, const std::string& indicator, int64_t now_ms);

// The JSON body POSTed to the webhook for a new alert. It carries the runbook
// link attached to the alert and the agent's own markdown runbook (#53).
nlohmann::json build_webhook_payload(Store& store, const AlertRecord& alert, int64_t alert_id);

// An alert any part of the server wants to raise: a metric transition, an
// offline agent or a log match.
struct AlertRequest
{
    std::string agent_id;
    std::string indicator;
    std::string old_status;
    std::string new_status; // must be "yellow", "amber" or "red"
    std::string message;
    int64_t created_at = 0;
    // Skip the alert when an open (unacknowledged, undeleted) alert with the
    // same agent, indicator and severity exists. Used by sources that can
    // repeat without a state change, such as log matches.
    bool deduplicate = false;
};

// A log match on `indicator` with `severity` ("yellow", "amber" or "red") as a request for the
// pipeline; the message quotes at most the first 200 characters of the matched line. Log matches can
// repeat without a state change, so the request de-duplicates.
AlertRequest log_match_request(const std::string& agent_id, const std::string& indicator, const std::string& severity,
                               const std::string& path, const std::string& matched_line, int64_t now_ms);

enum class AlertOutcome
{
    Raised,
    InvalidSeverity,
    SuppressedMaintenance,
    SuppressedSilence,
    Deduplicated
};

// The single path every alert takes (#52). In order it: validates the
// severity, skips agents in maintenance, applies silences, optionally
// de-duplicates, attaches the runbook, stores the alert and queues the
// webhook. New alert sources call raise(); new per-alert behaviour (for
// example more notification channels) is added here once.
class AlertPipeline
{
public:
    // Alerts are stored but no webhook is sent.
    explicit AlertPipeline(Store& store);
    // Alerts are stored and the configured webhook is queued on `worker`.
    AlertPipeline(Store& store, NotificationWorker& worker);

    AlertOutcome raise(const AlertRequest& request);

private:
    void notify(const AlertRecord& alert, int64_t alert_id);

    Store& store_;
    NotificationWorker* worker_ = nullptr; // optional collaborator, never owned
};

} // namespace thewatcher::server
