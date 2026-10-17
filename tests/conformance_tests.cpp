// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Conformance tests for the shared SDK contract (0.2.0, with the 0.3.0
// refused key rule: the run ends): nothing on the
// network while disabled, the saved opt-out, reset, refused keys, the kill
// switch, Retry-After, feedback validation, the playtest path, the session
// hand-off and the lifecycle once-guards. Driven through a fake transport;
// no network, no libcurl.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#include "ravensight/client.hpp"
#include "storage.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;
namespace fs = std::filesystem;

namespace {

double wall_seconds() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void sleep_seconds(double seconds) {
    std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string header_value(const Request& request, const std::string& name) {
    for (const auto& header : request.headers) {
        if (header.first == name) {
            return header.second;
        }
    }
    return "";
}

bool has_header(const Request& request, const std::string& name) {
    for (const auto& header : request.headers) {
        if (header.first == name) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> batch_event_names(const Request& request) {
    std::vector<std::string> names;
    const json::Value parsed = json::Value::parse(request.body);
    const json::Value* events = parsed.find("events");
    if (events != nullptr) {
        for (std::size_t i = 0; i < events->size(); ++i) {
            names.push_back(events->at(i).get_string("event", ""));
        }
    }
    return names;
}

Response reply(long status, const std::string& body, const std::string& retry_after = "") {
    Response response;
    response.status = status;
    response.body = body;
    response.retry_after = retry_after;
    return response;
}

Response default_routes(const Request& request) {
    if (ends_with(request.url, "/settings")) {
        return reply(200, "{\"trackingEnabled\":true}");
    }
    if (ends_with(request.url, "/session")) {
        return reply(201, "{\"token\":\"st_test\",\"expiresIn\":86400}");
    }
    if (ends_with(request.url, "/track/batch")) {
        return reply(202, "{\"accepted\":1}");
    }
    if (ends_with(request.url, "/feedback")) {
        return reply(201, "{\"id\":\"f1\"}");
    }
    if (ends_with(request.url, "/agent/suggestions")) {
        return reply(200, "{\"suggestions\":[{\"title\":\"a\"}]}");
    }
    return reply(404, "{\"error\":\"not_found\"}");
}

/// Records every request with the time it was sent, then answers it.
class Fake : public Transport {
public:
    using Responder = std::function<Response(const Request&)>;

    explicit Fake(Responder responder = &default_routes) : responder_(std::move(responder)) {}

    Response send(const Request& request) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
            times_.push_back(wall_seconds());
        }
        cv_.notify_all();
        return responder_(request);
    }

    bool wait_for(std::size_t count, double timeout_seconds = 3.0) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::duration<double>(timeout_seconds),
                            [&] { return requests_.size() >= count; });
    }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    std::vector<double> times() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return times_;
    }

    std::size_t count(const std::string& suffix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t n = 0;
        for (const Request& request : requests_) {
            if (ends_with(request.url, suffix)) {
                ++n;
            }
        }
        return n;
    }

    std::size_t total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    std::vector<std::string> sent_event_names() const {
        std::vector<std::string> names;
        for (const Request& request : requests()) {
            if (ends_with(request.url, "/track/batch")) {
                for (const std::string& name : batch_event_names(request)) {
                    names.push_back(name);
                }
            }
        }
        return names;
    }

private:
    Responder responder_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Request> requests_;
    std::vector<double> times_;
};

class Proxy : public Transport {
public:
    explicit Proxy(std::shared_ptr<Fake> inner) : inner_(std::move(inner)) {}
    Response send(const Request& request) override { return inner_->send(request); }

private:
    std::shared_ptr<Fake> inner_;
};

struct LogLines {
    std::mutex mutex;
    std::vector<std::string> lines;

    std::size_t count_containing(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        std::size_t n = 0;
        for (const std::string& line : lines) {
            if (line.find(text) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }
};

std::string temp_dir() {
    return rstest::make_temp_dir("ravensight-conf-");
}

Config base_config(const std::shared_ptr<LogLines>& logs = nullptr) {
    Config config;
    config.ingest_key = "gt_live_test_key";
    config.api_url = "https://api.example.test";
    config.game_version = "2.0.0";
    config.platform = "test";
    config.storage_dir = temp_dir();
    config.flush_interval_seconds = 0.05;
    config.track_lifecycle_events = false;
    if (logs) {
        config.log = [logs](const std::string& line) {
            std::lock_guard<std::mutex> lock(logs->mutex);
            logs->lines.push_back(line);
        };
    } else {
        config.log = [](const std::string&) {};
    }
    return config;
}

std::unique_ptr<Transport> proxy(const std::shared_ptr<Fake>& fake) {
    return std::make_unique<Proxy>(fake);
}

/// Waits for a callback style result.
template <typename T>
struct Waiter {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    T value{};

    void set(const T& v) {
        std::lock_guard<std::mutex> lock(mutex);
        value = v;
        done = true;
        cv.notify_all();
    }
    bool wait(double seconds = 3.0) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::duration<double>(seconds), [&] { return done; });
    }
};

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
}

} // namespace

// --- Nothing on the network while disabled ----------------------------------

RS_TEST(disabled_start_sends_nothing_then_settings_first_after_enable) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.enabled = false;
    config.track_lifecycle_events = true;
    Client client(config, proxy(fake));

    CHECK(!client.track("ignored"));
    client.flush();
    sleep_seconds(0.2);
    CHECK_EQ(fake->total(), std::size_t(0));
    CHECK(client.status() == Status::disabled);
    // No device id file is written while disabled.
    CHECK(!fs::exists(fs::path(config.storage_dir) / "device_id"));

    client.set_enabled(true);
    REQUIRE(fake->wait_for(1));
    CHECK(ends_with(fake->requests()[0].url, "/api/v1/settings"));
    CHECK(client.flush_and_wait(3.0));
    // game_started was queued once, when the client was enabled.
    const std::vector<std::string> names = fake->sent_event_names();
    REQUIRE(names.size() == std::size_t(1));
    CHECK_EQ(names[0], std::string("game_started"));
    CHECK(fs::exists(fs::path(config.storage_dir) / "device_id"));
}

RS_TEST(disabled_client_refuses_suggestions_and_feedback_without_requests) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.enabled = false;
    Client client(config, proxy(fake));

    bool called = false;
    bool ok = true;
    std::size_t size = 99;
    client.fetch_suggestions([&](bool request_ok, const json::Value& suggestions) {
        called = true;
        ok = request_ok;
        size = suggestions.size();
    });
    CHECK(called); // synchronous, on the calling thread
    CHECK(!ok);
    CHECK_EQ(size, std::size_t(0));

    FeedbackResult result;
    result.ok = true;
    client.submit_feedback("Too hard", "complaint", 2, [&](const FeedbackResult& r) { result = r; });
    CHECK(!result.ok);
    CHECK_EQ(result.code, std::string("disabled"));

    sleep_seconds(0.15);
    CHECK_EQ(fake->total(), std::size_t(0));
}

RS_TEST(suggestions_are_refused_after_runtime_opt_out) {
    auto fake = std::make_shared<Fake>();
    Client client(base_config(), proxy(fake));
    REQUIRE(fake->wait_for(1)); // settings
    client.set_enabled(false);
    Waiter<bool> waiter;
    client.fetch_suggestions([&](bool ok, const json::Value&) { waiter.set(ok); });
    REQUIRE(waiter.wait());
    CHECK(!waiter.value);
    sleep_seconds(0.1);
    CHECK_EQ(fake->count("/agent/suggestions"), std::size_t(0));
}

// --- Saved opt-out -------------------------------------------------------------

RS_TEST(saved_opt_out_makes_zero_requests_across_a_new_client) {
    Config config = base_config();
    config.track_lifecycle_events = true;
    {
        auto fake = std::make_shared<Fake>();
        Client first(config, proxy(fake));
        REQUIRE(fake->wait_for(1));
        first.set_tracking_enabled(false);
        CHECK(!first.is_tracking_enabled_by_player());
        CHECK(!first.track("after_opt_out"));
    }
    auto fake = std::make_shared<Fake>();
    Client second(config, proxy(fake)); // enabled = true, but the saved opt-out wins
    CHECK(!second.track("ignored"));
    second.flush();
    sleep_seconds(0.2);
    second.shutdown();
    CHECK_EQ(fake->total(), std::size_t(0));
    CHECK(second.status() == Status::disabled);
    CHECK(!second.is_tracking_enabled_by_player());
    CHECK(second.is_enabled());
}

RS_TEST(opt_out_before_the_client_exists) {
    Config config = base_config();
    config.track_lifecycle_events = true;
    CHECK(set_tracking_enabled(config, false));
    auto fake = std::make_shared<Fake>();
    {
        Client client(config, proxy(fake));
        sleep_seconds(0.2);
    }
    CHECK_EQ(fake->total(), std::size_t(0));
    // Nothing identifying was written either.
    CHECK(!fs::exists(fs::path(config.storage_dir) / "device_id"));

    // Opting back in before the next launch.
    CHECK(set_tracking_enabled(config, true));
    Client again(config, proxy(fake));
    REQUIRE(fake->wait_for(1));
    CHECK(again.flush_and_wait(3.0));
    CHECK(fake->count("/track/batch") >= std::size_t(1));
}

RS_TEST(opt_out_is_saved_even_when_constructed_disabled) {
    Config config = base_config();
    config.enabled = false;
    auto fake = std::make_shared<Fake>();
    {
        Client client(config, proxy(fake));
        client.set_tracking_enabled(false);
    }
    config.enabled = true;
    {
        Client client(config, proxy(fake));
        sleep_seconds(0.15);
        CHECK(client.status() == Status::disabled);
    }
    CHECK_EQ(fake->total(), std::size_t(0));
}

RS_TEST(opt_in_after_opt_out_resumes_with_one_game_started) {
    Config config = base_config();
    config.track_lifecycle_events = true;
    auto fake = std::make_shared<Fake>();
    {
        Client client(config, proxy(fake));
        CHECK(client.flush_and_wait(3.0));
        client.set_tracking_enabled(false);
        client.set_tracking_enabled(true);
        client.set_enabled(false);
        client.set_enabled(true);
        CHECK(client.track("level_start"));
        CHECK(client.flush_and_wait(3.0));
    }
    std::size_t started = 0;
    std::size_t exited = 0;
    for (const std::string& name : fake->sent_event_names()) {
        started += name == "game_started" ? 1 : 0;
        exited += name == "game_exited" ? 1 : 0;
    }
    CHECK_EQ(started, std::size_t(1));
    CHECK_EQ(exited, std::size_t(1));
    CHECK_EQ(fake->count("/settings"), std::size_t(1));
}

// --- Device id -----------------------------------------------------------------

RS_TEST(device_id_is_persisted_by_default) {
    Config config = base_config();
    auto fake = std::make_shared<Fake>();
    std::string first_id;
    {
        Client client(config, proxy(fake));
        first_id = client.device_id();
    }
    CHECK_EQ(first_id.size(), std::size_t(36));
    CHECK(first_id.compare(0, 4, "dev_") == 0);
    std::string saved;
    CHECK(detail::read_text_file((fs::path(config.storage_dir) / "device_id").string(), saved));
    CHECK_EQ(saved, first_id);
    Client again(config, proxy(fake));
    CHECK_EQ(again.device_id(), first_id);
}

RS_TEST(reset_device_id_mints_saves_and_drops_queue_and_session) {
    Config config = base_config();
    auto fake = std::make_shared<Fake>();
    Client client(config, proxy(fake));
    CHECK(client.track("before"));
    CHECK(client.flush_and_wait(3.0));
    const std::string old_id = client.device_id();
    CHECK(client.is_ready());

    client.set_enabled(false); // hold events so the queue is observable
    client.set_enabled(true);
    CHECK(client.track("pending"));
    const std::string fresh = client.reset_device_id();
    CHECK(fresh != old_id);
    CHECK_EQ(client.device_id(), fresh);
    CHECK_EQ(client.stats().queued, 0);
    CHECK(!client.is_ready());
    std::string saved;
    CHECK(detail::read_text_file((fs::path(config.storage_dir) / "device_id").string(), saved));
    CHECK_EQ(saved, fresh);

    CHECK(client.track("after"));
    CHECK(client.flush_and_wait(3.0));
    const std::vector<Request> requests = fake->requests();
    const Request& last_session = [&]() -> const Request& {
        for (auto it = requests.rbegin(); it != requests.rend(); ++it) {
            if (ends_with(it->url, "/session")) return *it;
        }
        return requests.front();
    }();
    CHECK_EQ(json::Value::parse(last_session.body).get_string("deviceId", ""), fresh);
    for (const std::string& name : fake->sent_event_names()) {
        CHECK(name != "pending");
    }
}

RS_TEST(reset_while_opted_out_does_not_bring_the_old_id_back) {
    Config config = base_config();
    std::string old_id;
    {
        auto fake = std::make_shared<Fake>();
        Client client(config, proxy(fake));
        CHECK(client.track("x"));
        CHECK(client.flush_and_wait(3.0));
        old_id = client.device_id();
        client.set_tracking_enabled(false);
        const std::string fresh = client.reset_device_id();
        CHECK(fresh != old_id);
        // Nothing is written while opted out, and the old id is gone.
        CHECK(!fs::exists(fs::path(config.storage_dir) / "device_id"));
    }
    auto fake = std::make_shared<Fake>();
    Client next(config, proxy(fake));
    CHECK(next.device_id() != old_id);
    next.set_tracking_enabled(true);
    CHECK(next.track("y"));
    CHECK(next.flush_and_wait(3.0));
    std::size_t sessions = 0;
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/session")) {
            ++sessions;
            const std::string sent = json::Value::parse(request.body).get_string("deviceId", "");
            CHECK(sent != old_id);
            CHECK_EQ(sent, next.device_id());
        }
    }
    CHECK_EQ(sessions, std::size_t(1));
}

RS_TEST(session_opened_for_the_old_id_after_reset_is_discarded) {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool first_started = false;
    bool release = false;
    std::atomic<int> sessions{0};
    auto fake = std::make_shared<Fake>([&](const Request& request) {
        if (ends_with(request.url, "/session")) {
            const int n = sessions.fetch_add(1) + 1;
            if (n == 1) {
                std::unique_lock<std::mutex> lock(gate_mutex);
                first_started = true;
                gate_cv.notify_all();
                gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return release; });
            }
            return reply(201, "{\"token\":\"st_" + std::to_string(n) + "\",\"expiresIn\":86400}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.flush_interval_seconds = 3600.0;
    Client client(config, proxy(fake));
    client.track("before_reset");
    client.flush();
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return first_started; }));
    }
    const std::string fresh = client.reset_device_id();
    client.track("after_reset");
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
        gate_cv.notify_all();
    }
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(client.session_token(), std::string("st_2"));

    std::vector<Request> session_requests;
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/session")) session_requests.push_back(request);
        if (ends_with(request.url, "/track/batch")) {
            CHECK_EQ(header_value(request, "X-Session-Token"), std::string("st_2"));
            for (const std::string& name : batch_event_names(request)) {
                CHECK(name != "before_reset");
            }
        }
    }
    REQUIRE(session_requests.size() == std::size_t(2));
    CHECK_EQ(json::Value::parse(session_requests[1].body).get_string("deviceId", ""), fresh);
    CHECK_EQ(fake->sent_event_names().size(), std::size_t(1));
}

RS_TEST(suggestions_are_not_requested_after_a_refused_key) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/settings")) {
            return reply(401, "{\"error\":\"invalid_key\"}");
        }
        return default_routes(request);
    });
    Client client(base_config(), proxy(fake));
    REQUIRE(fake->wait_for(1));
    sleep_seconds(0.1);
    REQUIRE(client.status() == Status::invalid_key);
    bool called = false;
    bool ok = true;
    client.fetch_suggestions([&](bool request_ok, const json::Value&) {
        called = true;
        ok = request_ok;
    });
    CHECK(called);
    CHECK(!ok);
    sleep_seconds(0.1);
    CHECK_EQ(fake->count("/agent/suggestions"), std::size_t(0));
}

// --- Refused key and kill switch -------------------------------------------------

RS_TEST(settings_401_makes_one_request_then_none) {
    auto logs = std::make_shared<LogLines>();
    std::vector<Status> statuses;
    std::mutex status_mutex;
    // A key the server refuses everywhere.
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/settings") || ends_with(request.url, "/session")) {
            return reply(401, "{\"error\":\"invalid_key\"}");
        }
        return default_routes(request);
    });
    Config config = base_config(logs);
    config.on_status = [&](Status status) {
        std::lock_guard<std::mutex> lock(status_mutex);
        statuses.push_back(status);
    };
    Client client(config, proxy(fake));
    REQUIRE(fake->wait_for(1));
    sleep_seconds(0.1);
    CHECK(!client.track("dropped")); // the run has ended: nothing queues
    sleep_seconds(0.3);              // six timer ticks
    CHECK_EQ(fake->total(), std::size_t(1));
    CHECK(client.status() == Status::invalid_key);
    CHECK(!client.tracking_enabled());
    CHECK(!client.is_collecting());
    CHECK_EQ(client.stats().queued, 0);
    CHECK_EQ(logs->count_containing(
                 "Ravensight: ingest key refused (invalid_key), sending stopped for this run. "
                 "Check your gt_live_ key."),
             std::size_t(1));
    CHECK_EQ(logs->count_containing("assuming tracking enabled"), std::size_t(0));
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        REQUIRE(!statuses.empty());
        CHECK(statuses.back() == Status::invalid_key);
    }

    // An explicit flush makes no further attempt either.
    client.flush();
    sleep_seconds(0.2);
    CHECK_EQ(fake->count("/session"), std::size_t(0));
    client.shutdown();
    // Nor does the shutdown flush.
    CHECK_EQ(fake->total(), std::size_t(1));
    CHECK_EQ(logs->count_containing("ingest key refused"), std::size_t(1));
}

RS_TEST(explicit_flush_after_a_refused_key_sends_nothing) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/settings")) {
            return reply(401, "{\"error\":\"invalid_key\"}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.track_lifecycle_events = true;
    Client client(config, proxy(fake));
    REQUIRE(fake->wait_for(1));
    sleep_seconds(0.15);
    CHECK_EQ(client.stats().queued, 0); // game_started was cleared
    CHECK(!client.track("late"));
    CHECK(client.flush_and_wait(1.0));
    client.set_tracking_enabled(false);
    client.set_tracking_enabled(true); // saved, restarts nothing
    sleep_seconds(0.15);
    client.shutdown();
    CHECK_EQ(fake->total(), std::size_t(1));
    CHECK_EQ(client.stats().sent, 0);
    CHECK(client.status() == Status::invalid_key);
}

RS_TEST(session_401_stops_automatic_retries) {
    auto logs = std::make_shared<LogLines>();
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/session")) {
            return reply(401, "{\"error\":\"invalid_key\"}");
        }
        return default_routes(request);
    });
    Client client(base_config(logs), proxy(fake));
    CHECK(client.track("one"));
    REQUIRE(fake->wait_for(2));
    sleep_seconds(0.4);
    client.shutdown();
    CHECK_EQ(fake->count("/session"), std::size_t(1));
    CHECK(client.status() == Status::invalid_key);
    CHECK_EQ(logs->count_containing("ingest key refused (invalid_key)"), std::size_t(1));
}

RS_TEST(session_403_other_code_is_invalid_key) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/session")) {
            return reply(403, "{\"error\":\"game_inactive\"}");
        }
        return default_routes(request);
    });
    Client client(base_config(), proxy(fake));
    CHECK(client.track("one"));
    REQUIRE(fake->wait_for(2));
    sleep_seconds(0.3);
    CHECK_EQ(fake->count("/session"), std::size_t(1));
    CHECK(client.status() == Status::invalid_key);
}

RS_TEST(session_403_tracking_disabled_stops_for_the_run) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/session")) {
            return reply(403, "{\"error\":\"tracking_disabled\"}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.track_lifecycle_events = true;
    Client client(config, proxy(fake));
    REQUIRE(fake->wait_for(2));
    sleep_seconds(0.3);
    CHECK(!client.tracking_enabled());
    CHECK(client.status() == Status::tracking_disabled);
    CHECK(!client.track("refused"));
    CHECK_EQ(client.stats().queued, 0);
    client.flush();
    sleep_seconds(0.1);
    client.shutdown();
    // Settings, one session attempt, and nothing after: no game_exited batch.
    CHECK_EQ(fake->total(), std::size_t(2));
}

RS_TEST(settings_kill_switch_stops_and_clears_game_started) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/settings")) {
            return reply(200, "{\"trackingEnabled\":false}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.track_lifecycle_events = true;
    {
        Client client(config, proxy(fake));
        REQUIRE(fake->wait_for(1));
        sleep_seconds(0.1);
        CHECK(client.status() == Status::tracking_disabled);
        CHECK_EQ(client.stats().queued, 0);
    }
    CHECK_EQ(fake->total(), std::size_t(1));
}

// --- Retry-After and 400 ------------------------------------------------------------

RS_TEST(session_429_waits_exactly_retry_after) {
    std::atomic<int> sessions{0};
    auto fake = std::make_shared<Fake>([&](const Request& request) {
        if (ends_with(request.url, "/session") && sessions.fetch_add(1) == 0) {
            return reply(429, "{\"error\":\"rate_limited\"}", "1");
        }
        return default_routes(request);
    });
    Client client(base_config(), proxy(fake));
    CHECK(client.track("one"));
    REQUIRE(fake->wait_for(2));
    sleep_seconds(0.2);
    CHECK(client.status() == Status::rate_limited);
    REQUIRE(fake->wait_for(4, 4.0)); // second session and the batch
    const std::vector<Request> requests = fake->requests();
    const std::vector<double> times = fake->times();
    std::vector<double> session_times;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (ends_with(requests[i].url, "/session")) session_times.push_back(times[i]);
    }
    REQUIRE(session_times.size() >= std::size_t(2));
    const double gap = session_times[1] - session_times[0];
    CHECK(gap >= 0.95);
    CHECK(gap < 2.0);
    sleep_seconds(0.1);
    CHECK(client.status() == Status::ok);
}

RS_TEST(batch_503_honors_retry_after) {
    std::atomic<int> batches{0};
    auto fake = std::make_shared<Fake>([&](const Request& request) {
        if (ends_with(request.url, "/track/batch") && batches.fetch_add(1) == 0) {
            return reply(503, "{\"error\":\"storage_unavailable\"}", "1");
        }
        return default_routes(request);
    });
    Client client(base_config(), proxy(fake));
    CHECK(client.track("one"));
    REQUIRE(fake->wait_for(4, 4.0));
    const std::vector<Request> requests = fake->requests();
    const std::vector<double> times = fake->times();
    std::vector<double> batch_times;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (ends_with(requests[i].url, "/track/batch")) batch_times.push_back(times[i]);
    }
    REQUIRE(batch_times.size() >= std::size_t(2));
    CHECK(batch_times[1] - batch_times[0] >= 0.95);
    CHECK_EQ(client.stats().sent, 1);
}

RS_TEST(single_event_400_is_dropped_and_the_queue_keeps_draining) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/track/batch")) {
            for (const std::string& name : batch_event_names(request)) {
                if (name == "poison") return reply(400, "{\"error\":\"invalid_event\"}");
            }
            return reply(202, "{}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.flush_interval_seconds = 3600.0;
    Client client(config, proxy(fake));
    client.track("a");
    client.track("poison");
    client.track("b");
    client.track("c");
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(client.stats().dropped, 1);
    CHECK_EQ(client.stats().sent, 3);
    CHECK_EQ(client.stats().queued, 0);
}

RS_TEST(fifty_queued_events_send_without_waiting_for_the_timer) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.flush_interval_seconds = 3600.0;
    Client client(config, proxy(fake));
    for (int i = 0; i < 50; ++i) {
        client.track("e", {{"i", i}});
    }
    REQUIRE(fake->wait_for(3)); // settings, session, batch
    sleep_seconds(0.1);
    REQUIRE(fake->count("/track/batch") == std::size_t(1));
    CHECK_EQ(fake->sent_event_names().size(), std::size_t(50));
}

RS_TEST(queue_cap_of_500_drops_oldest_while_offline) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/session")) {
            return reply(0, "connection refused");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.flush_interval_seconds = 3600.0;
    Client client(config, proxy(fake));
    for (int i = 0; i < 600; ++i) {
        client.track("e", {{"i", i}});
    }
    sleep_seconds(0.2);
    CHECK_EQ(client.stats().queued, 500);
    CHECK_EQ(client.stats().dropped, 100);
    CHECK(client.status() == Status::offline);
    // Offline backs off: only one session attempt despite repeated wakeups.
    CHECK_EQ(fake->count("/session"), std::size_t(1));
}

// --- Feedback ---------------------------------------------------------------------

RS_TEST(feedback_validation_refuses_locally_without_a_request) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.enabled = false; // proves no request could be the cause
    Client client(config, proxy(fake));
    client.set_enabled(true);
    REQUIRE(fake->wait_for(1)); // settings only
    const std::size_t before = fake->total();

    FeedbackResult result;
    client.submit_feedback("Too hard", "balance", 0, [&](const FeedbackResult& r) { result = r; });
    CHECK_EQ(result.code, std::string("invalid_category"));
    client.submit_feedback("Too hard", "bug", 6, [&](const FeedbackResult& r) { result = r; });
    CHECK_EQ(result.code, std::string("invalid_rating"));
    client.submit_feedback("Too hard", "bug", -1, [&](const FeedbackResult& r) { result = r; });
    CHECK_EQ(result.code, std::string("invalid_rating"));
    client.submit_feedback("  ", "bug", 3, [&](const FeedbackResult& r) { result = r; });
    CHECK_EQ(result.code, std::string("empty_message"));
    sleep_seconds(0.1);
    CHECK_EQ(fake->count("/feedback"), std::size_t(0));
    CHECK_EQ(fake->count("/session"), std::size_t(0));
    CHECK(fake->total() == before);

    CHECK_EQ(feedback_categories.size(), std::size_t(6));
    CHECK(feedback_categories[1] == "suggestion");
}

RS_TEST(feedback_rating_zero_is_omitted_and_categories_are_sent) {
    auto fake = std::make_shared<Fake>();
    Client client(base_config(), proxy(fake));
    for (std::string_view category : feedback_categories) {
        Waiter<FeedbackResult> waiter;
        client.submit_feedback("hello", std::string(category), 0,
                               [&](const FeedbackResult& r) { waiter.set(r); });
        REQUIRE(waiter.wait());
        CHECK(waiter.value.ok);
    }
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/feedback")) {
            CHECK(json::Value::parse(request.body).find("rating") == nullptr);
        }
    }
    CHECK_EQ(fake->count("/feedback"), std::size_t(6));
}

// --- Playtest -----------------------------------------------------------------------

RS_TEST(playtest_header_pt_id_tags_and_no_files_written) {
    auto fake = std::make_shared<Fake>();
    auto logs = std::make_shared<LogLines>();
    Config config = base_config(logs);
    config.playtest_token = "ptk_secret";
    config.playtest_run_id = "run42";
    config.playtest_job_id = "job7";
    config.playtest_persona = "speedrunner";
    config.track_lifecycle_events = true;
    {
        Client client(config, proxy(fake));
        CHECK(client.is_playtest());
        CHECK_EQ(client.device_id(), std::string("pt-run42"));
        client.track("level_start", {{"level", "castle"}});
        CHECK(client.flush_and_wait(3.0));

        // Opt-out and reset are no-ops that log, and do not stop the run.
        client.set_tracking_enabled(false);
        CHECK_EQ(client.reset_device_id(), std::string("pt-run42"));
        CHECK(client.track("still_running"));
        CHECK(client.flush_and_wait(3.0));
        CHECK(client.status() == Status::ok);
    }
    CHECK(logs->count_containing("set_tracking_enabled ignored during a playtest run") == 1);
    CHECK(logs->count_containing("reset_device_id ignored during a playtest run") == 1);

    for (const Request& request : fake->requests()) {
        CHECK_EQ(header_value(request, "X-Ravensight-Playtest"), std::string("ptk_secret"));
        if (ends_with(request.url, "/session")) {
            CHECK_EQ(json::Value::parse(request.body).get_string("deviceId", ""),
                     std::string("pt-run42"));
        }
        if (ends_with(request.url, "/track/batch")) {
            const json::Value body = json::Value::parse(request.body);
            const json::Value& first = body.find("events")->at(0);
            const json::Value* data = first.find("data");
            REQUIRE(data != nullptr);
            bool synthetic = false;
            CHECK(data->get_bool("synthetic", synthetic));
            CHECK(synthetic);
            CHECK_EQ(data->get_string("pt_run", ""), std::string("run42"));
            CHECK_EQ(data->get_string("pt_job", ""), std::string("job7"));
            CHECK_EQ(data->get_string("persona", ""), std::string("speedrunner"));
        }
    }
    const std::vector<std::string> names = fake->sent_event_names();
    REQUIRE(!names.empty());
    CHECK_EQ(names.front(), std::string("game_started"));
    CHECK_EQ(names.back(), std::string("game_exited"));
    // Nothing was written: not the id, not the opt-out.
    CHECK(!fs::exists(fs::path(config.storage_dir)));
}

RS_TEST(playtest_ignores_a_saved_opt_out) {
    Config config = base_config();
    CHECK(set_tracking_enabled(config, false));
    config.playtest_token = "ptk";
    config.playtest_run_id = "r1";
    auto fake = std::make_shared<Fake>();
    Client client(config, proxy(fake));
    CHECK(client.track("x"));
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(client.stats().sent, 1);
    // And the saved choice on disk is untouched.
    CHECK(!set_tracking_enabled(config, true)); // a playtest config: no-op
    std::string text;
    CHECK(detail::read_text_file((fs::path(config.storage_dir) / "state").string(), text));
    CHECK(text.find("tracking_enabled=false") != std::string::npos);
}

RS_TEST(playtest_from_environment_and_command_line) {
    set_env("RAVENSIGHT_PLAYTEST_TOKEN", "env_token");
    set_env("RAVENSIGHT_PLAYTEST_RUN_ID", "env_run");
    {
        auto fake = std::make_shared<Fake>();
        Config config = base_config();
        // The environment beats the command line.
        const char* argv[] = {"game", "--ravensight-playtest-token=arg_token",
                              "--ravensight-playtest-run-id=arg_run"};
        CHECK(!apply_playtest_args(config, 3, argv));
        Client client(config, proxy(fake));
        CHECK(client.is_playtest());
        CHECK_EQ(client.device_id(), std::string("pt-env_run"));
        REQUIRE(fake->wait_for(1));
        CHECK_EQ(header_value(fake->requests()[0], "X-Ravensight-Playtest"),
                 std::string("env_token"));
    }
    set_env("RAVENSIGHT_PLAYTEST_TOKEN", nullptr);
    set_env("RAVENSIGHT_PLAYTEST_RUN_ID", nullptr);

    Config config = base_config();
    const char* argv[] = {"game", "--ravensight-playtest-token=arg_token",
                          "--ravensight-playtest-run-id=arg_run"};
    CHECK(apply_playtest_args(config, 3, argv));
    CHECK_EQ(config.playtest_token, std::string("arg_token"));
    CHECK_EQ(config.playtest_run_id, std::string("arg_run"));

    // Outside a playtest, no header at all.
    auto fake = std::make_shared<Fake>();
    Client plain(base_config(), proxy(fake));
    CHECK(!plain.is_playtest());
    REQUIRE(fake->wait_for(1));
    CHECK(!has_header(fake->requests()[0], "X-Ravensight-Playtest"));
}

RS_TEST(playtest_token_without_run_id_never_runs_as_a_player) {
    auto fake = std::make_shared<Fake>();
    auto logs = std::make_shared<LogLines>();
    Config config = base_config(logs);
    config.playtest_token = "ptk";
    config.track_lifecycle_events = true;
    {
        Client client(config, proxy(fake));
        CHECK(!client.track("x"));
        sleep_seconds(0.15);
        CHECK(client.status() == Status::session_failed);
    }
    CHECK_EQ(fake->total(), std::size_t(0));
    CHECK_EQ(logs->count_containing("without a run id"), std::size_t(1));
    CHECK(!fs::exists(fs::path(config.storage_dir)));
}

RS_TEST(playtest_job_closed_stops_permanently) {
    std::atomic<int> batches{0};
    auto fake = std::make_shared<Fake>([&](const Request& request) {
        if (ends_with(request.url, "/track/batch")) {
            batches.fetch_add(1);
            return reply(403, "{\"error\":\"playtest_job_closed\"}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.playtest_token = "ptk";
    config.playtest_run_id = "r9";
    config.track_lifecycle_events = true;
    {
        Client client(config, proxy(fake));
        client.track("x");
        REQUIRE(fake->wait_for(3));
        sleep_seconds(0.2);
        CHECK(client.status() == Status::playtest_job_closed);
        CHECK(!client.track("y"));
        CHECK_EQ(client.stats().queued, 0);
        client.flush();
        sleep_seconds(0.1);
    }
    CHECK_EQ(batches.load(), 1);
    CHECK_EQ(fake->total(), std::size_t(3));
}

RS_TEST(invalid_playtest_token_is_session_failed) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/settings")) {
            return reply(403, "{\"error\":\"invalid_playtest_token\"}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.playtest_token = "bad";
    config.playtest_run_id = "r1";
    Client client(config, proxy(fake));
    REQUIRE(fake->wait_for(1));
    sleep_seconds(0.2);
    CHECK(client.status() == Status::session_failed);
    CHECK(!client.track("x"));
    CHECK_EQ(fake->total(), std::size_t(1));
}

// --- Session hand-off -----------------------------------------------------------------

RS_TEST(join_token_is_sent_once_and_session_token_is_readable) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.join_token = "st_from_player";
    Client client(config, proxy(fake));
    CHECK_EQ(client.session_token(), std::string(""));
    client.track("server_event");
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(client.session_token(), std::string("st_test"));

    std::vector<Request> sessions;
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/session")) sessions.push_back(request);
    }
    REQUIRE(sessions.size() == std::size_t(1));
    const json::Value body = json::Value::parse(sessions[0].body);
    CHECK_EQ(body.get_string("joinToken", ""), std::string("st_from_player"));
    CHECK_EQ(body.get_string("gameVersion", ""), std::string("2.0.0"));
    CHECK_EQ(body.get_string("platform", ""), std::string("test"));
    CHECK(!body.get_string("deviceId", "").empty());
}

RS_TEST(refused_join_token_falls_back_to_a_plain_session) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/session") &&
            json::Value::parse(request.body).find("joinToken") != nullptr) {
            return reply(401, "{\"error\":\"invalid_join_token\"}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.join_token = "stale";
    Client client(config, proxy(fake));
    client.track("e");
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(fake->count("/session"), std::size_t(2));
    CHECK_EQ(client.stats().last_error, std::string("invalid_join_token"));
    CHECK_EQ(client.stats().sent, 1);
    CHECK(client.status() == Status::ok);
}

// --- Acknowledgment by sequence ----------------------------------------------------------

RS_TEST(late_batch_ack_after_opt_out_and_back_keeps_new_events) {
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    bool batch_started = false;
    bool release = false;
    std::atomic<int> batches{0};
    auto fake = std::make_shared<Fake>([&](const Request& request) {
        if (ends_with(request.url, "/track/batch") && batches.fetch_add(1) == 0) {
            std::unique_lock<std::mutex> lock(gate_mutex);
            batch_started = true;
            gate_cv.notify_all();
            gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return release; });
            return reply(202, "{}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.flush_interval_seconds = 3600.0;
    Client client(config, proxy(fake));
    client.track("old_1");
    client.track("old_2");
    client.flush();
    {
        std::unique_lock<std::mutex> lock(gate_mutex);
        REQUIRE(gate_cv.wait_for(lock, std::chrono::seconds(3), [&] { return batch_started; }));
    }
    client.set_enabled(false);
    client.set_enabled(true);
    client.track("new_1");
    client.track("new_2");
    {
        std::lock_guard<std::mutex> lock(gate_mutex);
        release = true;
        gate_cv.notify_all();
    }
    CHECK(client.flush_and_wait(3.0));
    const std::vector<std::string> names = fake->sent_event_names();
    REQUIRE(names.size() == std::size_t(4));
    CHECK_EQ(names[2], std::string("new_1"));
    CHECK_EQ(names[3], std::string("new_2"));
    CHECK_EQ(client.stats().sent, 2); // the late 202 removed nothing new
}

// --- Quit ---------------------------------------------------------------------------------

RS_TEST(shutdown_holds_the_quit_for_at_most_about_one_and_a_half_seconds) {
    auto fake = std::make_shared<Fake>([](const Request& request) {
        if (ends_with(request.url, "/track/batch")) {
            // A hung server: give up only when the client's deadline passes.
            const double started = wall_seconds();
            while (wall_seconds() - started < 10.0) {
                const std::atomic<double>* deadline = request.abort_at_unix_seconds;
                if (deadline != nullptr && deadline->load() > 0.0 &&
                    wall_seconds() >= deadline->load()) {
                    return reply(0, "aborted");
                }
                sleep_seconds(0.01);
            }
            return reply(0, "timeout");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.flush_interval_seconds = 3600.0;
    config.track_lifecycle_events = true;
    auto client = std::make_unique<Client>(config, proxy(fake));
    client->track("last");
    REQUIRE(fake->wait_for(1));
    sleep_seconds(0.1);
    const double started = wall_seconds();
    client.reset();
    const double held = wall_seconds() - started;
    CHECK(held >= 1.3);
    CHECK(held < 2.5);
    CHECK_EQ(fake->count("/track/batch"), std::size_t(1));
}

// --- Public builds (0.3.0) -----------------------------------------------------------

namespace {

std::vector<std::string> files_in(const std::string& dir) {
    std::vector<std::string> names;
    std::error_code error;
    if (!fs::exists(fs::path(dir), error)) {
        return names;
    }
    for (const auto& entry : fs::directory_iterator(fs::path(dir), error)) {
        names.push_back(entry.path().filename().string());
    }
    return names;
}

/// Every event's data object from every batch sent, in order.
std::vector<json::Value> sent_event_data(const Fake& fake) {
    std::vector<json::Value> out;
    for (const Request& request : fake.requests()) {
        if (ends_with(request.url, "/track/batch")) {
            const json::Value parsed = json::Value::parse(request.body);
            const json::Value* events = parsed.find("events");
            for (std::size_t i = 0; events != nullptr && i < events->size(); ++i) {
                const json::Value* data = events->at(i).find("data");
                out.push_back(data != nullptr ? *data : json::Value());
            }
        }
    }
    return out;
}

std::string event_id_of(const json::Value& data) { return data.get_string("rs_event_id", ""); }

std::vector<std::string> split_dash(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == '-') {
            parts.push_back(current);
            current.clear();
        } else {
            current.push_back(ch);
        }
    }
    parts.push_back(current);
    return parts;
}

json::Value session_body(const Fake& fake) {
    for (const Request& request : fake.requests()) {
        if (ends_with(request.url, "/session")) {
            return json::Value::parse(request.body);
        }
    }
    return json::Value();
}

} // namespace

RS_TEST(no_key_is_inert) {
    auto fake = std::make_shared<Fake>();
    auto logs = std::make_shared<LogLines>();
    Config config = base_config(logs);
    config.ingest_key = "";
    config.track_lifecycle_events = true;
    const std::string dir = config.storage_dir;
    {
        Client client(config, proxy(fake));
        CHECK(client.status() == Status::no_key);
        CHECK_EQ(std::string(to_string(client.status())), std::string("no_key"));
        CHECK(!client.is_enabled());
        CHECK(!client.is_collecting());
        CHECK(!client.track("level_start", {{"level", 1}}));
        client.flush();
        const double before = wall_seconds();
        CHECK(client.flush_and_wait(3.0));
        CHECK(wall_seconds() - before < 0.5); // no worker to wait for
        std::string feedback_code;
        client.submit_feedback("hi", "bug", 3,
                               [&](const FeedbackResult& result) { feedback_code = result.code; });
        CHECK_EQ(feedback_code, std::string("no_key"));
        bool suggestions_ok = true;
        client.fetch_suggestions([&](bool ok, const json::Value&) { suggestions_ok = ok; });
        CHECK(!suggestions_ok);
        client.set_enabled(false);
        client.set_enabled(true);
        CHECK(client.device_id().empty());
        CHECK(client.reset_device_id().empty());
        sleep_seconds(0.2);
        client.shutdown();
    }
    CHECK_EQ(fake->total(), std::size_t(0));
    CHECK(files_in(dir).empty());
    CHECK_EQ(logs->count_containing("no ingest_key, so analytics is off"), std::size_t(1));
}

RS_TEST(no_key_optional_is_silent_and_still_saves_the_choice) {
    auto fake = std::make_shared<Fake>();
    auto logs = std::make_shared<LogLines>();
    Config config = base_config(logs);
    config.ingest_key = "  ";
    config.optional = true;
    Client client(config, proxy(fake));
    CHECK(client.status() == Status::no_key);
    CHECK_EQ(logs->count_containing("no ingest_key"), std::size_t(0));
    client.set_tracking_enabled(false);
    CHECK(!client.is_tracking_enabled_by_player());
    client.set_tracking_enabled(true);
    CHECK(!client.track("x"));
    sleep_seconds(0.1);
    client.shutdown();
    CHECK_EQ(fake->total(), std::size_t(0));
    // Only the state file the player's choice asked for, no device id.
    const std::vector<std::string> files = files_in(config.storage_dir);
    CHECK_EQ(files.size(), std::size_t(1));
    CHECK(files.empty() || files.front() == "state");
}

RS_TEST(opt_in_before_the_yes_does_nothing) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.consent = Consent::opt_in;
    config.track_lifecycle_events = true;
    {
        Client client(config, proxy(fake));
        CHECK(!client.track("before_yes"));
        client.flush();
        bool suggestions_ok = true;
        client.fetch_suggestions([&](bool ok, const json::Value&) { suggestions_ok = ok; });
        CHECK(!suggestions_ok);
        sleep_seconds(0.3);
        CHECK(client.device_id().empty());
        CHECK(client.status() == Status::disabled);
        CHECK(!client.is_tracking_enabled_by_player());
        CHECK(!client.is_collecting());
        CHECK(client.reset_device_id().empty());
    }
    CHECK_EQ(fake->total(), std::size_t(0));
    CHECK(files_in(config.storage_dir).empty());
}

RS_TEST(opt_in_after_the_yes_starts_and_queues_right_away) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.consent = Consent::opt_in;
    config.track_lifecycle_events = true;
    Client client(config, proxy(fake));
    CHECK(!client.track("before_yes"));
    client.set_tracking_enabled(true);
    CHECK(client.track("right_after_yes"));
    CHECK(!client.device_id().empty());
    std::string saved_id;
    CHECK(detail::read_text_file(detail::join_path(config.storage_dir, "device_id"), saved_id));
    CHECK_EQ(saved_id, client.device_id());
    std::string state;
    CHECK(detail::read_text_file(detail::join_path(config.storage_dir, "state"), state));
    CHECK(state.find("opt_in=true") != std::string::npos);
    CHECK(client.flush_and_wait(3.0));
    const std::vector<std::string> names = fake->sent_event_names();
    REQUIRE(names.size() == 2);
    CHECK_EQ(names[0], std::string("game_started"));
    CHECK_EQ(names[1], std::string("right_after_yes"));
    const std::vector<Request> requests = fake->requests();
    REQUIRE(!requests.empty());
    CHECK(ends_with(requests.front().url, "/settings"));
    CHECK(client.is_collecting());
}

RS_TEST(opt_in_revoke_keeps_the_device_id_and_a_saved_yes_starts_next_launch) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    config.consent = Consent::opt_in;
    std::string id;
    {
        Client client(config, proxy(fake));
        client.set_tracking_enabled(true);
        id = client.device_id();
        CHECK(client.track("one"));
        CHECK(client.flush_and_wait(3.0));
    }
    {
        // A saved yes: the next launch starts at once with the same id.
        Client client(config, proxy(fake));
        CHECK(client.is_tracking_enabled_by_player());
        CHECK_EQ(client.device_id(), id);
        CHECK(client.track("two"));
        CHECK(client.flush_and_wait(3.0));
        const std::size_t before = fake->total();
        client.set_tracking_enabled(false);
        CHECK(!client.track("after_no"));
        sleep_seconds(0.2);
        CHECK_EQ(fake->total(), before);
        std::string saved_id;
        CHECK(detail::read_text_file(detail::join_path(config.storage_dir, "device_id"), saved_id));
        CHECK_EQ(saved_id, id); // kept after the no
        CHECK(client.reset_device_id().empty());
        CHECK(!fs::exists(fs::path(detail::join_path(config.storage_dir, "device_id"))));
    }
    // The yes saved by opt_in does not read as an opt-out in the default mode.
    Config plain = config;
    plain.consent = Consent::opt_out;
    CHECK(set_tracking_enabled(config, true));
    Client client(plain, proxy(fake));
    CHECK(client.is_tracking_enabled_by_player());
}

RS_TEST(distribution_is_sent_on_the_session_body) {
    {
        auto fake = std::make_shared<Fake>();
        Config config = base_config();
        config.distribution = "  native-demo  ";
        Client client(config, proxy(fake));
        CHECK(client.track("x"));
        CHECK(client.flush_and_wait(3.0));
        CHECK_EQ(session_body(*fake).get_string("distribution", "?"), std::string("native-demo"));
    }
    {
        auto fake = std::make_shared<Fake>();
        Config config = base_config();
        config.distribution = std::string(40, 'y');
        Client client(config, proxy(fake));
        CHECK(client.track("x"));
        CHECK(client.flush_and_wait(3.0));
        CHECK_EQ(session_body(*fake).get_string("distribution", "?"), std::string(40, 'y'));
    }
    std::string emoji;
    for (int i = 0; i < 21; ++i) {
        emoji += "\xF0\x9F\x8E\xAE"; // one code point, two UTF-16 units
    }
    for (const std::string& bad : {std::string(41, 'x'), std::string("line\nbreak"),
                                   std::string("del\x7f"), emoji}) {
        auto fake = std::make_shared<Fake>();
        auto logs = std::make_shared<LogLines>();
        Config config = base_config(logs);
        config.distribution = bad;
        Client client(config, proxy(fake));
        CHECK(client.track("x"));
        CHECK(client.flush_and_wait(3.0));
        CHECK(session_body(*fake).find("distribution") == nullptr);
        client.shutdown();
        CHECK_EQ(logs->count_containing("distribution must be at most 40"), std::size_t(1));
    }
    auto fake = std::make_shared<Fake>();
    Client client(base_config(), proxy(fake));
    CHECK(client.track("x"));
    CHECK(client.flush_and_wait(3.0));
    const json::Value body = session_body(*fake);
    CHECK(body.find("distribution") == nullptr);
    // The SDK version is present even without a distribution label.
    CHECK_EQ(body.members().size(), std::size_t(4));
}

RS_TEST(batch_403_ends_the_run) {
    std::atomic<int> batches{0};
    auto fake = std::make_shared<Fake>([&](const Request& request) {
        if (ends_with(request.url, "/track/batch")) {
            ++batches;
            return reply(403, "{\"error\":\"forbidden\"}");
        }
        return default_routes(request);
    });
    Client client(base_config(), proxy(fake));
    CHECK(client.track("x"));
    client.flush_and_wait(3.0);
    sleep_seconds(0.2);
    CHECK(client.status() == Status::invalid_key);
    CHECK_EQ(client.stats().queued, 0);
    CHECK(!client.track("y"));
    client.flush();
    sleep_seconds(0.2);
    client.shutdown();
    CHECK_EQ(batches.load(), 1);
}

RS_TEST(rs_event_id_shape_prefix_and_resend) {
    std::atomic<int> batches{0};
    auto fake = std::make_shared<Fake>([&](const Request& request) {
        if (ends_with(request.url, "/track/batch") && batches++ == 0) {
            return reply(500, "{\"error\":\"internal_error\"}");
        }
        return default_routes(request);
    });
    Config config = base_config();
    config.flush_interval_seconds = 0;
    Client client(config, proxy(fake));
    CHECK(client.track("a", {{"level", 1}}));
    CHECK(client.track("b"));
    client.flush_and_wait(3.0);       // the first batch fails with 500
    CHECK(client.flush_and_wait(3.0)); // a forced resend
    const std::vector<json::Value> data = sent_event_data(*fake);
    REQUIRE(data.size() == 4);
    CHECK_EQ(event_id_of(data[0]), event_id_of(data[2]));
    CHECK_EQ(event_id_of(data[1]), event_id_of(data[3]));
    CHECK(event_id_of(data[0]) != event_id_of(data[1]));
    const std::regex shape("^[A-Za-z0-9_-]{1,64}$");
    const std::regex nonce("^[0-9a-f]{8}$");
    for (const json::Value& d : data) {
        const std::string id = event_id_of(d);
        CHECK(std::regex_match(id, shape));
        const std::vector<std::string> parts = split_dash(id);
        REQUIRE(parts.size() == 4);
        CHECK_EQ(parts[0], client.device_id().substr(0, 16));
        CHECK(std::regex_match(parts[2], nonce));
    }
    double level = 0;
    CHECK(data[0].get_number("level", level));
    CHECK_EQ(level, 1.0);
}

RS_TEST(rs_event_id_prefix_survives_a_relaunch) {
    Config config = base_config();
    std::string first;
    {
        auto fake = std::make_shared<Fake>();
        Client client(config, proxy(fake));
        CHECK(client.track("a"));
        CHECK(client.flush_and_wait(3.0));
        first = event_id_of(sent_event_data(*fake).at(0));
    }
    auto fake = std::make_shared<Fake>();
    Client client(config, proxy(fake));
    CHECK(client.track("a"));
    CHECK(client.flush_and_wait(3.0));
    const std::string second = event_id_of(sent_event_data(*fake).at(0));
    CHECK_EQ(split_dash(first)[0], split_dash(second)[0]);
    CHECK(split_dash(first)[2] != split_dash(second)[2]);

    // A playtest id with characters outside the shape is made safe.
    auto pt_fake = std::make_shared<Fake>();
    Config pt = base_config();
    pt.playtest_token = "ptk";
    pt.playtest_run_id = "run.with spaces";
    Client playtest(pt, proxy(pt_fake));
    CHECK(playtest.track("p"));
    CHECK(playtest.flush_and_wait(3.0));
    const std::string pid = event_id_of(sent_event_data(*pt_fake).at(0));
    CHECK(std::regex_match(pid, std::regex("^[A-Za-z0-9_-]{1,64}$")));
}

RS_TEST(event_over_8kb_is_dropped_with_one_warning) {
    auto fake = std::make_shared<Fake>();
    auto logs = std::make_shared<LogLines>();
    Client client(base_config(logs), proxy(fake));
    CHECK(!client.track("huge", {{"blob", std::string(8200, 'x')}}));
    CHECK(!client.track("huge_again", {{"blob", std::string(9000, 'x')}}));
    CHECK_EQ(client.stats().dropped, 2);
    CHECK_EQ(client.stats().last_error, std::string("event_too_large"));
    CHECK_EQ(client.stats().queued, 0);
    CHECK(client.track("fits", {{"blob", std::string(8000, 'x')}}));
    CHECK(client.flush_and_wait(3.0));
    client.shutdown();
    const std::vector<std::string> names = fake->sent_event_names();
    REQUIRE(names.size() == 1);
    CHECK_EQ(names[0], std::string("fits"));
    CHECK_EQ(logs->count_containing("larger than 8192 bytes"), std::size_t(1));
}

RS_TEST(a_key_only_client_writes_only_the_device_id) {
    auto fake = std::make_shared<Fake>();
    Config config = base_config();
    {
        Client client(config, proxy(fake));
        CHECK(client.track("x"));
        CHECK(client.flush_and_wait(3.0));
    }
    const std::vector<std::string> files = files_in(config.storage_dir);
    REQUIRE(files.size() == 1);
    CHECK_EQ(files[0], std::string("device_id"));
}

RS_TEST(random_unit_draws_stay_in_range) {
    for (int i = 0; i < 2000; ++i) {
        const double draw = detail::random_unit();
        CHECK(draw >= 0.0 && draw < 1.0);
    }
    CHECK_EQ(detail::random_hex(4).size(), std::size_t(8));
}
