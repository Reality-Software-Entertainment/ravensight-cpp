// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Unit tests for the protocol state machine. Every clock reading is passed
// in explicitly, so each retry path is exercised deterministically.

#include <cstdlib>
#include <string>

#include "ravensight/queue.hpp"
#include "test_runner.hpp"

using ravensight::Event;
using ravensight::FlushDecision;
using ravensight::FlushPlan;
using ravensight::Queue;
namespace limits = ravensight::limits;

namespace {

Event make_event(int index) {
    Event event;
    event.name = "event_" + std::to_string(index);
    event.timestamp = 1000 + index;
    return event;
}

void fill(Queue& queue, int count) {
    for (int i = 0; i < count; ++i) {
        queue.enqueue(make_event(i));
    }
}

Queue queue_with_session(int events, double now = 100.0) {
    Queue queue;
    queue.set_session("st_token", now + 3600.0);
    fill(queue, events);
    return queue;
}

} // namespace

RS_TEST(queue_defaults) {
    Queue queue;
    CHECK_EQ(queue.max_queue_size(), limits::default_queue_cap);
    CHECK_EQ(queue.count(), 0);
    CHECK(queue.is_active());
    CHECK(!queue.is_session_valid(0.0));
    CHECK_EQ(queue.batch_limit(), limits::max_batch_size);
    CHECK_EQ(queue.backoff_seconds(), limits::default_retry_seconds);
}

RS_TEST(queue_cap_drops_oldest_first) {
    Queue queue;
    queue.set_max_queue_size(3);
    fill(queue, 3);
    const int dropped = queue.enqueue(make_event(3));
    CHECK_EQ(dropped, 1);
    CHECK_EQ(queue.count(), 3);
    // The oldest event went; the newest survived.
    CHECK_EQ(queue.events().front().name, std::string("event_1"));
    CHECK_EQ(queue.events().back().name, std::string("event_3"));
    CHECK_EQ(queue.stats().dropped, 1);
}

RS_TEST(queue_shrinking_cap_trims_immediately) {
    Queue queue;
    fill(queue, 10);
    queue.set_max_queue_size(4);
    CHECK_EQ(queue.count(), 4);
    CHECK_EQ(queue.events().front().name, std::string("event_6"));
    queue.set_max_queue_size(0); // clamped to 1
    CHECK_EQ(queue.max_queue_size(), 1);
    CHECK_EQ(queue.count(), 1);
}

RS_TEST(kill_switch_discards_queue_and_blocks_enqueue) {
    Queue queue;
    fill(queue, 5);
    queue.set_server_tracking_enabled(false);
    CHECK_EQ(queue.count(), 0);
    CHECK_EQ(queue.stats().dropped, 5);
    CHECK_EQ(queue.enqueue(make_event(9)), 0);
    CHECK_EQ(queue.count(), 0);
    const FlushPlan plan = queue.begin_flush(0.0, true);
    CHECK(plan.decision == FlushDecision::disabled);
}

RS_TEST(local_opt_out_discards_queue) {
    Queue queue;
    fill(queue, 3);
    queue.set_locally_enabled(false);
    CHECK_EQ(queue.count(), 0);
    CHECK(!queue.is_active());
    queue.set_locally_enabled(true);
    CHECK(queue.is_active());
    CHECK_EQ(queue.enqueue(make_event(1)), 0);
    CHECK_EQ(queue.count(), 1);
}

RS_TEST(session_validity_honors_expiry_skew) {
    Queue queue;
    queue.set_session("st_token", 1000.0);
    CHECK(queue.is_session_valid(994.0));
    // Inside the 5 second skew window the token counts as expired already.
    CHECK(!queue.is_session_valid(995.0));
    CHECK(!queue.is_session_valid(1001.0));
    queue.invalidate_session();
    CHECK(!queue.is_session_valid(0.0));
    CHECK_EQ(queue.session_token(), std::string(""));
}

RS_TEST(begin_flush_decisions) {
    Queue queue;
    CHECK(queue.begin_flush(0.0, false).decision == FlushDecision::empty);

    fill(queue, 2);
    CHECK(queue.begin_flush(0.0, false).decision == FlushDecision::needs_session);

    queue.set_session("st_token", 3600.0);
    FlushPlan plan = queue.begin_flush(0.0, false);
    CHECK(plan.should_send());
    CHECK_EQ(plan.batch.size(), std::size_t(2));
    CHECK_EQ(plan.batch[0].name, std::string("event_0"));

    // While a flush is in flight another begin_flush must not start one.
    CHECK(queue.begin_flush(0.0, false).decision == FlushDecision::already_flushing);
    queue.end_flush();
    CHECK(queue.begin_flush(0.0, false).should_send());
}

RS_TEST(begin_flush_caps_batches_at_fifty) {
    Queue queue = queue_with_session(120);
    const FlushPlan plan = queue.begin_flush(100.0, false);
    REQUIRE(plan.should_send());
    CHECK_EQ(plan.batch.size(), std::size_t(limits::max_batch_size));
    // Accepting removes exactly the batch, leaving the rest queued.
    queue.on_batch_accepted(plan.batch.size());
    queue.end_flush();
    CHECK_EQ(queue.count(), 70);
    CHECK_EQ(queue.events().front().name, std::string("event_50"));
}

RS_TEST(backoff_blocks_flush_until_window_passes) {
    Queue queue = queue_with_session(1);
    queue.on_transport_failure(100.0);
    CHECK(queue.begin_flush(105.0, false).decision == FlushDecision::backing_off);
    CHECK_EQ(queue.seconds_until_next_attempt(105.0), 5.0);
    // Once the window passes the flush proceeds.
    CHECK(queue.begin_flush(110.0, false).should_send());
}

RS_TEST(force_ignores_backoff_but_not_session_or_kill_switch) {
    Queue queue = queue_with_session(1);
    queue.on_transport_failure(100.0);
    CHECK(queue.begin_flush(101.0, true).should_send());
    queue.end_flush();

    queue.invalidate_session();
    CHECK(queue.begin_flush(101.0, true).decision == FlushDecision::needs_session);

    queue.set_server_tracking_enabled(false);
    CHECK(queue.begin_flush(101.0, true).decision == FlushDecision::disabled);
}

RS_TEST(backoff_doubles_to_the_ceiling) {
    Queue queue;
    double now = 0.0;
    const double expected[] = {10.0, 20.0, 40.0, 80.0, 160.0, 300.0, 300.0};
    for (double delay : expected) {
        queue.schedule_backoff(now);
        CHECK_EQ(queue.seconds_until_next_attempt(now), delay);
        now += delay;
    }
    queue.reset_backoff();
    CHECK_EQ(queue.backoff_seconds(), limits::default_retry_seconds);
    CHECK_EQ(queue.seconds_until_next_attempt(now), 0.0);
}

RS_TEST(success_resets_backoff) {
    Queue queue = queue_with_session(10);
    queue.on_transport_failure(100.0);
    queue.on_transport_failure(110.0);
    CHECK_EQ(queue.backoff_seconds(), 40.0);
    queue.on_batch_accepted(5);
    CHECK_EQ(queue.backoff_seconds(), limits::default_retry_seconds);
    CHECK_EQ(queue.seconds_until_next_attempt(110.0), 0.0);
    CHECK_EQ(queue.count(), 5);
}

RS_TEST(rate_limit_honors_retry_after) {
    Queue queue = queue_with_session(1);
    queue.on_rate_limited(100.0, 30.0);
    CHECK_EQ(queue.seconds_until_next_attempt(100.0), 30.0);
    // Retry-After does not consume the exponential ladder.
    CHECK_EQ(queue.backoff_seconds(), limits::default_retry_seconds);
}

RS_TEST(rate_limit_without_header_uses_exponential_backoff) {
    Queue queue = queue_with_session(1);
    queue.on_rate_limited(100.0, 0.0);
    CHECK_EQ(queue.seconds_until_next_attempt(100.0), 10.0);
    queue.on_rate_limited(110.0, -1.0);
    CHECK_EQ(queue.seconds_until_next_attempt(110.0), 20.0);
}

RS_TEST(unauthorized_keeps_events_and_drops_session) {
    Queue queue = queue_with_session(7);
    const FlushPlan plan = queue.begin_flush(100.0, false);
    REQUIRE(plan.should_send());
    queue.on_unauthorized();
    queue.end_flush();
    // Nothing was lost; the same batch goes out after re-authentication.
    CHECK_EQ(queue.count(), 7);
    CHECK(!queue.is_session_valid(100.0));
    queue.set_session("st_new", 100.0 + 3600.0);
    const FlushPlan retry = queue.begin_flush(100.0, false);
    REQUIRE(retry.should_send());
    CHECK_EQ(retry.batch.size(), std::size_t(7));
    CHECK_EQ(retry.batch[0].name, std::string("event_0"));
}

RS_TEST(oversize_batch_splits_down_to_one_then_drops) {
    Queue queue = queue_with_session(50);

    FlushPlan plan = queue.begin_flush(100.0, false);
    REQUIRE(plan.should_send());
    CHECK_EQ(plan.batch.size(), std::size_t(50));
    queue.on_bad_request(plan.batch.size());
    queue.end_flush();

    const std::size_t expected_sizes[] = {25, 12, 6, 3, 1};
    for (std::size_t expected : expected_sizes) {
        plan = queue.begin_flush(100.0, false);
        REQUIRE(plan.should_send());
        CHECK_EQ(plan.batch.size(), expected);
        queue.on_bad_request(plan.batch.size());
        queue.end_flush();
    }

    // The single poisoned event was dropped and the limit restored, so the
    // rest of the queue is not blocked behind it.
    CHECK_EQ(queue.count(), 49);
    CHECK_EQ(queue.events().front().name, std::string("event_1"));
    CHECK_EQ(queue.batch_limit(), limits::max_batch_size);
    CHECK_EQ(queue.stats().dropped, 1);
}

RS_TEST(accepted_batch_restores_split_limit) {
    Queue queue = queue_with_session(40);
    FlushPlan plan = queue.begin_flush(100.0, false);
    queue.on_bad_request(plan.batch.size()); // limit now 20
    queue.end_flush();
    plan = queue.begin_flush(100.0, false);
    CHECK_EQ(plan.batch.size(), std::size_t(20));
    queue.on_batch_accepted(plan.batch.size());
    queue.end_flush();
    // Back to full batches after a success.
    plan = queue.begin_flush(100.0, false);
    CHECK_EQ(plan.batch.size(), std::size_t(20));
    CHECK_EQ(queue.batch_limit(), limits::max_batch_size);
}

RS_TEST(retry_timer_has_a_floor) {
    Queue queue;
    CHECK_EQ(queue.retry_timer_seconds(100.0), limits::min_retry_seconds);
    queue.schedule_backoff(100.0);
    CHECK_EQ(queue.retry_timer_seconds(100.0), 10.0);
    CHECK_EQ(queue.retry_timer_seconds(109.9), limits::min_retry_seconds);
}

RS_TEST(stats_track_the_whole_lifecycle) {
    Queue queue;
    queue.set_max_queue_size(5);
    fill(queue, 7); // 2 dropped by the cap
    queue.set_session("st", 3600.0);
    const FlushPlan plan = queue.begin_flush(0.0, false);
    queue.on_batch_accepted(plan.batch.size());
    queue.end_flush();

    const ravensight::QueueStats stats = queue.stats();
    CHECK_EQ(stats.queued, 0);
    CHECK_EQ(stats.sent, 5);
    CHECK_EQ(stats.dropped, 2);
    CHECK_EQ(stats.batches, 1);
    CHECK_EQ(stats.sessions, 1);
}

RS_TEST(accepted_batch_after_clear_keeps_new_events) {
    Queue queue = queue_with_session(3);
    FlushPlan plan = queue.begin_flush(100.0, false);
    REQUIRE(plan.should_send());
    CHECK_EQ(plan.batch.size(), std::size_t(3));

    // The player opts out and back in while the batch is in flight.
    queue.set_locally_enabled(false);
    queue.set_locally_enabled(true);
    queue.enqueue(make_event(10));
    queue.enqueue(make_event(11));

    // The late 202 must not erase the two new events.
    CHECK_EQ(queue.on_batch_accepted(plan), 0);
    queue.end_flush();
    CHECK_EQ(queue.count(), 2);
    CHECK_EQ(queue.events().front().name, std::string("event_10"));
    CHECK_EQ(queue.stats().sent, 0);
}

RS_TEST(accepted_batch_removes_exactly_its_events) {
    Queue queue = queue_with_session(60);
    FlushPlan plan = queue.begin_flush(100.0, false);
    REQUIRE(plan.should_send());
    queue.enqueue(make_event(99));
    CHECK_EQ(queue.on_batch_accepted(plan), 50);
    queue.end_flush();
    CHECK_EQ(queue.count(), 11);
    CHECK_EQ(queue.stats().sent, 50);
}

RS_TEST(sequences_strictly_increase) {
    Queue queue;
    fill(queue, 5);
    unsigned long long last = 0;
    for (const Event& event : queue.events()) {
        CHECK(event.sequence > last);
        last = event.sequence;
    }
}

RS_TEST(bad_request_single_event_after_clear_drops_nothing_new) {
    Queue queue = queue_with_session(1);
    FlushPlan plan = queue.begin_flush(100.0, false);
    REQUIRE(plan.should_send());
    queue.clear();
    queue.enqueue(make_event(7));
    CHECK(!queue.on_bad_request(plan));
    queue.end_flush();
    CHECK_EQ(queue.count(), 1);
    CHECK_EQ(queue.events().front().name, std::string("event_7"));
}

RS_TEST(bad_request_single_event_is_dropped_and_counted) {
    Queue queue = queue_with_session(2);
    // Force the limit down to one event.
    FlushPlan plan = queue.begin_flush(100.0, false);
    CHECK(!queue.on_bad_request(plan));
    queue.end_flush();
    CHECK_EQ(queue.batch_limit(), 1);
    plan = queue.begin_flush(100.0, false);
    REQUIRE(plan.batch.size() == std::size_t(1));
    CHECK(queue.on_bad_request(plan));
    queue.end_flush();
    CHECK_EQ(queue.count(), 1);
    CHECK_EQ(queue.stats().dropped, 1);
    CHECK_EQ(queue.events().front().name, std::string("event_1"));
}

// --- Backoff jitter (0.3.0) -------------------------------------------------------

RS_TEST(backoff_jitter_bounds) {
    Queue low;
    low.set_random_source([] { return 0.0; });
    low.schedule_backoff(100.0);
    CHECK_EQ(low.seconds_until_next_attempt(100.0), 8.0);
    CHECK_EQ(low.backoff_seconds(), 20.0); // the doubling is not spread

    Queue high;
    high.set_random_source([] { return 1.0; });
    high.schedule_backoff(100.0);
    CHECK_EQ(high.seconds_until_next_attempt(100.0), 12.0);

    Queue mid;
    mid.set_random_source([] { return 0.5; });
    mid.schedule_backoff(100.0);
    CHECK_EQ(mid.seconds_until_next_attempt(100.0), 10.0); // a fixed draw keeps 0.2.0's wait

    // Never past the ceiling.
    Queue capped;
    capped.set_random_source([] { return 1.0; });
    for (int i = 0; i < 10; ++i) {
        capped.schedule_backoff(100.0);
    }
    CHECK(capped.seconds_until_next_attempt(100.0) <= limits::max_backoff_seconds);
    CHECK_EQ(capped.seconds_until_next_attempt(100.0), limits::max_backoff_seconds);

    // A real source stays inside plus or minus 20 percent.
    Queue spread;
    spread.set_random_source([] { return static_cast<double>(std::rand()) / (RAND_MAX + 1.0); });
    for (int i = 0; i < 200; ++i) {
        spread.reset_backoff();
        spread.schedule_backoff(0.0);
        const double wait = spread.seconds_until_next_attempt(0.0);
        CHECK(wait >= 8.0 && wait <= 12.0);
    }
}

RS_TEST(retry_after_is_not_jittered) {
    Queue queue;
    queue.set_random_source([] { return 0.0; });
    queue.on_rate_limited(100.0, 30.0);
    CHECK_EQ(queue.seconds_until_next_attempt(100.0), 30.0);
}
