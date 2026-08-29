// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include <cmath>
#include <string>

#include "ravensight/client.hpp"
#include "ravensight/json.hpp"
#include "ravensight/transport.hpp"
#include "test_runner.hpp"

using ravensight::json::escape_string;
using ravensight::json::ObjectWriter;
using ravensight::json::Value;
using ravensight::json::write_number;

RS_TEST(escape_plain_text_passes_through) {
    CHECK_EQ(escape_string("hello"), std::string("\"hello\""));
    CHECK_EQ(escape_string(""), std::string("\"\""));
}

RS_TEST(escape_quotes_and_backslashes) {
    CHECK_EQ(escape_string("say \"hi\""), std::string("\"say \\\"hi\\\"\""));
    CHECK_EQ(escape_string("C:\\games\\save"), std::string("\"C:\\\\games\\\\save\""));
    CHECK_EQ(escape_string("\\\""), std::string("\"\\\\\\\"\""));
}

RS_TEST(escape_named_control_characters) {
    CHECK_EQ(escape_string("a\nb"), std::string("\"a\\nb\""));
    CHECK_EQ(escape_string("a\tb"), std::string("\"a\\tb\""));
    CHECK_EQ(escape_string("a\rb"), std::string("\"a\\rb\""));
    CHECK_EQ(escape_string("a\bb"), std::string("\"a\\bb\""));
    CHECK_EQ(escape_string("a\fb"), std::string("\"a\\fb\""));
}

RS_TEST(escape_other_control_characters_as_hex) {
    CHECK_EQ(escape_string(std::string(1, '\x01')), std::string("\"\\u0001\""));
    CHECK_EQ(escape_string(std::string(1, '\x1f')), std::string("\"\\u001f\""));
    std::string with_nul("a");
    with_nul.push_back('\0');
    with_nul.push_back('b');
    CHECK_EQ(escape_string(with_nul), std::string("\"a\\u0000b\""));
}

RS_TEST(escape_utf8_passes_through_unchanged) {
    const std::string utf8 = "sk\xC3\xA5l \xE2\x9C\x93 \xF0\x9F\x8E\xAE"; // skal, check mark, game die
    CHECK_EQ(escape_string(utf8), "\"" + utf8 + "\"");
}

RS_TEST(write_number_formats) {
    CHECK_EQ(write_number(0.0), std::string("0"));
    CHECK_EQ(write_number(42.0), std::string("42"));
    CHECK_EQ(write_number(-17.0), std::string("-17"));
    CHECK_EQ(write_number(1756400000.0), std::string("1756400000"));
    const std::string half = write_number(0.5);
    CHECK_EQ(half, std::string("0.5"));
    // NaN and infinity must not leak into the wire format.
    CHECK_EQ(write_number(std::nan("")), std::string("null"));
    CHECK_EQ(write_number(1.0 / 0.0), std::string("null"));
}

RS_TEST(object_writer_builds_flat_objects) {
    ObjectWriter writer;
    writer.add("name", "boss \"final\"");
    writer.add("count", 3);
    writer.add("ratio", 0.5);
    writer.add("won", true);
    writer.add_null("nothing");
    writer.add_raw("data", "{\"a\":1}");
    CHECK_EQ(writer.str(),
             std::string("{\"name\":\"boss \\\"final\\\"\",\"count\":3,\"ratio\":0.5,"
                         "\"won\":true,\"nothing\":null,\"data\":{\"a\":1}}"));
}

RS_TEST(field_round_trip_through_parser) {
    ravensight::EventData data{
        {"level", 3},
        {"time", 41.5},
        {"name", "cave \"deep\"\n"},
        {"won", false},
        {"none", nullptr},
        {"big", 9007199254740992LL},
    };
    const std::string json = ravensight::write_fields(data);
    const Value parsed = Value::parse(json);
    REQUIRE(parsed.is_object());
    double number = 0.0;
    CHECK(parsed.get_number("level", number));
    CHECK_EQ(number, 3.0);
    CHECK(parsed.get_number("time", number));
    CHECK_EQ(number, 41.5);
    CHECK_EQ(parsed.get_string("name", ""), std::string("cave \"deep\"\n"));
    bool flag = true;
    CHECK(parsed.get_bool("won", flag));
    CHECK_EQ(flag, false);
    const Value* none = parsed.find("none");
    REQUIRE(none != nullptr);
    CHECK(none->is_null());
}

RS_TEST(parse_session_response_shape) {
    const Value parsed = Value::parse(
        "{\"token\":\"st_abc\",\"expiresAt\":1756512000,\"expiresIn\":86400}");
    REQUIRE(parsed.is_object());
    CHECK_EQ(parsed.get_string("token", ""), std::string("st_abc"));
    double expires = 0.0;
    CHECK(parsed.get_number("expiresAt", expires));
    CHECK_EQ(expires, 1756512000.0);
}

RS_TEST(parse_settings_and_error_shapes) {
    bool enabled = true;
    CHECK(Value::parse("{\"trackingEnabled\":false}").get_bool("trackingEnabled", enabled));
    CHECK_EQ(enabled, false);
    // A string "false" must not be coerced into a bool.
    CHECK(!Value::parse("{\"trackingEnabled\":\"false\"}").get_bool("trackingEnabled", enabled));

    ravensight::Response response;
    response.status = 429;
    response.body = "{\"error\":\"rate_limited\"}";
    CHECK_EQ(response.error_code("fallback"), std::string("rate_limited"));
    response.body = "not json at all {";
    CHECK_EQ(response.error_code("fallback"), std::string("fallback"));
}

RS_TEST(parse_nested_arrays_and_objects) {
    const Value parsed = Value::parse(
        " { \"suggestions\" : [ {\"title\":\"a\"} , {\"title\":\"b\"} , 7 , null ] } ");
    REQUIRE(parsed.is_object());
    const Value* suggestions = parsed.find("suggestions");
    REQUIRE(suggestions != nullptr);
    REQUIRE(suggestions->is_array());
    CHECK_EQ(suggestions->size(), std::size_t(4));
    CHECK_EQ(suggestions->at(0).get_string("title", ""), std::string("a"));
    CHECK_EQ(suggestions->at(2).as_number(), 7.0);
    CHECK(suggestions->at(3).is_null());
    CHECK(suggestions->at(99).is_null());
}

RS_TEST(parse_unicode_escapes) {
    CHECK_EQ(Value::parse("\"\\u0041\\u00e5\"").as_string(), std::string("A\xC3\xA5"));
    // Surrogate pair for U+1F3AE (video game).
    CHECK_EQ(Value::parse("\"\\ud83c\\udfae\"").as_string(), std::string("\xF0\x9F\x8E\xAE"));
    // A lone surrogate is malformed and yields a null value.
    CHECK(Value::parse("\"\\ud83c\"").is_null());
}

RS_TEST(parse_numbers) {
    CHECK_EQ(Value::parse("0").as_number(), 0.0);
    CHECK_EQ(Value::parse("-12.25").as_number(), -12.25);
    CHECK_EQ(Value::parse("1e3").as_number(), 1000.0);
    CHECK_EQ(Value::parse("2.5E-1").as_number(), 0.25);
}

RS_TEST(parse_is_tolerant_of_malformed_input) {
    CHECK(Value::parse("").is_null());
    CHECK(Value::parse("{").is_null());
    CHECK(Value::parse("{\"a\":}").is_null());
    CHECK(Value::parse("[1,").is_null());
    CHECK(Value::parse("\"unterminated").is_null());
    CHECK(Value::parse("{\"a\" 1}").is_null());
    CHECK(Value::parse("nope").is_null());
    CHECK(Value::parse("\"bad\\qescape\"").is_null());
    // Deeply nested input is rejected instead of blowing the stack.
    std::string deep;
    for (int i = 0; i < 200; ++i) deep += "[";
    CHECK(Value::parse(deep).is_null());
}

RS_TEST(dump_round_trips) {
    const std::string source =
        "{\"a\":[1,2.5,true,null,\"x\\ny\"],\"b\":{\"c\":\"\xE2\x9C\x93\"}}";
    const Value parsed = Value::parse(source);
    REQUIRE(parsed.is_object());
    const Value reparsed = Value::parse(parsed.dump());
    REQUIRE(reparsed.is_object());
    CHECK_EQ(reparsed.dump(), parsed.dump());
    CHECK_EQ(reparsed.find("a")->size(), std::size_t(5));
    CHECK_EQ(reparsed.find("b")->get_string("c", ""), std::string("\xE2\x9C\x93"));
}

RS_TEST(retry_after_parsing) {
    using ravensight::parse_retry_after_seconds;
    CHECK_EQ(parse_retry_after_seconds("30"), 30.0);
    CHECK_EQ(parse_retry_after_seconds(" 2.5 "), 2.5);
    CHECK_EQ(parse_retry_after_seconds("0"), 0.0);
    CHECK_EQ(parse_retry_after_seconds("-5"), 0.0);
    CHECK_EQ(parse_retry_after_seconds(""), 0.0);
    // HTTP-date form falls back to exponential backoff.
    CHECK_EQ(parse_retry_after_seconds("Wed, 21 Oct 2026 07:28:00 GMT"), 0.0);
    CHECK_EQ(parse_retry_after_seconds("soon"), 0.0);
}
