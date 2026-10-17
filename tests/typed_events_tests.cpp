// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Typed event helpers and the opt-in health summary, driven through a fake
// transport. No network, no libcurl.

#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "health.hpp"
#include "ravensight/client.hpp"
#include "ravensight/version.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;

namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

Response json_response(long status, const std::string& body) {
    Response response;
    response.status = status;
    response.body = body;
    return response;
}

class RecordingTransport : public Transport {
public:
    Response send(const Request& request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        if (ends_with(request.url, "/settings")) {
            return json_response(200, "{\"trackingEnabled\":true}");
        }
        if (ends_with(request.url, "/session")) {
            return json_response(201, "{\"token\":\"st_test\",\"expiresIn\":86400}");
        }
        if (ends_with(request.url, "/track/batch")) {
            return json_response(202, "{\"accepted\":true}");
        }
        return json_response(404, "{\"error\":\"not_found\"}");
    }

    std::vector<Request> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<Request> requests_;
};

class ProxyTransport : public Transport {
public:
    explicit ProxyTransport(std::shared_ptr<RecordingTransport> inner) : inner_(std::move(inner)) {}
    Response send(const Request& request) override { return inner_->send(request); }

private:
    std::shared_ptr<RecordingTransport> inner_;
};

Config test_config() {
    Config config;
    config.ingest_key = "gt_live_test_key";
    config.storage_dir = rstest::make_temp_dir();
    config.log = [](const std::string&) {};
    config.api_url = "https://api.example.test";
    config.platform = "test";
    config.device_id = "dev_fixed";
    config.flush_interval_seconds = 3600.0; // only shutdown flushes
    config.track_lifecycle_events = false;
    return config;
}

/// One delivered event: its name and the parsed data object.
struct Sent {
    std::string name;
    json::Value data;
};

std::vector<Sent> delivered(const RecordingTransport& transport) {
    std::vector<Sent> out;
    for (const Request& request : transport.requests()) {
        if (!ends_with(request.url, "/track/batch")) {
            continue;
        }
        const json::Value batch = json::Value::parse(request.body);
        const json::Value* events = batch.find("events");
        if (events == nullptr) {
            continue;
        }
        for (std::size_t i = 0; i < events->size(); ++i) {
            const json::Value& event = events->at(i);
            Sent sent;
            sent.name = event.get_string("event", "");
            const json::Value* data = event.find("data");
            if (data != nullptr) {
                sent.data = *data;
            }
            out.push_back(std::move(sent));
        }
    }
    return out;
}

/// Runs `body` against a client, shuts it down and returns what was sent.
template <typename Body>
std::vector<Sent> run(Config config, Body body, std::shared_ptr<RecordingTransport>* keep = nullptr) {
    auto transport = std::make_shared<RecordingTransport>();
    {
        Client client(config, std::make_unique<ProxyTransport>(transport));
        body(client);
        client.shutdown();
    }
    if (keep != nullptr) {
        *keep = transport;
    }
    return delivered(*transport);
}

template <typename Body>
std::vector<Sent> run(Body body) {
    return run(test_config(), std::move(body));
}

std::string str(const json::Value& data, const std::string& key) {
    return data.get_string(key, "<missing>");
}

double num(const json::Value& data, const std::string& key) {
    double out = -12345.0;
    data.get_number(key, out);
    return out;
}

bool has(const json::Value& data, const std::string& key) { return data.find(key) != nullptr; }

} // namespace

RS_TEST(typed_purchase_sends_the_exact_property_set) {
    const auto events = run([](Client& client) {
        CHECK(client.track_purchase(499, "usd", "pack", "gems_500", "cart_1", "txn_9"));
    });
    REQUIRE(events.size() == 1);
    CHECK_EQ(events[0].name, std::string("purchase"));
    const json::Value& d = events[0].data;
    CHECK_EQ(str(d, "rs_kind"), std::string("purchase"));
    CHECK_EQ(num(d, "rs_amount"), 499.0);
    CHECK_EQ(str(d, "rs_currency"), std::string("USD"));
    CHECK_EQ(str(d, "rs_item_type"), std::string("pack"));
    CHECK_EQ(str(d, "rs_item_id"), std::string("gems_500"));
    CHECK_EQ(str(d, "rs_cart"), std::string("cart_1"));
    CHECK_EQ(str(d, "rs_transaction_id"), std::string("txn_9"));
}

RS_TEST(typed_purchase_omits_empty_optionals_and_accepts_a_digit_string) {
    const auto events = run([](Client& client) {
        client.track_purchase("1299", "eur", "skin", "red_cape");
    });
    REQUIRE(events.size() == 1);
    const json::Value& d = events[0].data;
    CHECK_EQ(str(d, "rs_amount"), std::string("1299"));
    CHECK_EQ(str(d, "rs_currency"), std::string("EUR"));
    CHECK(!has(d, "rs_cart"));
    CHECK(!has(d, "rs_transaction_id"));
}

RS_TEST(typed_purchase_passes_bad_money_through_unvalidated) {
    const auto events = run([](Client& client) {
        CHECK(client.track_purchase(4.99, "us", "pack", "x"));
    });
    REQUIRE(events.size() == 1);
    CHECK_EQ(num(events[0].data, "rs_amount"), 4.99);
    CHECK_EQ(str(events[0].data, "rs_currency"), std::string("US"));
}

RS_TEST(typed_resource_names_default_by_flow) {
    const auto events = run([](Client& client) {
        client.track_resource("source", "gold", 25.0, "quest", "q7");
        client.track_resource("sink", "gold", 10.5);
    });
    REQUIRE(events.size() == 2);
    CHECK_EQ(events[0].name, std::string("resource_source"));
    CHECK_EQ(str(events[0].data, "rs_kind"), std::string("resource"));
    CHECK_EQ(str(events[0].data, "rs_flow"), std::string("source"));
    CHECK_EQ(str(events[0].data, "rs_resource"), std::string("gold"));
    CHECK_EQ(num(events[0].data, "rs_amount"), 25.0);
    CHECK_EQ(str(events[0].data, "rs_item_type"), std::string("quest"));
    CHECK_EQ(str(events[0].data, "rs_item_id"), std::string("q7"));
    CHECK_EQ(events[1].name, std::string("resource_sink"));
    CHECK_EQ(num(events[1].data, "rs_amount"), 10.5);
    CHECK(!has(events[1].data, "rs_item_type"));
    CHECK(!has(events[1].data, "rs_item_id"));
}

RS_TEST(typed_progression_sends_the_exact_property_set) {
    const auto events = run([](Client& client) {
        client.track_progression("start", "world1");
        client.track_progression("complete", "world1", "level3", "boss", 1250.5);
        client.track_progression("fail", "world1", "level3");
    });
    REQUIRE(events.size() == 3);
    CHECK_EQ(events[0].name, std::string("progression_start"));
    CHECK_EQ(str(events[0].data, "rs_kind"), std::string("progression"));
    CHECK_EQ(str(events[0].data, "rs_status"), std::string("start"));
    CHECK_EQ(str(events[0].data, "rs_p1"), std::string("world1"));
    CHECK(!has(events[0].data, "rs_p2"));
    CHECK(!has(events[0].data, "rs_p3"));
    CHECK(!has(events[0].data, "rs_score"));

    CHECK_EQ(events[1].name, std::string("progression_complete"));
    CHECK_EQ(str(events[1].data, "rs_p2"), std::string("level3"));
    CHECK_EQ(str(events[1].data, "rs_p3"), std::string("boss"));
    CHECK_EQ(num(events[1].data, "rs_score"), 1250.5);

    CHECK_EQ(events[2].name, std::string("progression_fail"));
    CHECK(!has(events[2].data, "rs_p3"));
}

RS_TEST(typed_error_sends_the_property_set_and_cuts_message_and_stack) {
    const std::string long_message(450, 'm');
    const std::string long_stack(5000, 's');
    const auto events = run([&](Client& client) {
        client.track_error("warning", "short", "at main()");
        client.track_error("critical", long_message, long_stack);
        client.track_error("info", "no stack");
    });
    REQUIRE(events.size() == 3);
    CHECK_EQ(events[0].name, std::string("error"));
    CHECK_EQ(str(events[0].data, "rs_kind"), std::string("error"));
    CHECK_EQ(str(events[0].data, "rs_severity"), std::string("warning"));
    CHECK_EQ(str(events[0].data, "rs_message"), std::string("short"));
    CHECK_EQ(str(events[0].data, "rs_stack"), std::string("at main()"));
    CHECK_EQ(str(events[1].data, "rs_message").size(), std::size_t(200));
    CHECK_EQ(str(events[1].data, "rs_stack").size(), std::size_t(2000));
    CHECK(!has(events[2].data, "rs_stack"));
}

RS_TEST(typed_error_cut_never_splits_a_multibyte_character) {
    // 300 two byte characters: the cut keeps 200 whole characters.
    std::string message;
    for (int i = 0; i < 300; ++i) {
        message += "\xC3\xA9";
    }
    const auto events = run([&](Client& client) { client.track_error("error", message); });
    REQUIRE(events.size() == 1);
    CHECK_EQ(str(events[0].data, "rs_message").size(), std::size_t(400));
}

RS_TEST(typed_reserved_keys_win_over_extra_and_extra_is_kept) {
    const auto events = run([](Client& client) {
        client.track_purchase(100, "usd", "pack", "p1", "", "",
                              {{"level", 7}, {"rs_kind", "none"}, {"rs_amount", 1}, {"rs_cart", "mine"}});
    });
    REQUIRE(events.size() == 1);
    const json::Value& d = events[0].data;
    CHECK_EQ(str(d, "rs_kind"), std::string("purchase"));
    CHECK_EQ(num(d, "rs_amount"), 100.0);
    CHECK_EQ(num(d, "level"), 7.0);
    // The helper did not set a cart, so the caller's own key is untouched.
    CHECK_EQ(str(d, "rs_cart"), std::string("mine"));
}

RS_TEST(typed_event_name_override_applies_to_every_helper) {
    const auto events = run([](Client& client) {
        client.track_purchase(1, "usd", "a", "b", "", "", {}, "iap");
        client.track_resource("sink", "gem", 1, "", "", {}, "spend");
        client.track_progression("start", "w", "", "", std::nullopt, {}, "begin");
        client.track_error("error", "boom", "", {}, "crash");
    });
    REQUIRE(events.size() == 4);
    CHECK_EQ(events[0].name, std::string("iap"));
    CHECK_EQ(events[1].name, std::string("spend"));
    CHECK_EQ(events[2].name, std::string("begin"));
    CHECK_EQ(events[3].name, std::string("crash"));
}

RS_TEST(typed_helpers_return_what_track_returns) {
    auto transport = std::make_shared<RecordingTransport>();
    Config config = test_config();
    config.enabled = false;
    Client client(config, std::make_unique<ProxyTransport>(transport));
    CHECK(!client.track_purchase(1, "usd", "a", "b"));
    CHECK(!client.track_resource("sink", "gem", 1));
    CHECK(!client.track_progression("start", "w"));
    CHECK(!client.track_error("error", "x"));
    client.shutdown();
    CHECK(delivered(*transport).empty());
}

RS_TEST(session_request_names_the_sdk) {
    std::shared_ptr<RecordingTransport> transport;
    run(test_config(), [](Client& client) { client.track("x"); }, &transport);
    bool seen = false;
    for (const Request& request : transport->requests()) {
        if (ends_with(request.url, "/session")) {
            seen = true;
            const json::Value body = json::Value::parse(request.body);
            CHECK_EQ(body.get_string("sdk", ""), std::string("cpp/") + RAVENSIGHT_VERSION);
            CHECK_EQ(std::string(RAVENSIGHT_VERSION), std::string("0.3.0"));
        }
    }
    CHECK(seen);
}

RS_TEST(health_sampler_percentiles_from_a_known_sample_set) {
    // Each pair is one sample: a nearly full second, then the frame that
    // crosses the line and is the one measured.
    auto feed = [](HealthSampler& sampler, const std::vector<double>& fps) {
        for (double f : fps) {
            sampler.add_frame(0.9999);
            sampler.add_frame(1.0 / f);
        }
    };
    double median = 0.0;
    double p10 = 0.0;

    HealthSampler ten;
    feed(ten, {100, 90, 80, 70, 60, 50, 40, 30, 20, 10});
    CHECK_EQ(ten.count(), std::size_t(10));
    REQUIRE(ten.summarize(median, p10));
    CHECK(median > 54.99 && median < 55.01);
    CHECK(p10 > 9.99 && p10 < 10.01);

    HealthSampler five;
    feed(five, {50, 10, 30, 20, 40});
    REQUIRE(five.summarize(median, p10));
    CHECK(median > 29.99 && median < 30.01);
    CHECK(p10 > 9.99 && p10 < 10.01);

    HealthSampler twenty;
    std::vector<double> many;
    for (int i = 1; i <= 20; ++i) {
        many.push_back(5.0 * i);
    }
    feed(twenty, many);
    REQUIRE(twenty.summarize(median, p10));
    CHECK(median > 52.49 && median < 52.51);
    CHECK(p10 > 9.99 && p10 < 10.01);
}

RS_TEST(health_sampler_takes_at_most_one_sample_per_second) {
    HealthSampler sampler;
    for (int i = 0; i < 8; ++i) {
        sampler.add_frame(0.25);
    }
    CHECK_EQ(sampler.count(), std::size_t(2));
    // Bad frame times are ignored.
    HealthSampler bad;
    bad.add_frame(0.0);
    bad.add_frame(-1.0);
    bad.add_frame(std::nan(""));
    for (int i = 0; i < 3; ++i) {
        bad.add_frame(0.25);
    }
    CHECK_EQ(bad.count(), std::size_t(0));
}

RS_TEST(health_sampler_keeps_at_most_600_samples) {
    HealthSampler sampler;
    for (int i = 0; i < 2500; ++i) {
        sampler.add_frame(1.0);
    }
    CHECK_EQ(sampler.count(), HealthSampler::max_samples);
    double median = 0.0;
    double p10 = 0.0;
    REQUIRE(sampler.summarize(median, p10));
    CHECK_EQ(median, 1.0);
}

RS_TEST(health_sampler_with_no_samples_has_no_summary) {
    HealthSampler sampler;
    double median = 0.0;
    double p10 = 0.0;
    CHECK(!sampler.summarize(median, p10));
}

RS_TEST(health_summary_is_sent_once_from_shutdown) {
    Config config = test_config();
    config.performance = true;
    const auto events = run(config, [](Client& client) {
        client.sample_frame(0.9999);
        client.sample_frame(1.0 / 60.0);
        client.sample_frame(0.9999);
        client.sample_frame(1.0 / 30.0);
        client.sample_frame(0.9999);
        client.sample_frame(1.0 / 45.0);
        client.flush_and_wait(1.0);
        // Nothing goes out while the game runs.
    });
    int health = 0;
    for (const Sent& sent : events) {
        if (sent.name != "health") {
            continue;
        }
        ++health;
        CHECK_EQ(str(sent.data, "rs_kind"), std::string("health"));
        CHECK(num(sent.data, "rs_fps_median") > 44.99 && num(sent.data, "rs_fps_median") < 45.01);
        CHECK(num(sent.data, "rs_fps_p10") > 29.99 && num(sent.data, "rs_fps_p10") < 30.01);
        CHECK(!has(sent.data, "rs_fps_p50"));
        if (has(sent.data, "rs_memory_mb")) {
            CHECK(num(sent.data, "rs_memory_mb") > 0.0);
        }
        if (has(sent.data, "rs_boot_ms")) {
            CHECK(num(sent.data, "rs_boot_ms") >= 0.0);
        }
    }
    CHECK_EQ(health, 1);
}

RS_TEST(health_is_not_sent_by_a_second_shutdown_or_without_samples_or_opt_in) {
    // Idempotent shutdown: still exactly one.
    {
        Config config = test_config();
        config.performance = true;
        auto transport = std::make_shared<RecordingTransport>();
        Client client(config, std::make_unique<ProxyTransport>(transport));
        client.sample_frame(2.0);
        client.shutdown();
        client.shutdown();
        int health = 0;
        for (const Sent& sent : delivered(*transport)) {
            health += sent.name == "health" ? 1 : 0;
        }
        CHECK_EQ(health, 1);
    }
    // Opted in, no sample taken: nothing.
    {
        Config config = test_config();
        config.performance = true;
        const auto events = run(config, [](Client& client) { client.sample_frame(0.1); });
        CHECK(events.empty());
    }
    // Not opted in: samples are ignored.
    {
        const auto events = run([](Client& client) {
            client.sample_frame(2.0);
            client.sample_frame(2.0);
        });
        CHECK(events.empty());
    }
}

RS_TEST(health_is_not_queued_for_a_player_who_opted_out) {
    Config config = test_config();
    config.performance = true;
    config.enabled = false;
    const auto events = run(config, [](Client& client) { client.sample_frame(2.0); });
    CHECK(events.empty());
}
