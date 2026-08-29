// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The public entry point. One Client per game process: it owns a background
// worker thread that reads the server kill switch, opens the session, runs
// the 5 second flush timer and performs every network request. The public
// methods are thread safe and never block on the network, except the two
// that say so in their name (flush_and_wait, shutdown).

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ravensight/json.hpp"
#include "ravensight/queue.hpp"
#include "ravensight/transport.hpp"

namespace ravensight {

/// One value in an event payload. Implicitly constructible from strings,
/// integers, floating point values, bools and nullptr, so payloads can be
/// written as brace initializer lists at the call site.
class Field {
public:
    Field(std::nullptr_t) : type_(Type::null) {}
    Field(bool value) : type_(Type::boolean), bool_(value) {}
    Field(int value) : type_(Type::integer), int_(value) {}
    Field(unsigned int value) : type_(Type::integer), int_(static_cast<long long>(value)) {}
    Field(long value) : type_(Type::integer), int_(value) {}
    Field(unsigned long value) : Field(static_cast<unsigned long long>(value)) {}
    Field(long long value) : type_(Type::integer), int_(value) {}
    Field(unsigned long long value);
    Field(float value) : type_(Type::real), double_(static_cast<double>(value)) {}
    Field(double value) : type_(Type::real), double_(value) {}
    Field(const char* value);
    Field(std::string value) : type_(Type::string), string_(std::move(value)) {}

    /// Serializes this value as JSON.
    std::string to_json() const;

private:
    enum class Type { null, boolean, integer, real, string };
    Type type_ = Type::null;
    bool bool_ = false;
    long long int_ = 0;
    double double_ = 0.0;
    std::string string_;
};

/// An event payload: ordered key/value pairs, serialized as a flat JSON
/// object. Example: {{"level", 3}, {"deaths", 2}, {"seconds", 41.7}}.
using EventData = std::vector<std::pair<std::string, Field>>;

/// Serializes an event payload as a JSON object literal.
std::string write_fields(const EventData& data);

struct Config {
    /// Your publishable ingest key, format gt_live_... Required.
    std::string ingest_key;

    /// Base URL. "/api/v1" is appended when omitted. Only change this if you
    /// self host.
    std::string api_url = "https://api.ravensight.io/api/v1";

    /// Reported to the server as the client's game version.
    std::string game_version = "1.0.0";

    /// Reported platform. Empty uses the platform detected at compile time
    /// (windows, macos, linux, android, ios or unknown).
    std::string platform;

    /// Stable per install device id. Empty generates a random one, persisted
    /// to device_id_file when that is set and ephemeral otherwise.
    std::string device_id;

    /// Optional path where the generated device id is stored, so the same
    /// player is recognized across runs. Ignored when device_id is set.
    std::string device_id_file;

    /// Offline queue cap. Oldest events are dropped first past this.
    int max_queue_size = limits::default_queue_cap;

    /// How often the worker attempts a flush. 0 or less flushes only on
    /// demand (explicit flush calls and shutdown).
    double flush_interval_seconds = 5.0;

    /// Per request timeout for the built in libcurl transport.
    double request_timeout_seconds = 15.0;

    /// Sends game_started once tracking is confirmed enabled at boot, and
    /// game_exited during shutdown.
    bool track_lifecycle_events = true;

    /// Start opted out by setting this false.
    bool enabled = true;

    /// Optional log sink for SDK activity. Called from the worker thread.
    std::function<void(const std::string&)> log;
};

/// Aggregate counters, safe to read from any thread.
struct Stats {
    int queued = 0;
    int sent = 0;
    int dropped = 0;
    int batches = 0;
    int sessions = 0;
};

/// Result of a feedback submission, delivered to the callback.
struct FeedbackResult {
    bool ok = false;
    /// Machine readable reason when ok is false: the server's error code, or
    /// one of empty_message, tracking_disabled, no_session, shutting_down.
    std::string code;
    /// HTTP status, 0 when the request never reached the server.
    long status = 0;
};

using FeedbackCallback = std::function<void(const FeedbackResult&)>;

/// Receives the suggestions array (empty on failure or when the game has no
/// digest data yet). ok is false only when the request itself failed.
using SuggestionsCallback = std::function<void(bool ok, const json::Value& suggestions)>;

class Client {
public:
    /// Starts the worker thread using the built in libcurl transport.
    /// Throws std::invalid_argument when the ingest key is empty.
    explicit Client(Config config);

    /// Same, with a caller supplied transport. Used by the unit tests.
    Client(Config config, std::unique_ptr<Transport> transport);

    /// Calls shutdown().
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // --- Tracking, thread safe and non blocking ----------------------------

    /// Queues an event for delivery in batches of up to 50. Safe to call
    /// before the session is ready. Returns false when tracking is off or
    /// the client is shutting down.
    bool track(const std::string& event_name, const EventData& data = {});

    /// Asks the worker to flush now instead of waiting for the timer.
    /// Non blocking.
    void flush();

    /// Flushes and waits until the queue drains or timeout_seconds passes.
    /// Returns true when the queue is empty afterwards. Blocking: call it at
    /// a natural pause such as the end of a level, not from a callback.
    bool flush_and_wait(double timeout_seconds = 5.0);

    // --- Feedback and suggestions ------------------------------------------

    /// Submits free form player feedback. category and rating are optional;
    /// pass rating 1 to 5 if you have one, or 0 to omit it. The callback, if
    /// any, runs on the worker thread.
    void submit_feedback(std::string message, std::string category = "", int rating = 0,
                         FeedbackCallback callback = nullptr);

    /// EXPERIMENTAL. Fetches AI generated design suggestions for this game.
    /// Empty until the game has enough data for a weekly digest, and the
    /// shape of each entry may change, so do not build game logic on it.
    /// The callback runs on the worker thread.
    void fetch_suggestions(SuggestionsCallback callback);

    // --- State -------------------------------------------------------------

    /// Local opt in and opt out, for a privacy toggle. Disabling discards
    /// anything still queued, so an opt out does not leave player data
    /// sitting in memory.
    void set_enabled(bool enabled);
    bool is_enabled() const;

    /// The server side kill switch read from GET /settings at boot.
    bool tracking_enabled() const;

    /// True once a valid session token is held.
    bool is_ready() const;

    const std::string& device_id() const { return device_id_; }
    const std::string& api_url() const { return api_url_; }

    Stats stats() const;

    /// Sends game_exited (when lifecycle events are on), makes one final
    /// forced flush attempt and joins the worker. Blocking, bounded by the
    /// request timeout. Idempotent; the destructor calls it too.
    void shutdown();

    /// Accepts a bare host or a full /api/v1 base and returns the versioned base.
    static std::string normalize_api_url(const std::string& url);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::string device_id_;
    std::string api_url_;
};

} // namespace ravensight
