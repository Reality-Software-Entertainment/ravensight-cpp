// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "ravensight/queue.hpp"

#include <algorithm>

namespace ravensight {

void Queue::set_max_queue_size(int max_queue_size) {
    max_queue_size_ = std::max(1, max_queue_size);
    trim_to_cap(0);
}

int Queue::enqueue(Event event) {
    if (!is_active()) {
        return 0;
    }
    const int dropped = trim_to_cap(1);
    event.sequence = next_sequence_++;
    events_.push_back(std::move(event));
    return dropped;
}

void Queue::clear() {
    dropped_count_ += static_cast<int>(events_.size());
    events_.clear();
}

void Queue::set_server_tracking_enabled(bool enabled) {
    server_tracking_enabled_ = enabled;
    if (!enabled) {
        clear();
    }
}

void Queue::set_locally_enabled(bool enabled) {
    locally_enabled_ = enabled;
    if (!enabled) {
        clear();
    }
}

void Queue::set_session(std::string token, double expires_at_unix_seconds) {
    session_token_ = std::move(token);
    session_expires_at_seconds_ = expires_at_unix_seconds;
    ++session_count_;
    reset_backoff();
}

void Queue::invalidate_session() {
    session_token_.clear();
    session_expires_at_seconds_ = 0.0;
}

bool Queue::is_session_valid(double now_seconds) const {
    return !session_token_.empty() &&
           now_seconds < session_expires_at_seconds_ - limits::session_expiry_skew_seconds;
}

FlushPlan Queue::begin_flush(double now_seconds, bool force) {
    FlushPlan plan;
    if (!is_active()) {
        plan.decision = FlushDecision::disabled;
        return plan;
    }
    if (flush_in_progress_) {
        plan.decision = FlushDecision::already_flushing;
        return plan;
    }
    if (events_.empty()) {
        plan.decision = FlushDecision::empty;
        return plan;
    }
    if (!force && seconds_until_next_attempt(now_seconds) > 0.0) {
        plan.decision = FlushDecision::backing_off;
        return plan;
    }
    if (!is_session_valid(now_seconds)) {
        plan.decision = FlushDecision::needs_session;
        return plan;
    }

    plan.decision = FlushDecision::send;
    const std::size_t size = std::min<std::size_t>(
        events_.size(),
        static_cast<std::size_t>(std::min(limits::max_batch_size, batch_limit_)));
    plan.batch.assign(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(size));
    flush_in_progress_ = true;
    return plan;
}

void Queue::end_flush() {
    flush_in_progress_ = false;
}

void Queue::on_batch_accepted(std::size_t batch_size) {
    const std::size_t remove = std::min(batch_size, events_.size());
    events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(remove));
    sent_count_ += static_cast<int>(remove);
    ++batch_count_;
    batch_limit_ = limits::max_batch_size;
    reset_backoff();
}

int Queue::on_batch_accepted(const FlushPlan& plan) {
    int removed = 0;
    if (!plan.batch.empty()) {
        const unsigned long long first = plan.batch.front().sequence;
        const unsigned long long last = plan.batch.back().sequence;
        // Batches are always taken from the head, and sequences only grow,
        // so the accepted events (if still queued) are a prefix.
        while (!events_.empty() && events_.front().sequence >= first &&
               events_.front().sequence <= last) {
            events_.pop_front();
            ++removed;
        }
    }
    sent_count_ += removed;
    ++batch_count_;
    batch_limit_ = limits::max_batch_size;
    reset_backoff();
    return removed;
}

bool Queue::on_bad_request(const FlushPlan& plan) {
    const std::size_t batch_size = plan.batch.size();
    if (batch_size > 1) {
        on_bad_request(batch_size);
        return false;
    }
    batch_limit_ = limits::max_batch_size;
    if (batch_size == 1 && !events_.empty() &&
        events_.front().sequence == plan.batch.front().sequence) {
        events_.pop_front();
        ++dropped_count_;
        return true;
    }
    return false;
}

void Queue::on_unauthorized() {
    invalidate_session();
}

void Queue::on_rate_limited(double now_seconds, double retry_after_seconds) {
    if (retry_after_seconds > 0.0) {
        next_attempt_at_seconds_ = now_seconds + retry_after_seconds;
        return;
    }
    schedule_backoff(now_seconds);
}

void Queue::on_bad_request(std::size_t batch_size) {
    if (batch_size > 1) {
        const std::size_t half = std::max<std::size_t>(1, batch_size / 2);
        batch_limit_ = static_cast<int>(
            std::min<std::size_t>(half, static_cast<std::size_t>(limits::max_batch_size)));
        return;
    }
    // A single event the server will never take. Drop it so the rest of the
    // queue is not blocked behind it forever.
    if (!events_.empty()) {
        events_.pop_front();
        ++dropped_count_;
    }
    batch_limit_ = limits::max_batch_size;
}

void Queue::on_transport_failure(double now_seconds) {
    schedule_backoff(now_seconds);
}

void Queue::reset_backoff() {
    backoff_seconds_ = limits::default_retry_seconds;
    next_attempt_at_seconds_ = 0.0;
}

void Queue::schedule_backoff(double now_seconds) {
    next_attempt_at_seconds_ = now_seconds + backoff_seconds_;
    backoff_seconds_ = std::min(backoff_seconds_ * 2.0, limits::max_backoff_seconds);
}

double Queue::seconds_until_next_attempt(double now_seconds) const {
    return std::max(0.0, next_attempt_at_seconds_ - now_seconds);
}

double Queue::retry_timer_seconds(double now_seconds) const {
    return std::max(seconds_until_next_attempt(now_seconds), limits::min_retry_seconds);
}

QueueStats Queue::stats() const {
    QueueStats stats;
    stats.queued = static_cast<int>(events_.size());
    stats.sent = sent_count_;
    stats.dropped = dropped_count_;
    stats.batches = batch_count_;
    stats.sessions = session_count_;
    return stats;
}

int Queue::trim_to_cap(int headroom) {
    const int target = std::max(0, max_queue_size_ - headroom);
    int dropped = 0;
    while (static_cast<int>(events_.size()) > target) {
        events_.pop_front();
        ++dropped;
    }
    dropped_count_ += dropped;
    return dropped;
}

} // namespace ravensight
