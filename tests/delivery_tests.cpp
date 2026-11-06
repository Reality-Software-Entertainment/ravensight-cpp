// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The pure pieces of the delivery and ads contracts, driven from the
// shared vectors (tests/fixtures/delivery-vectors.json, a copy of the
// server's docs/sdk/delivery-vectors.json).

#include <deque>
#include <stdexcept>
#include <string>

#include "delivery.hpp"
#include "ravensight/client.hpp"
#include "storage.hpp"
#include "temp_dirs.hpp"
#include "test_runner.hpp"

using namespace ravensight;

namespace {

json::Value vectors() {
    std::string text;
    CHECK(detail::read_text_file(RAVENSIGHT_VECTORS_FILE, text));
    return json::Value::parse(text);
}

/// A vector value: a string, or {repeat, count} expanded.
std::string expand(const json::Value& value) {
    if (value.is_string()) return value.as_string();
    std::string out;
    double count = 0;
    value.get_number("count", count);
    const std::string unit = value.get_string("repeat", "");
    for (int i = 0; i < static_cast<int>(count); ++i) out += unit;
    return out;
}

EventData expand_data(const json::Value& data) {
    EventData out;
    for (const auto& member : data.members()) out.emplace_back(member.first, expand(member.second));
    return out;
}

Event event_at(const std::string& name, long long ts, const std::string& origin = "") {
    Event event;
    event.name = name;
    event.timestamp = ts;
    event.data_json = "{\"rs_event_id\":\"" + name + "\"}";
    event.origin_json = origin;
    return event;
}

const std::string origin_a =
    "{\"session\":\"ses_a\",\"build\":\"1.2.0\",\"platform\":\"Windows\",\"stamp\":\"o1.x.y\"}";

} // namespace

RS_TEST(signing_header_vectors) {
    const json::Value v = vectors();
    const json::Value* signing = v.find("signing");
    REQUIRE(signing != nullptr);
    const std::string secret = signing->get_string("secret", "");
    int checked = 0;
    for (const json::Value& c : signing->find("header")->items()) {
        double t = 0;
        c.get_number("timestamp", t);
        CHECK_EQ(detail::signature_header(secret, static_cast<long long>(t), c.get_string("body", "")),
                 c.get_string("header", "?"));
        ++checked;
    }
    CHECK_EQ(checked, 4);
}

RS_TEST(signing_beacon_vector) {
    const json::Value v = vectors();
    const json::Value* signing = v.find("signing");
    const std::string secret = signing->get_string("secret", "");
    for (const json::Value& c : signing->find("beacon")->items()) {
        double t = 0;
        c.get_number("t", t);
        const json::Value built = json::Value::parse(detail::beacon_envelope(
            "SESSION_TOKEN", secret, static_cast<long long>(t), c.get_string("inner", "")));
        CHECK_EQ(built.dump(), c.find("envelope")->dump());
    }
}

RS_TEST(any_key_event_size_vectors) {
    const json::Value v = vectors();
    int checked = 0;
    for (const json::Value& c : v.find("anyKeyEventSize")->find("cases")->items()) {
        const std::string data = write_fields(expand_data(*c.find("data")));
        const bool too_large = detail::counted_event_bytes(data) > limits::max_event_bytes;
        const bool expect_refused = c.find("expect")->is_string();
        if (!CHECK_EQ(too_large, expect_refused)) {
            std::printf("    case: %s\n", c.get_string("name", "").c_str());
        }
        ++checked;
    }
    CHECK_EQ(checked, 6);
}

RS_TEST(cut_utf16_never_splits_a_character) {
    CHECK_EQ(detail::cut_utf16(std::string(2001, 's'), 2000).size(), 2000u);
    // A four-byte character counts two units and is never split.
    std::string emoji;
    for (int i = 0; i < 3; ++i) emoji += "\xF0\x9F\x98\x80";
    CHECK_EQ(detail::cut_utf16(emoji, 5), std::string("\xF0\x9F\x98\x80\xF0\x9F\x98\x80"));
}

RS_TEST(queue_file_name_is_fnv1a_of_the_key) {
    CHECK_EQ(detail::fnv1a64_hex("gt_pub_test"), std::string("e0347efbebb7d40b"));
    CHECK_EQ(detail::queue_file_name("gt_pub_test"), std::string("queue-e0347efbebb7d40b.json"));
}

RS_TEST(queue_file_round_trips_groups_and_origins) {
    std::deque<Event> events{event_at("a", 1000, origin_a), event_at("b", 2000, origin_a),
                             event_at("c", 3000)};
    const std::string text = detail::write_queue_file(events);
    const json::Value parsed = json::Value::parse(text);
    double version = 0;
    parsed.get_number("version", version);
    CHECK_EQ(version, 2.0);
    CHECK_EQ(parsed.find("groups")->size(), 2u);
    CHECK(text.find("sessionToken") == std::string::npos);
    const detail::LoadedQueue loaded = detail::read_queue_file(text, 5000, 500);
    REQUIRE(loaded.events.size() == 3);
    CHECK_EQ(loaded.events[0].origin_json, json::Value::parse(origin_a).dump());
    CHECK_EQ(loaded.events[1].timestamp, 2000LL);
    CHECK(loaded.events[2].origin_json.empty());
    CHECK_EQ(loaded.events[2].data_json, std::string("{\"rs_event_id\":\"c\"}"));
}

RS_TEST(queue_file_reads_version_1_and_refuses_junk) {
    const std::string v1 =
        "{\"version\":1,\"groups\":[{\"sessionToken\":\"t\",\"events\":[{\"event\":\"x\","
        "\"timestamp\":10,\"data\":{}}]}]}";
    const detail::LoadedQueue loaded = detail::read_queue_file(v1, 20, 500);
    REQUIRE(loaded.events.size() == 1);
    CHECK(loaded.events[0].origin_json.empty());
    CHECK(detail::read_queue_file("not json", 0, 500).events.empty());
    CHECK(detail::read_queue_file("{\"version\":3,\"groups\":[]}", 0, 500).events.empty());
    CHECK(detail::read_queue_file("", 0, 500).events.empty());
}

RS_TEST(queue_file_drops_events_past_89_days_and_the_oldest_past_the_cap) {
    const long long day = 24LL * 60 * 60 * 1000;
    const long long now = 100 * day;
    std::deque<Event> events{event_at("old", now - 90 * day), event_at("edge", now - 89 * day),
                             event_at("a", now - 2), event_at("b", now - 1)};
    const detail::LoadedQueue loaded = detail::read_queue_file(detail::write_queue_file(events), now, 2);
    CHECK_EQ(loaded.too_old, 1);
    CHECK_EQ(loaded.over_cap, 1);
    REQUIRE(loaded.events.size() == 2);
    CHECK_EQ(loaded.events[0].name, std::string("a"));
}

RS_TEST(gzip_round_trips) {
    if (!detail::gzip_available()) {
        std::printf("  (zlib not found: bodies are sent uncompressed)\n");
        return;
    }
    const std::string body(5000, 'x');
    const std::string packed = detail::gzip_compress(body);
    CHECK(!packed.empty());
    CHECK(packed.size() < body.size());
    CHECK_EQ(static_cast<unsigned char>(packed[0]), 0x1fu);
    CHECK_EQ(detail::gzip_decompress(packed), body);
}

RS_TEST(micros_from_decimal_text_rounds_half_up_on_the_digits) {
    CHECK_EQ(micros_from_decimal_string("0.0015"), std::string("1500"));
    CHECK_EQ(micros_from_decimal_string("1.23456789"), std::string("1234568"));
    CHECK_EQ(micros_from_decimal_string("1.2345675"), std::string("1234568"));
    CHECK_EQ(micros_from_decimal_string("1.2345674999"), std::string("1234567"));
    CHECK_EQ(micros_from_decimal_string("0.9999995"), std::string("1000000"));
    CHECK_EQ(micros_from_decimal_string("12"), std::string("12000000"));
    CHECK_EQ(micros_from_decimal_string("0"), std::string("0"));
    CHECK_EQ(micros_from_decimal_string("-1"), std::string(""));
    CHECK_EQ(micros_from_decimal_string("1e-3"), std::string(""));
    CHECK_EQ(micros_from_decimal_string("1."), std::string(""));
    CHECK_EQ(micros_from_decimal_string("abc"), std::string(""));
    CHECK_EQ(micros_from_decimal_string("1234567890123.5"), std::string(""));
}

RS_TEST(micros_from_a_mediation_double_never_multiplies) {
    CHECK_EQ(micros_from_double(0.0015), std::string("1500"));
    CHECK_EQ(micros_from_double(1.23456789), std::string("1234568"));
    CHECK_EQ(micros_from_double(0.000001), std::string("1"));
    CHECK_EQ(micros_from_double(0.0000005), std::string("1"));
    CHECK_EQ(micros_from_double(2.5), std::string("2500000"));
    CHECK_EQ(micros_from_double(0.0), std::string("0"));
    CHECK_EQ(micros_from_double(-1.0), std::string(""));
}

RS_TEST(mediation_formats_map_to_the_server_list) {
    CHECK_EQ(map_ad_format("BANNER"), std::string("banner"));
    CHECK_EQ(map_ad_format("MREC"), std::string("banner"));
    CHECK_EQ(map_ad_format("LEADER"), std::string("banner"));
    CHECK_EQ(map_ad_format("INTER"), std::string("interstitial"));
    CHECK_EQ(map_ad_format("interstitial"), std::string("interstitial"));
    CHECK_EQ(map_ad_format("REWARDED"), std::string("rewarded"));
    CHECK_EQ(map_ad_format("rewarded_video"), std::string("rewarded"));
    CHECK_EQ(map_ad_format("Rewarded Video"), std::string("rewarded"));
    CHECK_EQ(map_ad_format("REWARDED_INTER"), std::string("rewarded"));
    CHECK_EQ(map_ad_format("NATIVE"), std::string("native"));
    CHECK_EQ(map_ad_format("APP_OPEN"), std::string("app_open"));
    CHECK_EQ(map_ad_format("CAROUSEL"), std::string("other"));
}

RS_TEST(mediation_adapters_build_shown_events) {
    const AdEvent admob = ad_from_admob(1500, "usd", "", "rewarded", "level_end");
    CHECK_EQ(admob.network, std::string("admob"));
    CHECK_EQ(admob.revenue_micros, std::string("1500"));
    CHECK_EQ(admob.currency, std::string("USD"));
    CHECK_EQ(admob.result, std::string("shown"));
    const AdEvent max = ad_from_applovin_max(0.0015, "AppLovin", "INTER", "menu");
    CHECK_EQ(max.revenue_micros, std::string("1500"));
    CHECK_EQ(max.currency, std::string("USD"));
    CHECK_EQ(max.format, std::string("interstitial"));
    const AdEvent iron = ad_from_ironsource(1.23456789, "ironSource", "Rewarded Video", "p");
    CHECK_EQ(iron.revenue_micros, std::string("1234568"));
    CHECK_EQ(iron.format, std::string("rewarded"));
    const AdEvent topon = ad_from_topon(0.02, "CNY", "Pangle", "BANNER", "home");
    CHECK_EQ(topon.revenue_micros, std::string("20000"));
    CHECK_EQ(topon.currency, std::string("CNY"));
}

RS_TEST(device_fields_are_printable_trimmed_and_cut) {
    CHECK_EQ(detail::clean_device_field("  Pixel 7 \n"), std::string("Pixel 7"));
    CHECK_EQ(detail::clean_device_field("a\x01" "b"), std::string("ab"));
    CHECK_EQ(detail::clean_device_field(std::string(300, 'g')).size(), 128u);
}

RS_TEST(queue_file_keeps_the_automatic_error_flag) {
    std::deque<Event> events{event_at("a", 1000)};
    Event error = event_at("err", 1001);
    error.automatic_error = true;
    events.push_back(error);
    const detail::LoadedQueue loaded = detail::read_queue_file(detail::write_queue_file(events), 2000, 500);
    REQUIRE(loaded.events.size() == 2);
    CHECK(!loaded.events[0].automatic_error);
    CHECK(loaded.events[1].automatic_error);
}
