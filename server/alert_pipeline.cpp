#include "alert_pipeline.hpp"

#include "common/SingleLog.hpp"
#include "webhook_security.hpp"

#include <nlohmann/json.hpp>

namespace thewatcher::server
{

using json = nlohmann::json;

namespace
{
    bool is_alert_severity(const std::string& status)
    {
        return status == "yellow" || status == "amber" || status == "red";
    }
} // namespace

AlertPipeline::AlertPipeline(Store& store)
    : store_(store)
{
}

AlertPipeline::AlertPipeline(Store& store, NotificationWorker& worker)
    : store_(store)
    , worker_(&worker)
{
}

AlertOutcome AlertPipeline::raise(const AlertRequest& request)
{
    if (!is_alert_severity(request.new_status))
    {
        LOGF_WARNING("Alert dropped: invalid severity agent_id=%s indicator=%s severity=%s", request.agent_id.c_str(),
                     request.indicator.c_str(), request.new_status.c_str());
        return AlertOutcome::InvalidSeverity;
    }

    const auto agent = store_.get_agent(request.agent_id);
    if (agent && agent->maintenance)
    {
        LOGF_DEBUG("Alert suppressed by maintenance agent_id=%s indicator=%s", request.agent_id.c_str(),
                   request.indicator.c_str());
        return AlertOutcome::SuppressedMaintenance;
    }

    if (store_.is_silenced(request.agent_id, request.indicator, request.created_at))
    {
        LOGF_DEBUG("Alert suppressed by silence rule agent_id=%s indicator=%s", request.agent_id.c_str(),
                   request.indicator.c_str());
        return AlertOutcome::SuppressedSilence;
    }

    if (request.deduplicate && store_.has_open_alert(request.agent_id, request.indicator, request.new_status))
    {
        LOGF_DEBUG("Alert deduplicated agent_id=%s indicator=%s severity=%s", request.agent_id.c_str(),
                   request.indicator.c_str(), request.new_status.c_str());
        return AlertOutcome::Deduplicated;
    }

    AlertRecord alert;
    alert.agent_id = request.agent_id;
    alert.indicator = request.indicator;
    alert.old_status = request.old_status;
    alert.new_status = request.new_status;
    alert.message = request.message;
    alert.created_at = request.created_at;
    if (const auto runbook = store_.get_runbook(alert.agent_id, alert.indicator, alert.new_status))
        alert.runbook_url = runbook->url;

    const auto alert_id = store_.insert_alert(alert);
    LOGF_WARNING("Generated alert id=%lld agent_id=%s indicator=%s old=%s new=%s", static_cast<long long>(alert_id),
                 alert.agent_id.c_str(), alert.indicator.c_str(), alert.old_status.c_str(), alert.new_status.c_str());
    notify(alert, alert_id);
    return AlertOutcome::Raised;
}

// Queues the alert webhook. The URL is read and validated here and the
// payload is built here, so the worker thread never touches the store.
void AlertPipeline::notify(const AlertRecord& alert, int64_t alert_id)
{
    if (worker_ == nullptr)
        return;
    const auto webhook_url = store_.get_setting("notifications.webhook_url", "");
    if (webhook_url.empty())
        return;

    const auto target = parse_webhook_url(webhook_url);
    if (!target)
    {
        LOGF_WARNING("Skipping unsupported webhook URL: %s", webhook_url.c_str());
        return;
    }

    const json payload = {
        {"alert_id",    alert_id         },
        {"agent_id",    alert.agent_id   },
        {"indicator",   alert.indicator  },
        {"old_status",  alert.old_status },
        {"new_status",  alert.new_status },
        {"message",     alert.message    },
        {"created_at",  alert.created_at },
        {"runbook_url", alert.runbook_url},
    };

    Notification notification;
    notification.kind = NotificationKind::AlertWebhook;
    notification.url = webhook_url;
    notification.target = *target;
    // Invalid UTF-8 from an agent (#56) must not throw out of the pipeline.
    notification.body = payload.dump(-1, ' ', false, json::error_handler_t::replace);
    notification.tag = alert_id;
    notification.timeout_seconds = 2;
    notification.max_attempts = 3;
    worker_->submit(std::move(notification));
}

} // namespace thewatcher::server
