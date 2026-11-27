#include "notification_worker.hpp"

#include "common/SingleLog.hpp"

#include <algorithm>
#include <utility>

#include <httplib.h>

namespace thewatcher::server
{

bool is_success_status(int http_status)
{
    return http_status >= 200 && http_status < 300;
}

int deliver_notification(const Notification& notification)
{
    httplib::Client client(notification.target.host, notification.target.port);
    client.set_connection_timeout(notification.timeout_seconds, 0);
    client.set_read_timeout(notification.timeout_seconds, 0);
    client.set_write_timeout(notification.timeout_seconds, 0);
    const auto response = client.Post(notification.target.path, notification.body, "application/json");
    return response ? response->status : 0;
}

NotificationWorker::NotificationWorker()
    : NotificationWorker(Options{})
{
}

NotificationWorker::NotificationWorker(Options options, Deliverer deliverer)
    : options_(options)
    , deliverer_(std::move(deliverer))
    , thread_([this](std::stop_token stop) {
        run(stop);
    })
{
}

NotificationWorker::~NotificationWorker()
{
    // The jthread joins on destruction; ask it to stop first so the wait on
    // the condition variable (which honours the stop token) ends promptly.
    thread_.request_stop();
}

bool NotificationWorker::submit(Notification notification)
{
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= options_.capacity)
        {
            ++dropped_;
            LOGF_WARNING("Notification queue full (%zu); dropping notification url=%s", options_.capacity,
                         notification.url.c_str());
            return false;
        }
        queue_.push_back(std::move(notification));
    }
    wake_.notify_one();
    return true;
}

std::vector<DeliveryResult> NotificationWorker::drain_results()
{
    std::lock_guard lock(mutex_);
    return std::exchange(results_, {});
}

std::size_t NotificationWorker::pending() const
{
    std::lock_guard lock(mutex_);
    return queue_.size() + in_flight_;
}

std::size_t NotificationWorker::dropped() const
{
    std::lock_guard lock(mutex_);
    return dropped_;
}

void NotificationWorker::run(std::stop_token stop)
{
    // A stop request must wake the waits below. Taking the mutex first means
    // the notify cannot fall between a predicate check and the wait.
    const std::stop_callback wake_on_stop(stop, [this] {
        {
            std::lock_guard lock(mutex_);
        }
        wake_.notify_all();
    });

    while (true)
    {
        Notification next;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this, &stop] {
                return !queue_.empty() || stop.stop_requested();
            });
            if (stop.stop_requested())
                return;
            next = std::move(queue_.front());
            queue_.pop_front();
            ++in_flight_;
        }

        DeliveryResult result{next.kind, next.tag, false, 0, 1};
        try
        {
            result = deliver(next, stop);
        }
        catch (const std::exception& e)
        {
            LOGF_WARNING("Notification delivery threw: %s", e.what());
        }

        std::lock_guard lock(mutex_);
        --in_flight_;
        results_.push_back(result);
    }
}

// Tries up to max_attempts times, waiting retry_backoff * 2^n between tries.
// A stop request interrupts the wait so shutdown stays prompt.
DeliveryResult NotificationWorker::deliver(const Notification& notification, std::stop_token stop)
{
    DeliveryResult result{notification.kind, notification.tag, false, 0, 0};
    const int attempts = std::max(1, notification.max_attempts);
    auto backoff = options_.retry_backoff;
    for (int attempt = 1; attempt <= attempts && !stop.stop_requested(); ++attempt)
    {
        result.attempts = attempt;
        result.http_status = deliverer_(notification);
        result.success = is_success_status(result.http_status);
        if (result.success)
        {
            LOGF_DEBUG("Notification delivered tag=%lld status=%d", static_cast<long long>(notification.tag),
                       result.http_status);
            return result;
        }
        LOGF_WARNING("Notification failed tag=%lld url=%s status=%d attempt=%d/%d",
                     static_cast<long long>(notification.tag), notification.url.c_str(), result.http_status, attempt,
                     attempts);
        if (attempt < attempts)
        {
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, backoff, [&stop] {
                return stop.stop_requested();
            });
            backoff *= 2;
        }
    }
    return result;
}

} // namespace thewatcher::server
