// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "delivery.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "ravensight/json.hpp"
#include "storage.hpp"

#if defined(RAVENSIGHT_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace ravensight {
namespace detail {

namespace {

const char hex_digits[] = "0123456789abcdef";

std::string to_hex(const std::string& bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (char ch : bytes) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        out.push_back(hex_digits[byte >> 4]);
        out.push_back(hex_digits[byte & 0xF]);
    }
    return out;
}

} // namespace

// --- Queue file --------------------------------------------------------------

std::string fnv1a64_hex(const std::string& text) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (char ch : text) {
        hash ^= static_cast<unsigned char>(ch);
        hash *= 0x100000001b3ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

std::string queue_file_name(const std::string& ingest_key) {
    return "queue-" + fnv1a64_hex(ingest_key) + ".json";
}

std::string origin_session(const std::string& origin_json) {
    if (origin_json.empty()) {
        return "";
    }
    return json::Value::parse(origin_json).get_string("session", "");
}

namespace {

/// The event's data as written to the queue file: an error's rs_trail never
/// reaches disk (the queue rules, rule 3); the event in memory keeps it.
std::string data_for_file(const std::string& data_json) {
    if (data_json.empty()) {
        return "{}";
    }
    if (data_json.find("\"rs_trail\"") == std::string::npos) {
        return data_json;
    }
    const json::Value data = json::Value::parse(data_json);
    if (!data.is_object()) {
        return data_json;
    }
    json::ObjectWriter writer;
    for (const auto& member : data.members()) {
        if (member.first != "rs_trail") {
            writer.add_raw(member.first, member.second.dump());
        }
    }
    return writer.str();
}

} // namespace

std::string write_queue_file(const std::deque<Event>& events) {
    std::string out = "{\"version\":2,\"groups\":[";
    bool first_group = true;
    std::size_t i = 0;
    while (i < events.size()) {
        const std::string& origin = events[i].origin_json;
        if (!first_group) {
            out.push_back(',');
        }
        first_group = false;
        out += "{";
        if (!origin.empty()) {
            out += "\"origin\":" + origin + ",";
        }
        out += "\"events\":[";
        bool first_event = true;
        for (; i < events.size() && events[i].origin_json == origin; ++i) {
            if (!first_event) {
                out.push_back(',');
            }
            first_event = false;
            out += "{\"event\":" + json::escape_string(events[i].name) +
                   ",\"timestamp\":" + std::to_string(events[i].timestamp) +
                   ",\"data\":" + data_for_file(events[i].data_json) +
                   (events[i].automatic_error ? ",\"automaticError\":true" : "") + "}";
        }
        out += "]}";
    }
    out += "]}";
    return out;
}

LoadedQueue read_queue_file(const std::string& text, long long now_ms, int cap) {
    LoadedQueue loaded;
    const json::Value root = json::Value::parse(text);
    double version = 0;
    if (!root.is_object() || !root.get_number("version", version) ||
        (version != 1.0 && version != 2.0)) {
        return loaded;
    }
    const json::Value* groups = root.find("groups");
    if (groups == nullptr || !groups->is_array()) {
        return loaded;
    }
    for (const json::Value& group : groups->items()) {
        if (!group.is_object()) {
            continue;
        }
        std::string origin;
        const json::Value* origin_value = group.find("origin");
        if (version == 2.0 && origin_value != nullptr && origin_value->is_object()) {
            origin = origin_value->dump();
        }
        const json::Value* events = group.find("events");
        if (events == nullptr || !events->is_array()) {
            continue;
        }
        for (const json::Value& item : events->items()) {
            const std::string name = item.get_string("event", "");
            double timestamp = 0;
            if (name.empty() || !item.get_number("timestamp", timestamp)) {
                continue;
            }
            const json::Value* data = item.find("data");
            Event event;
            event.name = name;
            event.timestamp = static_cast<long long>(timestamp);
            event.data_json = data != nullptr && data->is_object() ? data->dump() : "{}";
            event.origin_json = origin;
            bool automatic = false;
            event.automatic_error = item.get_bool("automaticError", automatic) && automatic;
            if (now_ms - event.timestamp > max_queued_event_age_ms) {
                ++loaded.too_old;
                continue;
            }
            loaded.events.push_back(std::move(event));
        }
    }
    const std::size_t limit = static_cast<std::size_t>(std::max(1, cap));
    if (loaded.events.size() > limit) {
        const std::size_t extra = loaded.events.size() - limit;
        loaded.over_cap = static_cast<int>(extra);
        loaded.events.erase(loaded.events.begin(),
                            loaded.events.begin() + static_cast<std::ptrdiff_t>(extra));
    }
    return loaded;
}

// --- Signing -----------------------------------------------------------------

std::string hmac_sha256_hex(const std::string& secret, const std::string& message) {
    std::string key = secret.size() > 64 ? sha256_bytes(secret) : secret;
    key.resize(64, '\0');
    std::string inner(64, '\0');
    std::string outer(64, '\0');
    for (std::size_t i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(key[i] ^ 0x36);
        outer[i] = static_cast<char>(key[i] ^ 0x5c);
    }
    return to_hex(sha256_bytes(outer + sha256_bytes(inner + message)));
}

std::string signature_header(const std::string& secret, long long timestamp,
                             const std::string& body) {
    return "v1=" + hmac_sha256_hex(secret, "v1:" + std::to_string(timestamp) + ":" + body);
}

std::string beacon_envelope(const std::string& session_token, const std::string& secret,
                            long long timestamp, const std::string& body) {
    json::ObjectWriter signed_part;
    signed_part.add("v", 1LL)
        .add("t", timestamp)
        .add("body", body)
        .add("sig", hmac_sha256_hex(secret, "v1:" + std::to_string(timestamp) + ":" + body));
    json::ObjectWriter envelope;
    envelope.add("sessionToken", session_token).add_raw("signed", signed_part.str());
    return envelope.str();
}

// --- Event size ----------------------------------------------------------------

std::size_t utf16_units(const std::string& text) {
    std::size_t units = 0;
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c & 0xC0) == 0x80) {
            continue;
        }
        units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

std::string cut_utf16(const std::string& text, std::size_t max_units) {
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        std::size_t length = 1;
        if (c >= 0xF0) {
            length = 4;
        } else if (c >= 0xE0) {
            length = 3;
        } else if (c >= 0xC0) {
            length = 2;
        }
        const std::size_t width = c >= 0xF0 ? 2 : 1;
        if (units + width > max_units) {
            break;
        }
        units += width;
        i += length;
    }
    return text.substr(0, std::min(i, text.size()));
}

std::size_t counted_event_bytes(const std::string& data_json) {
    std::size_t size = data_json.size();
    if (data_json.find("\"rs_kind\"") == std::string::npos) {
        return size;
    }
    const json::Value data = json::Value::parse(data_json);
    if (!data.is_object() || data.get_string("rs_kind", "") != "error") {
        return size;
    }
    const std::pair<const char*, std::size_t> exempt[] = {{"rs_message", typed_message_chars},
                                                          {"rs_stack", typed_stack_chars},
                                                          {"rs_trail", typed_trail_chars}};
    for (const auto& entry : exempt) {
        const json::Value* value = data.find(entry.first);
        if (value == nullptr || !value->is_string()) {
            continue;
        }
        const std::size_t units = utf16_units(value->as_string());
        if (units > entry.second) {
            continue;
        }
        const std::size_t serialized = json::escape_string(value->as_string()).size();
        size -= std::min(size, serialized - units);
    }
    return size;
}

// --- Compression ---------------------------------------------------------------

#if defined(RAVENSIGHT_HAVE_ZLIB)

bool gzip_available() { return true; }

std::string gzip_compress(const std::string& data) {
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8,
                     Z_DEFAULT_STRATEGY) != Z_OK) {
        return "";
    }
    std::string out;
    out.resize(deflateBound(&stream, static_cast<uLong>(data.size())) + 32);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    stream.next_out = reinterpret_cast<Bytef*>(&out[0]);
    stream.avail_out = static_cast<uInt>(out.size());
    const int result = deflate(&stream, Z_FINISH);
    const std::size_t written = stream.total_out;
    deflateEnd(&stream);
    if (result != Z_STREAM_END) {
        return "";
    }
    out.resize(written);
    return out;
}

std::string gzip_decompress(const std::string& data) {
    z_stream stream{};
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        return "";
    }
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    stream.avail_in = static_cast<uInt>(data.size());
    std::string out;
    char buffer[16384];
    int result = Z_OK;
    while (result == Z_OK) {
        stream.next_out = reinterpret_cast<Bytef*>(buffer);
        stream.avail_out = sizeof(buffer);
        result = inflate(&stream, Z_NO_FLUSH);
        if (result != Z_OK && result != Z_STREAM_END) {
            inflateEnd(&stream);
            return "";
        }
        out.append(buffer, sizeof(buffer) - stream.avail_out);
    }
    inflateEnd(&stream);
    return out;
}

#else

bool gzip_available() { return false; }
std::string gzip_compress(const std::string&) { return ""; }
std::string gzip_decompress(const std::string&) { return ""; }

#endif

// --- Ads -----------------------------------------------------------------------

std::string micros_from_decimal_string(const std::string& raw) {
    const std::string text = trim(raw);
    std::string whole;
    std::string fraction;
    bool dot = false;
    for (char ch : text) {
        if (ch == '.' && !dot) {
            dot = true;
        } else if (ch >= '0' && ch <= '9') {
            (dot ? fraction : whole).push_back(ch);
        } else {
            return "";
        }
    }
    if (whole.empty() || (dot && fraction.empty())) {
        return "";
    }
    const bool round_up = fraction.size() > 6 && fraction[6] >= '5';
    fraction.resize(6, '0');
    std::string digits = whole + fraction;
    if (round_up) {
        int i = static_cast<int>(digits.size()) - 1;
        while (i >= 0 && digits[static_cast<std::size_t>(i)] == '9') {
            digits[static_cast<std::size_t>(i)] = '0';
            --i;
        }
        if (i < 0) {
            digits.insert(digits.begin(), '1');
        } else {
            ++digits[static_cast<std::size_t>(i)];
        }
    }
    const std::size_t nonzero = digits.find_first_not_of('0');
    digits = nonzero == std::string::npos ? "0" : digits.substr(nonzero);
    return digits.size() > 18 ? "" : digits;
}

std::string micros_from_double(double value) {
    if (!(value >= 0.0) || value > 1e12) {
        return "";
    }
    // 15 significant digits, the most a double always holds exactly, so a
    // binary tail never decides the rounding (1.23456789 is stored as
    // 1.2345678899999999). Then the scientific text becomes plain decimal
    // text and the digits do the rest.
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.14e", value);
    const std::string sci = buf;
    const std::size_t e = sci.find('e');
    if (e == std::string::npos) {
        return "";
    }
    std::string mantissa;
    for (std::size_t i = 0; i < e; ++i) {
        if (sci[i] != '.') {
            mantissa.push_back(sci[i]);
        }
    }
    const int exponent = std::atoi(sci.c_str() + e + 1);
    std::string decimal;
    if (exponent < 0) {
        decimal = "0." + std::string(static_cast<std::size_t>(-exponent - 1), '0') + mantissa;
    } else {
        const std::size_t point = static_cast<std::size_t>(exponent) + 1;
        if (mantissa.size() <= point) {
            decimal = mantissa + std::string(point - mantissa.size(), '0');
        } else {
            decimal = mantissa.substr(0, point) + "." + mantissa.substr(point);
        }
    }
    return micros_from_decimal_string(decimal);
}

std::string map_ad_format(const std::string& label) {
    std::string key;
    for (char ch : trim(label)) {
        key.push_back(ch == ' ' || ch == '-' ? '_'
                                             : static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    if (key == "BANNER" || key == "MREC" || key == "LEADER" || key == "LEADERBOARD") return "banner";
    if (key == "INTER" || key == "INTERSTITIAL") return "interstitial";
    if (key == "REWARDED" || key == "REWARDED_VIDEO" || key == "REWARDED_INTER" ||
        key == "REWARDED_INTERSTITIAL" || key == "REWARDEDVIDEO") return "rewarded";
    if (key == "NATIVE") return "native";
    if (key == "APP_OPEN" || key == "APPOPEN" || key == "SPLASH") return "app_open";
    return "other";
}

// --- Device fields -------------------------------------------------------------

std::string clean_device_field(const std::string& value) {
    std::string printable;
    for (char ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= 0x20 && c != 0x7F) {
            printable.push_back(ch);
        }
    }
    return cut_utf16(trim(printable), 128);
}

} // namespace detail
} // namespace ravensight
