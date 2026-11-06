// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The Ravensight protocol state machine: the offline queue, batching, the
// session validity window, the retry schedule and oversize batch splitting.
//
// Nothing in this header touches libcurl, threads or a real clock. Every
// decision is a pure function of the current state plus a caller supplied
// clock reading, which is what lets the unit tests drive every retry path
// deterministically. The Client owns one of these behind a mutex and drives
// it from its worker thread.

#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace ravensight {

namespace limits {

/// Server hard limit on POST /track/batch.
inline constexpr int max_batch_size = 50;

/// Default cap on unsent events held in memory. Oldest are dropped first.
inline constexpr int default_queue_cap = 500;

/// First retry delay, and the value backoff resets to after a success.
inline constexpr double default_retry_seconds = 10.0;

/// Exponential backoff ceiling.
inline constexpr double max_backoff_seconds = 300.0;

/// Treat a session as expired slightly early, so a token does not lapse mid
/// flight between the validity check and the server receiving the request.
inline constexpr double session_expiry_skew_seconds = 5.0;

/// Floor on any scheduled retry wait, so a zero delay never busy loops.
inline constexpr double min_retry_seconds = 0.5;

/// Backoff waits are multiplied by a random factor in [backoff_jitter_floor,
/// 1), so many games that failed together do not retry together. Retry-After
/// is never spread.
inline constexpr double backoff_jitter_floor = 0.5;

/// The largest event the API accepts: its data serialized as JSON, with a
/// error event's message, stack and trail counted one byte a character (as the
/// server counts). Bigger events are dropped before they are queued.
inline constexpr std::size_t max_event_bytes = 8192;

} // namespace limits

/// One queued analytics event. Data is carried as already serialized JSON.
struct Event {
    /// Event name, for example "level_completed".
    std::string name;

    /// A JSON object literal, "{}" when the event carries no payload.
    std::string data_json = "{}";

    /// Unix milliseconds, captured when the event happened rather than when
    /// it is sent.
    long long timestamp = 0;

    /// The /session answer's origin object, as JSON text exactly as
    /// received, of the session this event was tracked under. Empty when
    /// that session had none or none was open yet. Sent as the event's
    /// top-level origin when it is not the current session's.
    std::string origin_json;

    /// Assigned by Queue::enqueue, strictly increasing. Lets an acknowledgment
    /// remove exactly the events it carried, even if the queue was cleared
    /// and refilled while the batch was in flight. Callers leave it at 0.
    unsigned long long sequence = 0;

    /// An automatic error capture. Gameplay events always stay ahead of
    /// these in the queue, and at the cap a capture is dropped first. The
    /// queue file keeps the flag, so the rule survives a restart.
    bool automatic_error = false;
};

/// Why a flush attempt did or did not produce a batch to send.
enum class FlushDecision {
    /// A batch is ready in FlushPlan::batch.
    send,

    /// Nothing is queued.
    empty,

    /// A batch is already in flight.
    already_flushing,

    /// Still inside the retry window from an earlier failure.
    backing_off,

    /// Events are waiting but there is no valid session token yet.
    needs_session,

    /// Tracking is off, either by the server kill switch or a local opt out.
    disabled,
};

/// The outcome of Queue::begin_flush.
struct FlushPlan {
    FlushDecision decision = FlushDecision::empty;

    /// Populated only when decision is send. Never larger than the current
    /// batch limit, which is max_batch_size except while splitting after a 400.
    std::vector<Event> batch;

    bool should_send() const { return decision == FlushDecision::send; }
};

/// Counters, useful for a debug overlay or a test assertion.
struct QueueStats {
    int queued = 0;
    int sent = 0;
    int dropped = 0;
    int batches = 0;
    int sessions = 0;
};

/// The queue and retry state machine. Single threaded by contract: the
/// Client drives it under its own mutex, so it takes no locks itself.
class Queue {
public:
    // --- Configuration -----------------------------------------------------

    /// Values below 1 are clamped to 1. Shrinking the cap drops oldest events immediately.
    void set_max_queue_size(int max_queue_size);
    int max_queue_size() const { return max_queue_size_; }

    // --- Enqueue -----------------------------------------------------------

    /// Adds an event, dropping oldest events first if the cap is already met.
    /// Returns how many were dropped to make room, so the caller can log it.
    /// A no-op returning 0 when tracking is off.
    ///
    /// Automatic error captures (Event::automatic_error) go behind every
    /// gameplay event: a gameplay event is inserted ahead of any queued
    /// capture, so an error burst never holds gameplay back. At the cap the
    /// oldest capture is dropped first; a new capture with only gameplay
    /// events queued is itself dropped (counted, and returned as 1).
    int enqueue(Event event);

    int count() const { return static_cast<int>(events_.size()); }
    bool is_empty() const { return events_.empty(); }
    const std::deque<Event>& events() const { return events_; }

    /// Discards everything queued and counts it as dropped.
    void clear();

    /// Gives every queued event with a sequence above after_sequence and no
    /// origin yet this origin (the session they were tracked in, which
    /// opened after they were queued). Returns how many changed.
    int stamp_origin(unsigned long long after_sequence, const std::string& origin_json);

    /// The sequence of the newest queued event, 0 when empty.
    unsigned long long last_sequence() const { return events_.empty() ? 0 : events_.back().sequence; }

    // --- Enablement --------------------------------------------------------

    /// The server side kill switch from GET /settings. Disabling discards the queue.
    void set_server_tracking_enabled(bool enabled);
    bool is_server_tracking_enabled() const { return server_tracking_enabled_; }

    /// Local opt in and opt out, for a privacy toggle. Disabling discards the queue.
    void set_locally_enabled(bool enabled);
    bool is_locally_enabled() const { return locally_enabled_; }

    /// True only when both the server and the local toggle allow tracking.
    bool is_active() const { return server_tracking_enabled_ && locally_enabled_; }

    // --- Session -----------------------------------------------------------

    /// Stores a fresh session, counts it, and clears any pending backoff.
    void set_session(std::string token, double expires_at_unix_seconds);
    void invalidate_session();
    bool is_session_valid(double now_seconds) const;
    const std::string& session_token() const { return session_token_; }
    double session_expires_at() const { return session_expires_at_seconds_; }

    // --- Flush lifecycle ---------------------------------------------------

    /// Decides whether a batch can go out right now and, if so, marks a flush
    /// in progress and returns a copy of the leading events. Events are not
    /// removed here: they leave the queue only in on_batch_accepted, so a
    /// failed send never loses them.
    ///
    /// force ignores the backoff window, for a flush on shutdown or an
    /// explicit flush call. It never ignores a server's Retry-After wait, the
    /// session or the kill switch.
    FlushPlan begin_flush(double now_seconds, bool force);

    /// Clears the in flight marker. Safe to call even if no flush was started.
    void end_flush();

    bool is_flush_in_progress() const { return flush_in_progress_; }

    // --- Responses ---------------------------------------------------------

    /// HTTP 202 for the batch in plan. Removes only the events that batch
    /// carried, matched by sequence number wherever they now sit, so a 202
    /// that lands after a clear() and fresh enqueues cannot erase the new
    /// events, and a gameplay event inserted ahead of an in-flight capture
    /// is not mistaken for it. Restores the
    /// full batch limit and clears any backoff. Returns how many were removed.
    int on_batch_accepted(const FlushPlan& plan);

    /// Positional variant: removes the first batch_size events. Only safe
    /// when nothing cleared the queue since begin_flush. Prefer the FlushPlan
    /// overload.
    void on_batch_accepted(std::size_t batch_size);

    /// HTTP 400 naming one event by its index in the batch: drops that event
    /// only (if still queued), restores the full batch limit. True when one
    /// was dropped.
    bool drop_indexed(const FlushPlan& plan, std::size_t index);

    /// Any other 400 on the batch: drops every event the batch carried that
    /// is still queued. Returns how many.
    int drop_batch(const FlushPlan& plan);

    /// HTTP 401. Drops the session token and leaves every event queued, so
    /// the same batch is re-sent once a fresh session is issued.
    void on_unauthorized();

    /// HTTP 429. Honors retry_after_seconds exactly when the server sent a
    /// usable Retry-After header (a flush call never cuts it short),
    /// otherwise falls back to exponential backoff. Pass a value <= 0 to mean
    /// "no header".
    void on_rate_limited(double now_seconds, double retry_after_seconds);

    /// HTTP 400 on a batch of batch_size events. A batch larger than one
    /// event halves the batch limit and the caller should retry immediately.
    /// A single event the server rejects outright is dropped so it cannot
    /// block everything queued behind it, and the limit is restored.
    void on_bad_request(std::size_t batch_size);

    /// Same as on_bad_request(plan.batch.size()), except that the single
    /// rejected event is dropped only if it is still queued, found by
    /// sequence (it may have been cleared while the request was in flight).
    /// The client treats a 413 the same way.
    /// Returns true when an event was dropped.
    bool on_bad_request(const FlushPlan& plan);

    /// Any other failure, including a connection that never completed.
    void on_transport_failure(double now_seconds);

    // --- Backoff -----------------------------------------------------------

    /// Back to the 10 second first delay, with no pending retry window.
    void reset_backoff();

    /// Opens a retry window of the current backoff, multiplied by a factor in
    /// [backoff_jitter_floor, 1) when a random source is set, then doubles
    /// the backoff up to the ceiling.
    void schedule_backoff(double now_seconds);

    /// A source of draws in [0, 1) for the backoff jitter. Unset (the
    /// default for a bare Queue) means no spread, so the state machine stays
    /// deterministic; the Client sets the operating system's random source.
    void set_random_source(std::function<double()> draw) { random_ = std::move(draw); }

    /// 0 when a request may go out now, otherwise the wait remaining.
    double seconds_until_next_attempt(double now_seconds) const;

    /// The delay the next schedule_backoff would use, before any jitter.
    double backoff_seconds() const { return backoff_seconds_; }

    /// seconds_until_next_attempt raised to min_retry_seconds, for handing
    /// straight to a wait that must not spin on a zero delay.
    double retry_timer_seconds(double now_seconds) const;

    /// Batch size cap for the next flush: max_batch_size normally, smaller
    /// while splitting an oversize batch after a 400.
    int batch_limit() const { return batch_limit_; }

    /// Seconds a server's Retry-After still holds, 0 when none.
    double retry_after_remaining(double now_seconds) const;

    // --- Stats -------------------------------------------------------------

    QueueStats stats() const;

    /// Grows by one on every change to the queued events, so an owner that
    /// persists them knows when to write.
    unsigned long long revision() const { return revision_; }

    /// Counts events dropped outside the queue (for example too old to load).
    void count_dropped(int count) { dropped_count_ += count; }

private:
    /// Removes the plan's events still queued, found by sequence.
    int remove_sent(const FlushPlan& plan);

    /// Removes oldest events until the queue fits the cap minus headroom.
    /// Returns how many went.
    int trim_to_cap(int headroom);

    std::deque<Event> events_;
    unsigned long long next_sequence_ = 1;
    int max_queue_size_ = limits::default_queue_cap;

    bool server_tracking_enabled_ = true;
    bool locally_enabled_ = true;
    bool flush_in_progress_ = false;

    std::string session_token_;
    double session_expires_at_seconds_ = 0.0;

    double next_attempt_at_seconds_ = 0.0;
    double retry_after_until_seconds_ = 0.0;
    unsigned long long revision_ = 0;
    double backoff_seconds_ = limits::default_retry_seconds;
    std::function<double()> random_;
    int batch_limit_ = limits::max_batch_size;

    int sent_count_ = 0;
    int dropped_count_ = 0;
    int batch_count_ = 0;
    int session_count_ = 0;
};

} // namespace ravensight
