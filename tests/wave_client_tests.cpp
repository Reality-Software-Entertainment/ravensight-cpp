// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The November contracts through a whole Client and a fake transport: the
// persisted queue and origins (delivery 2, 3), the response table (4.1),
// gzip (4.3), signing (7), exposure tagging, ads and attribution, device
// fields.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "decoded.hpp"
#include "delivery.hpp"
#include "ravensight/client.hpp"
#include "storage.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;
namespace fs = std::filesystem;

namespace {

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string header(const Request& request, const std::string& name) {
    for (const auto& entry : request.headers) {
        if (entry.first == name) return entry.second;
    }
    return "";
}

Response reply(long status, const std::string& body, const std::string& retry_after = "") {
    Response response;
    response.status = status;
    response.body = body;
    response.retry_after = retry_after;
    return response;
}

std::string session_body(const std::string& session, const std::string& extra = "") {
    return "{\"token\":\"tok_" + session + "\",\"expiresIn\":3600,\"origin\":{\"session\":\"" + session +
           "\",\"build\":\"2.0.0\",\"platform\":\"test\",\"stamp\":\"o1.a.b\"}" + extra + "}";
}

/// Records decoded requests (the raw ones too) and answers with a script.
struct Server {
    std::mutex mutex;
    std::vector<Request> requests;
    std::vector<Request> raw;
    std::function<Response(const Request&)> respond;
    std::string session = "ses_one";
    std::string session_extra;
    std::string config = "{\"values\":{\"speed\":2},\"experiments\":[],\"etag\":\"\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\"\"}";

    Response handle(const Request& wire) {
        const Request request = rstest::decoded(wire);
        std::function<Response(const Request&)> custom;
        {
            std::lock_guard<std::mutex> lock(mutex);
            requests.push_back(request);
            raw.push_back(wire);
            custom = respond;
        }
        if (custom) {
            Response response = custom(request);
            if (response.status != -1) return response;
        }
        if (ends_with(request.url, "/settings")) return reply(200, "{\"trackingEnabled\":true}");
        if (ends_with(request.url, "/session")) {
            std::lock_guard<std::mutex> lock(mutex);
            return reply(201, session_body(session, session_extra));
        }
        if (request.url.find("/ingest/config") != std::string::npos) {
            std::lock_guard<std::mutex> lock(mutex);
            return reply(200, config);
        }
        if (ends_with(request.url, "/track/batch")) return reply(202, "{\"accepted\":1}");
        return reply(404, "{}");
    }

    /// Every event object sent on /track/batch, in order.
    std::vector<json::Value> events() {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<json::Value> out;
        for (const Request& request : requests) {
            if (!ends_with(request.url, "/track/batch")) continue;
            const json::Value body = json::Value::parse(request.body);
            const json::Value* list = body.find("events");
            if (list == nullptr) continue;
            for (const json::Value& event : list->items()) out.push_back(event);
        }
        return out;
    }

    std::vector<Request> to(const std::string& suffix) {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Request> out;
        for (const Request& request : requests) {
            if (request.url.find(suffix) != std::string::npos) out.push_back(request);
        }
        return out;
    }
};

class Wire : public Transport {
public:
    explicit Wire(std::shared_ptr<Server> server) : server_(std::move(server)) {}
    Response send(const Request& request) override { return server_->handle(request); }

private:
    std::shared_ptr<Server> server_;
};

const Response pass = reply(-1, "");

Config wave_config(const std::string& dir) {
    Config config;
    config.ingest_key = "gt_pub_wave";
    config.api_url = "https://api.example.test";
    config.game_version = "2.0.0";
    config.platform = "test";
    config.storage_dir = dir;
    config.flush_interval_seconds = 0;
    config.track_lifecycle_events = false;
    config.log = [](const std::string&) {};
    return config;
}

std::string queue_path(const Config& config) {
    return detail::join_path(config.storage_dir, detail::queue_file_name(config.ingest_key));
}

bool wait_for(const std::function<bool()>& done, double seconds = 3.0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (std::chrono::steady_clock::now() < until) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

json::Value data_of(const json::Value& event) {
    const json::Value* data = event.find("data");
    return data != nullptr ? *data : json::Value();
}

} // namespace

// --- Persisted queue and origins -----------------------------------------------

RS_TEST(an_offline_run_is_sent_next_launch_under_its_own_origin) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto first = std::make_shared<Server>();
    first->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(0, "") : pass;
    };
    long long before_ms = 0;
    {
        Client client(config, std::make_unique<Wire>(first));
        before_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
        CHECK(client.track("pickup", {{"coins", 3}}));
        client.flush_and_wait(2.0);
        CHECK(client.track("death"));
    }
    std::string saved;
    REQUIRE(detail::read_text_file(queue_path(config), saved));
    const json::Value file = json::Value::parse(saved);
    double version = 0;
    file.get_number("version", version);
    CHECK_EQ(version, 2.0);
    CHECK(saved.find("ses_one") != std::string::npos);
    CHECK(saved.find("tok_ses_one") == std::string::npos); // no token, no secret

    auto second = std::make_shared<Server>();
    second->session = "ses_two";
    {
        Client client(config, std::make_unique<Wire>(second));
        CHECK(client.track("next_run"));
        CHECK(client.flush_and_wait(3.0));
    }
    const std::vector<json::Value> events = second->events();
    REQUIRE(events.size() == 3);
    CHECK_EQ(events[0].get_string("event", ""), std::string("pickup"));
    const json::Value* origin = events[0].find("origin");
    REQUIRE(origin != nullptr);
    CHECK_EQ(origin->get_string("session", ""), std::string("ses_one"));
    CHECK_EQ(origin->get_string("stamp", ""), std::string("o1.a.b"));
    CHECK(events[1].find("origin") != nullptr);
    CHECK(events[2].find("origin") == nullptr); // tracked in the current session
    double ts = 0;
    events[0].get_number("timestamp", ts);
    CHECK(ts >= static_cast<double>(before_ms)); // milliseconds, from when it was tracked
    CHECK(!data_of(events[0]).get_string("rs_event_id", "").empty());
    // Delivered: the file is gone, so a third launch sends nothing again.
    CHECK(!fs::exists(queue_path(config)));
}

RS_TEST(the_same_event_ids_are_resent_after_a_lost_answer) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto first = std::make_shared<Server>();
    first->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(503, "{}") : pass;
    };
    {
        Client client(config, std::make_unique<Wire>(first));
        client.track("a");
        client.flush_and_wait(2.0);
    }
    auto second = std::make_shared<Server>();
    {
        Client client(config, std::make_unique<Wire>(second));
        CHECK(client.flush_and_wait(3.0));
    }
    REQUIRE(!first->events().empty());
    REQUIRE(second->events().size() == 1);
    for (const json::Value& attempt : first->events()) {
        CHECK_EQ(data_of(attempt).get_string("rs_event_id", "1"),
                 data_of(second->events()[0]).get_string("rs_event_id", "2"));
    }
}

RS_TEST(persist_queue_off_writes_no_queue_file) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    config.persist_queue = false;
    auto server = std::make_shared<Server>();
    server->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(0, "") : pass;
    };
    {
        Client client(config, std::make_unique<Wire>(server));
        client.track("a");
        client.flush_and_wait(1.0);
    }
    CHECK(!fs::exists(queue_path(config)));
}

RS_TEST(opting_out_deletes_the_queue_file) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    server->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(0, "") : pass;
    };
    Client client(config, std::make_unique<Wire>(server));
    client.track("a");
    client.flush_and_wait(1.0);
    REQUIRE(wait_for([&] { return fs::exists(queue_path(config)); }));
    client.set_tracking_enabled(false);
    client.shutdown();
    CHECK(!fs::exists(queue_path(config)));
}

RS_TEST(rule_3_a_device_id_reset_deletes_the_queue_file) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    server->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(0, "") : pass;
    };
    Client client(config, std::make_unique<Wire>(server));
    client.track("a");
    client.flush_and_wait(1.0);
    REQUIRE(wait_for([&] { return fs::exists(queue_path(config)); }));
    client.reset_device_id();
    CHECK(!fs::exists(queue_path(config)));
}

// --- Response table --------------------------------------------------------------

RS_TEST(a_403_keeps_the_events_for_the_next_launch) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    server->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(403, "{\"error\":\"version_refused\"}") : pass;
    };
    {
        Client client(config, std::make_unique<Wire>(server));
        client.track("kept");
        client.flush_and_wait(2.0);
        CHECK(!client.tracking_enabled());
    }
    std::string saved;
    REQUIRE(detail::read_text_file(queue_path(config), saved));
    CHECK(saved.find("kept") != std::string::npos);
}

RS_TEST(a_401_reopens_the_session_once_then_backs_off) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    server->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(401, "{\"error\":\"signature_invalid\"}") : pass;
    };
    Client client(config, std::make_unique<Wire>(server));
    client.track("a");
    client.flush_and_wait(1.0);
    CHECK_EQ(server->to("/session").size(), std::size_t(2));
    CHECK_EQ(server->to("/track/batch").size(), std::size_t(2));
    CHECK_EQ(client.stats().queued, 1);
}

RS_TEST(a_400_without_an_index_drops_the_batch_and_too_large_splits) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    server->respond = [](const Request& request) {
        if (!ends_with(request.url, "/track/batch")) return pass;
        const json::Value body = json::Value::parse(request.body);
        const std::size_t count = body.find("events")->size();
        if (count > 2) return reply(400, "{\"error\":\"batch_too_large\"}");
        if (body.find("events")->at(0).get_string("event", "") == "bad") {
            return reply(400, "{\"error\":\"invalid_json\"}");
        }
        return reply(202, "{}");
    };
    Client client(config, std::make_unique<Wire>(server));
    client.track("bad");
    client.track("bad_two");
    client.track("c");
    client.track("d");
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(client.stats().sent, 2);
    CHECK_EQ(client.stats().dropped, 2);
}

RS_TEST(a_413_splits_like_batch_too_large) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    server->respond = [](const Request& request) {
        if (!ends_with(request.url, "/track/batch")) return pass;
        return json::Value::parse(request.body).find("events")->size() > 1 ? reply(413, "{}")
                                                                            : reply(202, "{}");
    };
    Client client(config, std::make_unique<Wire>(server));
    for (int i = 0; i < 4; ++i) client.track("e");
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(client.stats().sent, 4);
}

RS_TEST(retry_after_is_waited_exactly_even_by_flush) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    std::atomic<int> calls{0};
    server->respond = [&](const Request& request) {
        if (!ends_with(request.url, "/track/batch")) return pass;
        return calls.fetch_add(1) == 0 ? reply(429, "{}", "1") : reply(202, "{}");
    };
    Client client(config, std::make_unique<Wire>(server));
    client.track("a");
    const auto start = std::chrono::steady_clock::now();
    client.flush_and_wait(0.4);
    client.flush_and_wait(0.3); // a flush inside the wait sends nothing
    CHECK_EQ(calls.load(), 1);
    std::this_thread::sleep_until(start + std::chrono::milliseconds(1150));
    CHECK(client.flush_and_wait(2.0));
    CHECK_EQ(calls.load(), 2);
}

// --- Compression and signing -------------------------------------------------------

RS_TEST(bodies_of_1kb_or_more_are_gzipped_and_signed_uncompressed) {
    if (!detail::gzip_available()) return;
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    const std::string secret = "dGVzdC1zZWNyZXQtZm9yLWRlbGl2ZXJ5LWNvbnRyYWN0";
    const long long server_time =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count() + 1000;
    server->session_extra = ",\"signing\":{\"algorithm\":\"HMAC-SHA256\",\"version\":1,\"secret\":\"" +
                            secret + "\",\"serverTime\":" + std::to_string(server_time) + "}";
    Client client(config, std::make_unique<Wire>(server));
    client.track("small");
    CHECK(client.flush_and_wait(3.0));
    client.track("big", {{"blob", std::string(1500, 'x')}});
    CHECK(client.flush_and_wait(3.0));
    std::vector<Request> raw;
    {
        std::lock_guard<std::mutex> lock(server->mutex);
        for (const Request& request : server->raw) {
            if (ends_with(request.url, "/track/batch")) raw.push_back(request);
        }
    }
    REQUIRE(raw.size() == 2);
    CHECK(header(raw[0], "Content-Encoding").empty());
    CHECK_EQ(header(raw[1], "Content-Encoding"), std::string("gzip"));
    for (const Request& request : raw) {
        const std::string plain = rstest::decoded(request).body;
        const std::string ts = header(request, "X-Ravensight-Timestamp");
        REQUIRE(!ts.empty());
        // The device clock corrected by the server's offset.
        CHECK(std::llabs(std::atoll(ts.c_str()) - server_time) < 5);
        CHECK_EQ(header(request, "X-Ravensight-Signature"),
                 detail::signature_header(secret, std::atoll(ts.c_str()), plain));
    }
}

RS_TEST(a_session_without_signing_sends_no_signature) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    Client client(config, std::make_unique<Wire>(server));
    client.track("a");
    CHECK(client.flush_and_wait(3.0));
    REQUIRE(!server->to("/track/batch").empty());
    CHECK(header(server->to("/track/batch")[0], "X-Ravensight-Signature").empty());
}

RS_TEST(a_415_on_gzip_resends_uncompressed) {
    if (!detail::gzip_available()) return;
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    // The decoded copy keeps the header, so the responder can see it.
    server->respond = [](const Request& request) {
        if (!ends_with(request.url, "/track/batch")) return pass;
        return header(request, "Content-Encoding") == "gzip" ? reply(415, "{}") : reply(202, "{}");
    };
    Client client(config, std::make_unique<Wire>(server));
    client.track("big", {{"blob", std::string(2000, 'x')}});
    CHECK(client.flush_and_wait(3.0));
    CHECK_EQ(client.stats().sent, 1);
    CHECK_EQ(server->to("/track/batch").size(), std::size_t(2));
}

// --- Exposure ----------------------------------------------------------------------

RS_TEST(exposure_tags_follow_the_first_typed_read) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    std::atomic<int> ready{0};
    config.on_config_ready = [&](bool) { ++ready; };
    auto server = std::make_shared<Server>();
    server->config = "{\"values\":{\"speed\":2},\"experiments\":[{\"key\":\"difficulty\",\"variant\":\"easy\"},"
                     "{\"key\":\"shop\",\"variant\":\"b\"}],\"etag\":\"\\\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\\\"\"}";
    {
        Client client(config, std::make_unique<Wire>(server));
        client.track("first");
        CHECK(client.flush_and_wait(3.0)); // opens the session, which fetches config
        REQUIRE(wait_for([&] { return ready.load() >= 1; }));
        client.track("fetched_not_read");
        CHECK_EQ(client.get_config_number("speed", 0), 2.0);
        client.track("after_read", {{"rs_exp_shop", "game_value"}});
        CHECK(client.flush_and_wait(3.0));
        {
            std::lock_guard<std::mutex> lock(server->mutex);
            server->config = "{\"values\":{},\"experiments\":[{\"key\":\"shop\",\"variant\":\"b\"}],\"etag\":\"\\\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\\\"\"}";
        }
        client.fetch_config();
        REQUIRE(wait_for([&] { return ready.load() >= 2; }));
        client.track("after_stop");
        CHECK(client.flush_and_wait(3.0));
    }
    std::vector<json::Value> events = server->events();
    auto find = [&](const std::string& name) -> json::Value {
        for (const json::Value& event : events) if (event.get_string("event", "") == name) return event;
        return json::Value();
    };
    CHECK(data_of(find("first")).find("rs_exp_difficulty") == nullptr);
    CHECK(data_of(find("fetched_not_read")).find("rs_exp_difficulty") == nullptr);
    const json::Value exposure = find("rs_exposure");
    CHECK_EQ(data_of(exposure).get_string("rs_exp_difficulty", ""), std::string("easy"));
    CHECK_EQ(data_of(exposure).get_string("rs_exp_shop", ""), std::string("b"));
    int exposures = 0;
    for (const json::Value& event : events) exposures += event.get_string("event", "") == "rs_exposure";
    CHECK_EQ(exposures, 1);
    CHECK_EQ(data_of(find("after_read")).get_string("rs_exp_difficulty", ""), std::string("easy"));
    CHECK_EQ(data_of(find("after_read")).get_string("rs_exp_shop", ""), std::string("game_value"));
    CHECK(data_of(find("after_stop")).find("rs_exp_difficulty") == nullptr);
    CHECK_EQ(data_of(find("after_stop")).get_string("rs_exp_shop", ""), std::string("b"));

    // A second run starts untagged until its own first read.
    auto next = std::make_shared<Server>();
    next->config = server->config;
    {
        Client client(config, std::make_unique<Wire>(next));
        client.track("second_run");
        CHECK(client.flush_and_wait(3.0));
    }
    events = next->events();
    REQUIRE(!events.empty());
    CHECK(data_of(events[0]).find("rs_exp_shop") == nullptr);
}

RS_TEST(an_offline_event_keeps_its_original_tag) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    std::atomic<int> ready{0};
    config.on_config_ready = [&](bool) { ++ready; };
    auto server = std::make_shared<Server>();
    server->config = "{\"values\":{},\"experiments\":[{\"key\":\"difficulty\",\"variant\":\"hard\"}],\"etag\":\"\\\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\\\"\"}";
    server->respond = [](const Request& request) {
        return ends_with(request.url, "/track/batch") ? reply(0, "") : pass;
    };
    {
        Client client(config, std::make_unique<Wire>(server));
        client.track("boot");
        client.flush_and_wait(1.0);
        REQUIRE(wait_for([&] { return ready.load() >= 1; }));
        client.get_config_bool("x", false);
        client.track("offline");
    }
    auto next = std::make_shared<Server>();
    {
        Client client(config, std::make_unique<Wire>(next));
        CHECK(client.flush_and_wait(3.0));
    }
    bool found = false;
    for (const json::Value& event : next->events()) {
        if (event.get_string("event", "") == "offline") {
            found = true;
            CHECK_EQ(data_of(event).get_string("rs_exp_difficulty", ""), std::string("hard"));
        }
    }
    CHECK(found);
}

// --- Ads, attribution, device fields --------------------------------------------------

RS_TEST(ad_revenue_sends_exactly_the_seven_keys) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.track_ad_revenue("admob", "rewarded", "level_end", "1500", "USD"));
    AdEvent no_currency{"admob", "rewarded", "shown", "", "1500", ""};
    CHECK(!client.track_ad(no_currency));
    AdEvent click_with_revenue{"admob", "rewarded", "clicked", "", "1500", "USD"};
    CHECK(!client.track_ad(click_with_revenue));
    AdEvent float_revenue{"admob", "rewarded", "shown", "", "0.0015", "USD"};
    CHECK(!client.track_ad(float_revenue));
    AdEvent click{"applovin", "interstitial", "clicked", "menu", "", ""};
    CHECK(client.track_ad(click));
    CHECK(client.track_ad(ad_from_applovin_max(1.23456789, "AppLovin", "INTER", "menu")));
    CHECK(client.flush_and_wait(3.0));
    const std::vector<json::Value> events = server->events();
    REQUIRE(events.size() == 3);
    CHECK_EQ(events[0].get_string("event", ""), std::string("rs_ad"));
    const json::Value data = data_of(events[0]);
    int keys = 0;
    for (const auto& member : data.members()) keys += member.first != "rs_event_id";
    CHECK_EQ(keys, 7);
    CHECK_EQ(data.get_string("rs_kind", ""), std::string("ad"));
    CHECK_EQ(data.get_string("rs_ad_network", ""), std::string("admob"));
    CHECK_EQ(data.get_string("rs_ad_format", ""), std::string("rewarded"));
    CHECK_EQ(data.get_string("rs_ad_placement", ""), std::string("level_end"));
    CHECK_EQ(data.get_string("rs_ad_result", ""), std::string("shown"));
    CHECK_EQ(data.get_string("rs_revenue_micros", ""), std::string("1500"));
    CHECK_EQ(data.get_string("rs_currency", ""), std::string("USD"));
    CHECK(data_of(events[1]).find("rs_revenue_micros") == nullptr);
    CHECK_EQ(data_of(events[2]).get_string("rs_revenue_micros", ""), std::string("1234568"));
}

RS_TEST(attribution_is_sent_once_per_install_across_restarts) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    Attribution attribution;
    attribution.source = "store_referrer";
    attribution.campaign = "spring";
    attribution.touch_time_ms = 1767225600000LL;
    {
        Client client(config, std::make_unique<Wire>(server));
        CHECK(client.set_attribution(attribution));
        CHECK(!client.set_attribution(attribution));
        CHECK(client.flush_and_wait(3.0));
    }
    {
        Client client(config, std::make_unique<Wire>(server));
        CHECK(!client.set_attribution(attribution));
        CHECK(client.flush_and_wait(3.0));
    }
    const std::vector<json::Value> events = server->events();
    REQUIRE(events.size() == 1);
    const json::Value data = data_of(events[0]);
    CHECK_EQ(events[0].get_string("event", ""), std::string("rs_attribution"));
    CHECK_EQ(data.get_string("rs_kind", ""), std::string("attribution"));
    CHECK_EQ(data.get_string("rs_attr_source", ""), std::string("store_referrer"));
    CHECK_EQ(data.get_string("rs_campaign", ""), std::string("spring"));
    CHECK(data.find("rs_adset") == nullptr);
    double touch = 0;
    CHECK(data.get_number("rs_touch_time", touch));
    CHECK_EQ(touch, 1767225600000.0);
    Attribution empty;
    Client client(config, std::make_unique<Wire>(server));
    CHECK(!client.set_attribution(empty));
}

RS_TEST(device_fields_go_on_the_session_request) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    config.device.model = " Pixel 7 ";
    config.device.os_version = "14";
    auto server = std::make_shared<Server>();
    {
        Client client(config, std::make_unique<Wire>(server));
        client.track("a");
        CHECK(client.flush_and_wait(3.0));
    }
    const std::vector<Request> sessions = server->to("/session");
    REQUIRE(!sessions.empty());
    const json::Value body = json::Value::parse(sessions[0].body);
    const json::Value* device = body.find("device");
    REQUIRE(device != nullptr);
    CHECK_EQ(device->get_string("model", ""), std::string("Pixel 7"));
    CHECK_EQ(device->get_string("osVersion", ""), std::string("14"));
    CHECK(device->find("gpu") == nullptr);

    auto plain = std::make_shared<Server>();
    Config bare = wave_config(rstest::make_temp_dir());
    {
        Client client(bare, std::make_unique<Wire>(plain));
        client.track("a");
        CHECK(client.flush_and_wait(3.0));
    }
    REQUIRE(!plain->to("/session").empty());
    CHECK(json::Value::parse(plain->to("/session")[0].body).find("device") == nullptr);
}

RS_TEST(a_typed_error_of_control_characters_fits_the_8kb_count) {
    const std::string dir = rstest::make_temp_dir();
    Config config = wave_config(dir);
    auto server = std::make_shared<Server>();
    Client client(config, std::make_unique<Wire>(server));
    CHECK(client.track_error("error", std::string(200, '\x01'), std::string(2000, '\x01')));
    CHECK(!client.track("plain", {{"rs_stack", std::string(2000, '\x01')}}));
}
