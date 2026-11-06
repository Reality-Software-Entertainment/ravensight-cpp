// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Internal pieces of the delivery contract (docs/sdk/DELIVERY_CONTRACT.md),
// kept pure so the unit tests drive them straight from the shared vectors:
// the queue file name and format (version 2), request signing, the event
// size count the server uses, gzip, and the ad revenue arithmetic. Not part
// of the public API.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "ravensight/queue.hpp"

namespace ravensight {
namespace detail {

// --- Limits from the contract ------------------------------------------------

/// Events older than this are dropped when the queue file is read: the
/// server refuses anything past 90 days.
inline constexpr long long max_queued_event_age_ms = 89LL * 24 * 60 * 60 * 1000;

/// A body this size or larger (before compression) is sent gzip compressed.
inline constexpr std::size_t gzip_threshold_bytes = 1024;

/// A typed error's message and stack caps, in UTF-16 units as the server
/// counts them.
inline constexpr std::size_t typed_message_chars = 200;
inline constexpr std::size_t typed_stack_chars = 2000;
inline constexpr std::size_t typed_trail_chars = 1500;

// --- Queue file --------------------------------------------------------------

/// 64-bit FNV-1a of the key's bytes as 16 lower case hex characters.
std::string fnv1a64_hex(const std::string& text);

/// queue-<16 hex>.json for this ingest key.
std::string queue_file_name(const std::string& ingest_key);

/// The queue as a version 2 file: consecutive events with the same origin
/// form one group. Session tokens and an error's rs_trail are never written.
std::string write_queue_file(const std::deque<Event>& events);

/// What read_queue_file found.
struct LoadedQueue {
    std::vector<Event> events;
    /// Dropped for being older than max_queued_event_age_ms.
    int too_old = 0;
    /// Dropped because the file held more than the cap.
    int over_cap = 0;
};

/// Reads a version 1 or 2 file. A missing, malformed or unknown version
/// file is an empty queue. Keeps at most cap events, the newest.
LoadedQueue read_queue_file(const std::string& text, long long now_ms, int cap);

/// The session id inside an origin object's JSON text, empty when none.
std::string origin_session(const std::string& origin_json);

// --- Signing -----------------------------------------------------------------

/// Lower case hex HMAC-SHA256 of message keyed with the bytes of secret.
std::string hmac_sha256_hex(const std::string& secret, const std::string& message);

/// The X-Ravensight-Signature header value: "v1=" + HMAC of
/// "v1:<timestamp>:<body>". body is the exact bytes sent, before gzip.
std::string signature_header(const std::string& secret, long long timestamp,
                             const std::string& body);

/// The signed beacon envelope (contract 7.4) as JSON text.
std::string beacon_envelope(const std::string& session_token, const std::string& secret,
                            long long timestamp, const std::string& body);

// --- Event size ----------------------------------------------------------------

/// The size the server's 8 KB ceiling counts for one event's data: its
/// serialized bytes, except that an error event's rs_message (200),
/// rs_stack (2,000) and rs_trail (1,500) within their caps count one byte a
/// UTF-16 unit (docs/errors/CONTRACT.md defines the fields).
std::size_t counted_event_bytes(const std::string& data_json);

/// UTF-16 length of UTF-8 text, how the server counts characters.
std::size_t utf16_units(const std::string& text);

/// Cuts UTF-8 text to at most max_units UTF-16 units without splitting a
/// character.
std::string cut_utf16(const std::string& text, std::size_t max_units);

// --- Compression ---------------------------------------------------------------

/// True when this build can gzip request bodies (zlib was found).
bool gzip_available();

/// gzip of data, or empty on failure or when gzip is not available.
std::string gzip_compress(const std::string& data);

/// Inverse, for the tests. Empty on failure.
std::string gzip_decompress(const std::string& data);

// --- Ads -----------------------------------------------------------------------

/// "0.0015" to "1500": decimal text in major units to whole micros, rounding
/// half up past the sixth decimal, on the digits only. Empty when the text is
/// not a plain non-negative decimal or the result has more than 18 digits.
std::string micros_from_decimal_string(const std::string& text);

/// A mediation callback's double in major units to micros: formatted with
/// 15 significant digits, then micros_from_decimal_string. Never multiplies.
std::string micros_from_double(double value);

/// A mediation format label (BANNER, MREC, INTER, REWARDED, rewarded_video,
/// APP_OPEN, ...) to banner, interstitial, rewarded, native, app_open or other.
std::string map_ad_format(const std::string& label);

// --- Device fields -------------------------------------------------------------

/// A device field as sent: printable characters only, trimmed, at most 128
/// characters. Empty means unknown and is not sent.
std::string clean_device_field(const std::string& value);

} // namespace detail
} // namespace ravensight
