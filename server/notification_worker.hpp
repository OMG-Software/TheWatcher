#pragma once

#include "webhook_security.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace thewatcher::server
{

enum class NotificationKind
{
    AlertWebhook,
    ReportDigest
};

// One outbound HTTP POST. The producer validates the URL (parse_webhook_url)
// and builds the payload, so the worker never reads the store.
struct Notification
{
    NotificationKind kind = NotificationKind::AlertWebhook;
    std::string url; // original URL, for log lines only
    WebhookTarget target;
    std::string body; // JSON payload
    int64_t tag = 0;  // alert id, or the report's generation time
    int timeout_seconds = 2;
    int max_attempts = 1; // transport errors and non-2xx responses are retried
};

// Outcome of one notification, handed back to the producer's thread.
struct DeliveryResult
{
    NotificationKind kind = NotificationKind::AlertWebhook;
    int64_t tag = 0;
    bool success = false;
    int http_status = 0; // 0 when the request never got a response
    int attempts = 0;
};

// True for any 2xx HTTP status.
bool is_success_status(int http_status);

// POSTs the notification and returns the HTTP status, or 0 when no response
// arrived (connect failure, timeout). Blocking; used by the worker and by the
// synchronous "send report now" API.
int deliver_notification(const Notification& notification);

// Delivers notifications on a dedicated thread so a slow or dead webhook
// cannot stall the data plane (#48).
//
// - submit() never blocks: when the bounded queue is full the notification is
//   dropped and false is returned.
// - Each attempt is bounded by Notification::timeout_seconds, and a failed
//   attempt is retried with doubling backoff up to max_attempts.
// - The destructor asks the thread to stop (std::jthread), abandons queued
//   notifications and joins. A request in flight finishes within its timeout.
// - The worker never touches the Store; the producer collects results with
//   drain_results() on its own thread.
class NotificationWorker
{
public:
    using Deliverer = std::function<int(const Notification&)>;

    struct Options
    {
        std::size_t capacity = 256;
        std::chrono::milliseconds retry_backoff{1000}; // doubles per attempt
    };

    NotificationWorker();
    explicit NotificationWorker(Options options, Deliverer deliverer = deliver_notification);
    ~NotificationWorker();

    NotificationWorker(const NotificationWorker&) = delete;
    NotificationWorker& operator=(const NotificationWorker&) = delete;

    // Queues a notification. Returns false (and drops it) when the queue is full.
    bool submit(Notification notification);

    // Moves out the results of notifications finished since the last call.
    std::vector<DeliveryResult> drain_results();

    // Notifications queued or in flight.
    std::size_t pending() const;

    // Notifications dropped because the queue was full.
    std::size_t dropped() const;

private:
    void run(std::stop_token stop);
    DeliveryResult deliver(const Notification& notification, std::stop_token stop);

    Options options_;
    Deliverer deliverer_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Notification> queue_;
    std::vector<DeliveryResult> results_;
    std::size_t in_flight_ = 0;
    std::size_t dropped_ = 0;
    std::jthread thread_; // last member: starts only once the others exist
};

} // namespace thewatcher::server
