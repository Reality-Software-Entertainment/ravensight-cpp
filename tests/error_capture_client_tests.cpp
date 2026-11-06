// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automatic error capture through a real Client with a fake transport: the
// gate (key, consent, the game's switch), the boot hold, the log sink, the
// trail, the device caps, queue order, and a crash saved by the previous
// run. No network, no libcurl, nothing near the production API.

#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "error_capture.hpp"
#include "ravensight/client.hpp"
#include "ravensight/error_capture.hpp"
#include "storage.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;

namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Response reply(long status, const std::string& body) {
    Response response;
    response.status = status;
    response.body = body;
    return response;
}

/// Answers like the API, with a chosen errorCapture object on /settings and
/// /session (empty means the key is left out). /settings can be held back to
/// test the boot hold.
class ErrorFake : public Transport {
public:
    explicit ErrorFake(std::string capture_json, bool hold_settings = false)
        : capture_(std::move(capture_json)), hold_(hold_settings) {}

    Response send(const Request& request) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            requests_.push_back(request);
            cv_.notify_all();
            if (ends_with(request.url, "/settings")) {
                cv_.wait_for(lock, std::chrono::seconds(5), [&] { return !hold_; });
            }
        }
        const std::string extra = capture_.empty() ? "" : ",\"errorCapture\":" + capture_;
        if (ends_with(request.url, "/settings")) {
            return reply(200, "{\"trackingEnabled\":true" + extra + "}");
        }
        if (ends_with(request.url, "/session")) {
            return reply(201, "{\"token\":\"st_test\",\"expiresIn\":86400" + extra + "}");
        }
        if (ends_with(request.url, "/track/batch")) {
            return reply(202, "{}");
        }
        return reply(404, "{}");
    }

    void release_settings() {
        std::lock_guard<std::mutex> lock(mutex_);
        hold_ = false;
        cv_.notify_all();
    }

    bool wait_for_settings() {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::seconds(3), [&] {
            for (const Request& r : requests_) {
                if (ends_with(r.url, "/settings")) return true;
            }
            return false;
        });
    }

    std::size_t total() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    /// Every delivered event, in order: name and data.
    std::vector<std::pair<std::string, json::Value>> events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<std::string, json::Value>> out;
        for (const Request& r : requests_) {
            if (!ends_with(r.url, "/track/batch")) continue;
            const json::Value body = json::Value::parse(r.body);
            const json::Value* list = body.find("events");
            for (std::size_t i = 0; list != nullptr && i < list->size(); ++i) {
                const json::Value& e = list->at(i);
                const json::Value* data = e.find("data");
                out.emplace_back(e.get_string("event", ""), data ? *data : json::Value());
            }
        }
        return out;
    }

    std::vector<json::Value> errors() const {
        std::vector<json::Value> out;
        for (const auto& e : events()) {
            if (e.second.get_string("rs_kind", "") == "error") out.push_back(e.second);
        }
        return out;
    }

private:
    std::string capture_;
    bool hold_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Request> requests_;
};

class Proxy : public Transport {
public:
    explicit Proxy(std::shared_ptr<ErrorFake> inner) : inner_(std::move(inner)) {}
    Response send(const Request& request) override { return inner_->send(request); }

private:
    std::shared_ptr<ErrorFake> inner_;
};

Config error_config(const std::string& dir = rstest::make_temp_dir("ravensight-err-")) {
    Config config;
    config.ingest_key = "gt_live_test_key";
    config.api_url = "https://api.example.test";
    config.platform = "test";
    config.storage_dir = dir;
    config.flush_interval_seconds = 3600.0; // flush_and_wait and shutdown only
    config.track_lifecycle_events = false;
    config.log = [](const std::string&) {};
    return config;
}

const char* const on_with_trail = "{\"enabled\":true,\"trail\":true}";
const char* const on_no_trail = "{\"enabled\":true,\"trail\":false}";
const char* const off = "{\"enabled\":false,\"trail\":true}";

/// Returns once the settings answer (and so the game's switch) is in: a
/// flush runs the settings check first, on the worker.
void wait_ready(Client& client) {
    client.flush_and_wait(3.0);
}

} // namespace

RS_TEST(log_error_becomes_one_scrubbed_warning_event) {
    auto fake = std::make_shared<ErrorFake>(on_no_trail);
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        CHECK(client.is_error_capture_active());
        client.track("level_started");
        client.flush_and_wait(3.0);
        ravensight::log(LogLevel::info, "loading level 3");
        ravensight::log(LogLevel::error, "save failed for jane@example.com at /Users/jane/save1.dat");
        client.flush_and_wait(3.0);
    }
    const auto errors = fake->errors();
    REQUIRE(errors.size() == std::size_t(1));
    CHECK_EQ(errors[0].get_string("rs_severity", ""), std::string("warning"));
    CHECK_EQ(errors[0].get_string("rs_source", ""), std::string("log"));
    CHECK_EQ(errors[0].get_string("rs_message", ""),
             std::string("save failed for [email] at ~/save1.dat"));
    CHECK(errors[0].find("rs_trail") == nullptr); // trail switch off
    CHECK(errors[0].find("rs_fp") == nullptr);
}

RS_TEST(trail_is_attached_only_when_the_game_turns_it_on) {
    auto fake = std::make_shared<ErrorFake>(on_with_trail);
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        wait_ready(client);
        client.flush_and_wait(3.0);
        client.track("boss_entered");
        auto sink = ravensight::log_sink();
        sink(LogLevel::info, "boss phase 2 from 10.0.0.1");
        sink(LogLevel::critical, "boss AI stuck");
        client.flush_and_wait(3.0);
    }
    const auto errors = fake->errors();
    REQUIRE(errors.size() == std::size_t(1));
    const std::string trail = errors[0].get_string("rs_trail", "");
    CHECK(trail.find(" event boss_entered") != std::string::npos);
    CHECK(trail.find(" info boss phase 2 from [ip]") != std::string::npos);
    CHECK(trail.find("boss AI stuck") == std::string::npos); // not in its own trail
    CHECK(trail.find('+') == 0);
}

RS_TEST(game_switch_off_uninstalls_and_sends_nothing) {
    auto fake = std::make_shared<ErrorFake>(off);
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        wait_ready(client);
        client.flush_and_wait(3.0);
        CHECK(!client.is_error_capture_active());
        ravensight::log(LogLevel::error, "should not be sent");
        client.track_error("error", "manual still works");
        client.flush_and_wait(3.0);
    }
    const auto errors = fake->errors();
    REQUIRE(errors.size() == std::size_t(1));
    CHECK_EQ(errors[0].get_string("rs_message", ""), std::string("manual still works"));
    CHECK(errors[0].find("rs_source") == nullptr);
}

RS_TEST(boot_errors_are_held_then_sent_when_the_answer_says_on) {
    auto fake = std::make_shared<ErrorFake>(on_no_trail, true);
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        REQUIRE(fake->wait_for_settings());
        for (int i = 0; i < 8; ++i) {
            ravensight::log(LogLevel::error, "boot failure kind " + std::string(1, char('a' + i)));
        }
        CHECK(fake->events().empty());
        fake->release_settings();
        wait_ready(client);
        client.flush_and_wait(3.0);
    }
    // At most five held, and only five were ever allowed by the bucket of six
    // anyway: the hold caps first.
    CHECK_EQ(fake->errors().size(), std::size_t(5));
}

RS_TEST(boot_errors_are_discarded_when_the_answer_says_off) {
    auto fake = std::make_shared<ErrorFake>(off, true);
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        REQUIRE(fake->wait_for_settings());
        ravensight::log(LogLevel::error, "boot failure");
        fake->release_settings();
        wait_ready(client);
        client.track("after");
        client.flush_and_wait(3.0);
    }
    CHECK(fake->errors().empty());
}

RS_TEST(absent_error_capture_means_on) {
    auto fake = std::make_shared<ErrorFake>("");
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        wait_ready(client);
        ravensight::log(LogLevel::error, "older API answer");
        client.flush_and_wait(3.0);
    }
    CHECK_EQ(fake->errors().size(), std::size_t(1));
}

RS_TEST(no_key_no_consent_or_config_off_installs_nothing) {
    {
        Config config = error_config();
        config.ingest_key = "";
        config.optional = true;
        Client client(config);
        CHECK(!client.is_error_capture_active());
        ravensight::log(LogLevel::error, "nothing");
    }
    {
        auto fake = std::make_shared<ErrorFake>(on_no_trail);
        Config config = error_config();
        config.consent = Consent::opt_in;
        Client client(config, std::make_unique<Proxy>(fake));
        CHECK(!client.is_error_capture_active());
        ravensight::log(LogLevel::error, "before consent");
        CHECK_EQ(fake->total(), std::size_t(0));
        client.set_tracking_enabled(true);
        CHECK(client.is_error_capture_active());
        client.set_tracking_enabled(false);
        CHECK(!client.is_error_capture_active());
    }
    {
        auto fake = std::make_shared<ErrorFake>(on_no_trail);
        Config config = error_config();
        config.error_capture = false;
        Client client(config, std::make_unique<Proxy>(fake));
        CHECK(!client.is_error_capture_active());
        client.set_error_capture(true);
        CHECK(client.is_error_capture_active());
    }
}

RS_TEST(the_sdk_own_log_lines_are_never_captured) {
    auto fake = std::make_shared<ErrorFake>(on_with_trail);
    {
        Config config = error_config();
        // A host that routes the SDK's lines into its own logger, which
        // feeds ravensight::log: no loop, no capture.
        config.log = [](const std::string& line) { ravensight::log(LogLevel::error, line); };
        config.distribution = std::string(60, 'x'); // makes the SDK warn
        Client client(config, std::make_unique<Proxy>(fake));
        wait_ready(client);
        ravensight::log(LogLevel::error, "Ravensight: something the SDK said");
        client.flush_and_wait(3.0);
    }
    CHECK(fake->errors().empty());
}

RS_TEST(device_caps_bound_an_error_loop) {
    auto fake = std::make_shared<ErrorFake>(on_with_trail);
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        wait_ready(client);
        client.flush_and_wait(3.0);
        for (int i = 0; i < 500; ++i) {
            ravensight::log(LogLevel::error, "enemy " + std::to_string(i) + " fell through the floor");
        }
        client.flush_and_wait(3.0);
    }
    // One dedupe key (the number is templated away): three at most.
    CHECK_EQ(fake->errors().size(), std::size_t(3));
}

RS_TEST(captures_never_jump_gameplay_events) {
    auto fake = std::make_shared<ErrorFake>(on_no_trail);
    {
        Client client(error_config(), std::make_unique<Proxy>(fake));
        wait_ready(client);
        client.flush_and_wait(3.0);
        ravensight::log(LogLevel::error, "first problem");
        client.track("a");
        ravensight::log(LogLevel::error, "second problem");
        client.track("b");
        client.flush_and_wait(3.0);
    }
    std::vector<std::string> kinds;
    for (const auto& e : fake->events()) {
        kinds.push_back(e.first);
    }
    const std::vector<std::string> expected{"a", "b", "error", "error"};
    CHECK(kinds == expected);
}

RS_TEST(a_crash_saved_by_the_last_run_is_sent_once_on_the_next_launch) {
    const std::string dir = rstest::make_temp_dir("ravensight-err-");
    detail::CrashRecord record;
    record.type = "std::runtime_error";
    record.message = "save failed for /home/sam/slot1.sav";
    record.timestamp = 1700000000;
    REQUIRE(detail::write_text_file(detail::join_path(dir, "crash"), detail::serialize_crash(record)));
    REQUIRE(detail::write_text_file(detail::join_path(dir, "crash_signal"), "signal 11 1700000005\n"));

    auto fake = std::make_shared<ErrorFake>(on_no_trail);
    {
        Client client(error_config(dir), std::make_unique<Proxy>(fake));
        wait_ready(client);
        client.flush_and_wait(3.0);
    }
    const auto errors = fake->errors();
    REQUIRE(errors.size() == std::size_t(2));
    CHECK_EQ(errors[0].get_string("rs_source", ""), std::string("crash"));
    CHECK_EQ(errors[0].get_string("rs_severity", ""), std::string("critical"));
    CHECK_EQ(errors[0].get_string("rs_error_type", ""), std::string("std::runtime_error"));
    CHECK_EQ(errors[0].get_string("rs_message", ""), std::string("save failed for ~/slot1.sav"));
    CHECK_EQ(errors[1].get_string("rs_error_type", ""), std::string("SIGSEGV"));
    std::string text;
    CHECK(!detail::read_text_file(detail::join_path(dir, "crash"), text));

    // The next launch sends nothing again.
    auto again = std::make_shared<ErrorFake>(on_no_trail);
    {
        Client client(error_config(dir), std::make_unique<Proxy>(again));
        wait_ready(client);
        client.flush_and_wait(3.0);
    }
    CHECK(again->errors().empty());
}

RS_TEST(a_saved_crash_is_discarded_when_the_game_switch_is_off) {
    const std::string dir = rstest::make_temp_dir("ravensight-err-");
    detail::CrashRecord record;
    record.type = "std::logic_error";
    record.message = "bad state";
    REQUIRE(detail::write_text_file(detail::join_path(dir, "crash"), detail::serialize_crash(record)));
    auto fake = std::make_shared<ErrorFake>(off);
    {
        Client client(error_config(dir), std::make_unique<Proxy>(fake));
        wait_ready(client);
        client.flush_and_wait(3.0);
    }
    CHECK(fake->errors().empty());
}

RS_TEST(only_one_client_holds_capture_and_shutdown_releases_it) {
    const std::terminate_handler before = std::get_terminate();
    auto first_fake = std::make_shared<ErrorFake>(on_no_trail);
    auto second_fake = std::make_shared<ErrorFake>(on_no_trail);
    {
        Client first(error_config(), std::make_unique<Proxy>(first_fake));
        Client second(error_config(), std::make_unique<Proxy>(second_fake));
        CHECK(first.is_error_capture_active());
        CHECK(!second.is_error_capture_active());
        CHECK(std::get_terminate() != before);
    }
    CHECK(std::get_terminate() == before);
    CHECK(!detail::terminate_handler_installed());
}
