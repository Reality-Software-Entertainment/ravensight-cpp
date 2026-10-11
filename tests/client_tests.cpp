// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// End to end tests for the Client worker thread, driven through a scripted
// fake transport. No network, no libcurl.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ravensight/client.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;

namespace {

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

std::size_t batch_event_count(const Request& request) {
    const json::Value parsed = json::Value::parse(request.body);
    const json::Value* events = parsed.find("events");
    return events != nullptr ? events->size() : 0;
}

class FakeTransport : public Transport {
public:
    using Responder = std::function<Response(const Request&)>;

    explicit FakeTransport(Responder responder) : responder_(std::move(responder)) {}

    Response send(const Request& request) override {
        Response response = responder_(request);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(request);
        }
        cv_.notify_all();
        return response;
    }

    bool wait_for_requests(std::size_t count, double timeout_seconds = 3.0) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, std::chrono::duration<double>(timeout_seconds),
                            [&] { return requests_.size() >= count; });
    }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    std::size_t count_matching(const std::string& url_suffix) const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::size_t count = 0;
        for (const auto& request : requests_) {
            if (ends_with(request.url, url_suffix)) {
                ++count;
            }
        }
        return count;
    }

private:
    Responder responder_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Request> requests_;
};

/// The Client owns its Transport, so tests that inspect requests after the
/// Client is gone hand it a proxy and keep the fake alive via shared_ptr.
class ProxyTransport : public Transport {
public:
    explicit ProxyTransport(std::shared_ptr<FakeTransport> inner) : inner_(std::move(inner)) {}
    Response send(const Request& request) override { return inner_->send(request); }

private:
    std::shared_ptr<FakeTransport> inner_;
};

Response json_response(long status, const std::string& body) {
    Response response;
    response.status = status;
    response.body = body;
    return response;
}

Response default_routes(const Request& request) {
    if (ends_with(request.url, "/settings")) {
        return json_response(200, "{\"trackingEnabled\":true}");
    }
    if (ends_with(request.url, "/session")) {
        return json_response(201, "{\"token\":\"st_test\",\"expiresIn\":86400}");
    }
    if (ends_with(request.url, "/track/batch")) {
        return json_response(202, "{\"accepted\":true}");
    }
    if (ends_with(request.url, "/feedback")) {
        return json_response(201, "{}");
    }
    if (ends_with(request.url, "/agent/suggestions")) {
        return json_response(200, "{\"suggestions\":[{\"title\":\"a\"},{\"title\":\"b\"}]}");
    }
    return json_response(404, "{\"error\":\"not_found\"}");
}

Config test_config() {
    Config config;
    config.ingest_key = "gt_live_test_key";
    // Never touch the real per user directory from a test.
    config.storage_dir = rstest::make_temp_dir();
    config.log = [](const std::string&) {};
    config.api_url = "https://api.example.test";
    config.game_version = "9.9.9";
    config.platform = "test";
    config.device_id = "dev_fixed";
    config.flush_interval_seconds = 0.05;
    config.track_lifecycle_events = false;
    return config;
}

} // namespace

RS_TEST(client_requires_an_ingest_key) {
    Config config = test_config();
    config.ingest_key = "  ";
    bool threw = false;
    try {
        Client client(config, std::make_unique<FakeTransport>(&default_routes));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

RS_TEST(client_normalizes_api_urls) {
    CHECK_EQ(Client::normalize_api_url("https://api.ravensight.io"),
             std::string("https://api.ravensight.io/api/v1"));
    CHECK_EQ(Client::normalize_api_url("https://api.ravensight.io/"),
             std::string("https://api.ravensight.io/api/v1"));
    CHECK_EQ(Client::normalize_api_url("https://api.ravensight.io/api/v1"),
             std::string("https://api.ravensight.io/api/v1"));
    CHECK_EQ(Client::normalize_api_url(""), std::string("https://api.ravensight.io/api/v1"));
    CHECK_EQ(Client::normalize_api_url("   "), std::string("https://api.ravensight.io/api/v1"));
    CHECK_EQ(Client::normalize_api_url("https://api.ravensight.io/api/v1/"),
             std::string("https://api.ravensight.io/api/v1"));
    CHECK_EQ(Client::normalize_api_url("  https://self.example.test//  "),
             std::string("https://self.example.test/api/v1"));
    CHECK_EQ(Client::normalize_api_url("https://self.example.test/api/v1//"),
             std::string("https://self.example.test/api/v1"));
    CHECK_EQ(Client::normalize_api_url("http://localhost:3000/api/v1"),
             std::string("http://localhost:3000/api/v1"));
    // The default is the bare host.
    CHECK_EQ(Config{}.api_url, std::string("https://api.ravensight.io"));
}

RS_TEST(client_happy_path_sends_session_then_batch) {
    auto fake = std::make_shared<FakeTransport>(&default_routes);
    Client client(test_config(), std::make_unique<ProxyTransport>(fake));
    CHECK(client.track("level_completed", {{"level", 3}, {"seconds", 41.5}}));
    CHECK(client.track("player_died"));
    CHECK(client.flush_and_wait(3.0));

    REQUIRE(fake->wait_for_requests(3));
    const std::vector<Request> requests = fake->requests();

    // Boot order: settings with the ingest key, then session, then the batch.
    CHECK(ends_with(requests[0].url, "/api/v1/settings"));
    CHECK_EQ(header_value(requests[0], "X-API-Key"), std::string("gt_live_test_key"));

    CHECK(ends_with(requests[1].url, "/api/v1/session"));
    CHECK_EQ(requests[1].method, std::string("POST"));
    const json::Value session_body = json::Value::parse(requests[1].body);
    CHECK_EQ(session_body.get_string("deviceId", ""), std::string("dev_fixed"));
    CHECK_EQ(session_body.get_string("gameVersion", ""), std::string("9.9.9"));
    CHECK_EQ(session_body.get_string("platform", ""), std::string("test"));

    CHECK(ends_with(requests[2].url, "/api/v1/track/batch"));
    CHECK_EQ(header_value(requests[2], "X-Session-Token"), std::string("st_test"));
    const json::Value batch = json::Value::parse(requests[2].body);
    const json::Value* events = batch.find("events");
    REQUIRE(events != nullptr);
    CHECK_EQ(events->size(), std::size_t(2));
    CHECK_EQ(events->at(0).get_string("event", ""), std::string("level_completed"));
    double level = 0.0;
    CHECK(events->at(0).find("data")->get_number("level", level));
    CHECK_EQ(level, 3.0);
    CHECK(client.is_ready());
    CHECK_EQ(client.stats().sent, 2);
}

RS_TEST(client_kill_switch_disables_everything) {
    auto fake = std::make_shared<FakeTransport>([](const Request& request) {
        if (ends_with(request.url, "/settings")) {
            return json_response(200, "{\"trackingEnabled\":false}");
        }
        return default_routes(request);
    });
    Config config = test_config();
    config.track_lifecycle_events = true;
    Client client(config, std::make_unique<ProxyTransport>(fake));

    REQUIRE(fake->wait_for_requests(1));
    // Give the worker a moment to apply the switch, then verify tracking is off.
    client.flush_and_wait(1.0);
    CHECK(!client.tracking_enabled());
    CHECK(!client.track("ignored"));
    client.flush_and_wait(1.0);
    client.shutdown();
    // Only the settings call ever went out: no session, no batches, even
    // though lifecycle events were enabled.
    CHECK_EQ(fake->requests().size(), std::size_t(1));
}

RS_TEST(client_lifecycle_events_bracket_the_run) {
    auto fake = std::make_shared<FakeTransport>(&default_routes);
    Config config = test_config();
    config.track_lifecycle_events = true;
    {
        Client client(config, std::make_unique<ProxyTransport>(fake));
        client.flush_and_wait(3.0);
    } // destructor shuts down and flushes game_exited

    std::vector<std::string> names;
    for (const Request& request : fake->requests()) {
        if (!ends_with(request.url, "/track/batch")) {
            continue;
        }
        const json::Value batch = json::Value::parse(request.body);
        const json::Value* events = batch.find("events");
        REQUIRE(events != nullptr);
        for (std::size_t i = 0; i < events->size(); ++i) {
            names.push_back(events->at(i).get_string("event", ""));
        }
    }
    REQUIRE(names.size() == std::size_t(2));
    CHECK_EQ(names.front(), std::string("game_started"));
    CHECK_EQ(names.back(), std::string("game_exited"));
}

RS_TEST(client_batches_of_fifty) {
    auto fake = std::make_shared<FakeTransport>(&default_routes);
    Client client(test_config(), std::make_unique<ProxyTransport>(fake));
    for (int i = 0; i < 120; ++i) {
        client.track("spam", {{"i", i}});
    }
    CHECK(client.flush_and_wait(5.0));

    // Batches may start as soon as 50 are queued, so their boundaries depend
    // on timing; none is ever larger than 50 and every event goes out once.
    std::size_t total = 0;
    std::size_t batches = 0;
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/track/batch")) {
            const std::size_t size = batch_event_count(request);
            CHECK(size <= std::size_t(50));
            CHECK(size > std::size_t(0));
            total += size;
            ++batches;
        }
    }
    CHECK_EQ(total, std::size_t(120));
    CHECK(batches >= std::size_t(3));
    CHECK_EQ(client.stats().sent, 120);
    CHECK_EQ(client.stats().queued, 0);
}

RS_TEST(client_reauthenticates_on_401_and_resends_the_batch) {
    std::atomic<int> batch_calls{0};
    auto fake = std::make_shared<FakeTransport>([&](const Request& request) {
        if (ends_with(request.url, "/track/batch")) {
            if (batch_calls.fetch_add(1) == 0) {
                return json_response(401, "{\"error\":\"session_expired\"}");
            }
            return json_response(202, "{}");
        }
        return default_routes(request);
    });
    Client client(test_config(), std::make_unique<ProxyTransport>(fake));
    client.track("important", {{"n", 1}});
    CHECK(client.flush_and_wait(5.0));

    CHECK_EQ(fake->count_matching("/session"), std::size_t(2));
    CHECK_EQ(fake->count_matching("/track/batch"), std::size_t(2));
    // The same event went out both times; nothing was lost to the 401.
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/track/batch")) {
            CHECK_EQ(batch_event_count(request), std::size_t(1));
        }
    }
    CHECK_EQ(client.stats().sent, 1);
}

RS_TEST(client_splits_oversize_batches_on_400) {
    auto fake = std::make_shared<FakeTransport>([&](const Request& request) {
        if (ends_with(request.url, "/track/batch")) {
            if (batch_event_count(request) > 10) {
                return json_response(400, "{\"error\":\"batch_too_large\"}");
            }
            return json_response(202, "{}");
        }
        return default_routes(request);
    });
    Client client(test_config(), std::make_unique<ProxyTransport>(fake));
    for (int i = 0; i < 40; ++i) {
        client.track("spam", {{"i", i}});
    }
    CHECK(client.flush_and_wait(5.0));

    std::size_t delivered = 0;
    std::size_t rejected = 0;
    for (const Request& request : fake->requests()) {
        if (!ends_with(request.url, "/track/batch")) {
            continue;
        }
        if (batch_event_count(request) > 10) {
            ++rejected;
        } else {
            delivered += batch_event_count(request);
        }
    }
    CHECK(rejected >= std::size_t(1));
    // Every event was eventually delivered in acceptable batch sizes.
    CHECK_EQ(delivered, std::size_t(40));
    CHECK_EQ(client.stats().sent, 40);
    CHECK_EQ(client.stats().dropped, 0);
}

RS_TEST(client_local_opt_out_discards_queue) {
    auto fake = std::make_shared<FakeTransport>(&default_routes);
    Client client(test_config(), std::make_unique<ProxyTransport>(fake));
    client.track("secret", {{"x", 1}});
    client.set_enabled(false);
    CHECK(!client.is_enabled());
    CHECK(!client.track("still_secret"));
    client.flush_and_wait(1.0);
    CHECK_EQ(client.stats().sent, 0);
    CHECK_EQ(client.stats().queued, 0);
}

RS_TEST(client_feedback_success_and_validation) {
    auto fake = std::make_shared<FakeTransport>(&default_routes);
    Client client(test_config(), std::make_unique<ProxyTransport>(fake));

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    FeedbackResult result;
    client.submit_feedback("The boss fight drags", "suggestion", 3, [&](const FeedbackResult& r) {
        std::lock_guard<std::mutex> lock(mutex);
        result = r;
        done = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return done; }));
    }
    CHECK(result.ok);
    CHECK_EQ(result.status, 201L);

    bool found = false;
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/feedback")) {
            found = true;
            const json::Value body = json::Value::parse(request.body);
            CHECK_EQ(body.get_string("message", ""), std::string("The boss fight drags"));
            CHECK_EQ(body.get_string("category", ""), std::string("suggestion"));
            double rating = 0.0;
            CHECK(body.get_number("rating", rating));
            CHECK_EQ(rating, 3.0);
            CHECK_EQ(header_value(request, "X-Session-Token"), std::string("st_test"));
        }
    }
    CHECK(found);

    // An empty message fails locally, synchronously, without a request.
    FeedbackResult local;
    client.submit_feedback("   ", "", 0, [&](const FeedbackResult& r) { local = r; });
    CHECK(!local.ok);
    CHECK_EQ(local.code, std::string("empty_message"));
}

RS_TEST(client_fetches_suggestions) {
    auto fake = std::make_shared<FakeTransport>(&default_routes);
    Client client(test_config(), std::make_unique<ProxyTransport>(fake));

    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool ok = false;
    std::size_t count = 0;
    std::string first_title;
    client.fetch_suggestions([&](bool request_ok, const json::Value& suggestions) {
        std::lock_guard<std::mutex> lock(mutex);
        ok = request_ok;
        count = suggestions.size();
        first_title = suggestions.at(0).get_string("title", "");
        done = true;
        cv.notify_all();
    });
    {
        std::unique_lock<std::mutex> lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(3), [&] { return done; }));
    }
    CHECK(ok);
    CHECK_EQ(count, std::size_t(2));
    CHECK_EQ(first_title, std::string("a"));
}

RS_TEST(client_shutdown_flushes_whatever_is_left) {
    auto fake = std::make_shared<FakeTransport>(&default_routes);
    {
        Config config = test_config();
        config.flush_interval_seconds = 3600.0; // never fires during the test
        Client client(config, std::make_unique<ProxyTransport>(fake));
        client.track("last_event");
    } // destructor: shutdown flush

    std::size_t delivered = 0;
    for (const Request& request : fake->requests()) {
        if (ends_with(request.url, "/track/batch")) {
            delivered += batch_event_count(request);
        }
    }
    CHECK_EQ(delivered, std::size_t(1));
}
