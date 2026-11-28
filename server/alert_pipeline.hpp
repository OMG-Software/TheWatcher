#pragma once

#include "notification_worker.hpp"
#include "store.hpp"

#include <cstdint>
#include <string>

namespace thewatcher::server
{

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
