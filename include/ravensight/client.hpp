// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// The public entry point. One Client per game process: it owns a background
// worker thread that reads the server kill switch, opens the session, runs
// the 5 second flush timer and performs every network request. The public
// methods are thread safe and never block on the network, except the two
// that say so in their name (flush_and_wait, shutdown). Opting out, reading
// and writing the saved device id and opt-out are small local file reads and
// writes on the calling thread.

#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
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

/// The six feedback categories the server accepts. Any other non-empty
/// category is refused locally with invalid_category, without a request.
inline constexpr std::array<std::string_view, 6> feedback_categories{
    {"bug", "suggestion", "complaint", "praise", "playtest", "other"}};

/// Why the client is or is not sending. Ok means ready (null in the other
/// SDKs); every other value is one of the shared reason codes.
enum class Status {
    /// Sending normally, or about to (no failure seen).
    ok,
    /// Turned off by Config::enabled, set_enabled(false) or the player's
    /// saved opt-out (set_tracking_enabled(false)).
    disabled,
    /// The ingest key was refused (401, or a 403 that is not one of the
    /// codes below). Automatic sending stops for the run; events still
    /// queue and an explicit flush() makes one more attempt.
    invalid_key,
    /// The server kill switch: GET /settings said trackingEnabled false, or
    /// a request answered 403 tracking_disabled. Stops for the run.
    tracking_disabled,
    /// No connection, a timeout or a server error. Retried with backoff.
    offline,
    /// 429, or a 503 with Retry-After. Retried after the server's wait.
    rate_limited,
    /// The session could not be opened, an invalid playtest token, or a
    /// playtest token with no run id (the last two stop for the run).
    session_failed,
    /// The playtest job this run belongs to has finished. Stops for the run.
    playtest_job_closed,
};

/// The reason code as text: "disabled", "invalid_key" and so on, or an
/// empty string for Status::ok.
const char* to_string(Status status);

struct Config {
    /// Your publishable ingest key, format gt_live_... Required.
    std::string ingest_key;

    /// Base URL. Trailing slashes and a trailing "/api/v1" are accepted and
    /// normalized; empty means the default. Only change this if you self host.
    std::string api_url = "https://api.ravensight.io";

    /// Reported to the server as the client's game version.
    std::string game_version = "1.0.0";

    /// Reported platform. Empty uses the platform detected at compile time
    /// (windows, macos, linux, android, ios or unknown).
    std::string platform;

    /// Your own stable per install id. Empty (the default) uses a random
    /// "dev_" id saved in the storage directory.
    std::string device_id;

    /// Directory for the saved device id and the player's opt-out. Empty
    /// uses the per user default (see default_storage_dir). Set it on
    /// platforms with no HOME or APPDATA, such as Android or consoles.
    std::string storage_dir;

    /// Overrides the device id file path. Default: storage_dir/device_id.
    std::string device_id_file;

    /// Overrides the opt-out file path. Default: beside device_id_file when
    /// that is set (a file named ravensight_state), else storage_dir/state.
    std::string state_file;

    /// Offline queue cap. Oldest events are dropped first past this.
    int max_queue_size = limits::default_queue_cap;

    /// How often the worker attempts a flush. 0 or less flushes only on
    /// demand (explicit flush calls, 50 queued events, and shutdown).
    double flush_interval_seconds = 5.0;

    /// Per request timeout for the built in libcurl transport.
    double request_timeout_seconds = 15.0;

    /// Queues game_started when the client starts (or is first enabled) and
    /// game_exited during shutdown, once each.
    bool track_lifecycle_events = true;

    /// Start opted out by setting this false. Never saved: for the player's
    /// own choice, use set_tracking_enabled, which is remembered.
    bool enabled = true;

    /// A session token from another Ravensight client (usually the player's
    /// game client), sent once as joinToken when this client opens its
    /// session, so a dedicated server can share the player's session.
    std::string join_token;

    /// Playtest overrides. Normally left empty: the Ravensight Playtest
    /// runner sets RAVENSIGHT_PLAYTEST_TOKEN and RAVENSIGHT_PLAYTEST_RUN_ID
    /// (and RAVENSIGHT_PLAYTEST_JOB_ID, RAVENSIGHT_PLAYTEST_PERSONA), which
    /// the client reads when these are empty. See apply_playtest_args for
    /// the command line form.
    std::string playtest_token;
    std::string playtest_run_id;
    std::string playtest_job_id;
    std::string playtest_persona;

    /// Opt in to performance sampling. When true, Client::sample_frame
    /// samples the frame rate once a second and shutdown() sends one
    /// `health` event (median and slowest tenth frame rate, memory and boot
    /// time where the platform exposes them). Off by default.
    bool performance = false;

    /// Optional log sink for SDK activity. Called from the worker thread.
    /// Warnings (a refused key, the kill switch, a closed playtest) go to
    /// stderr when no sink is set.
    std::function<void(const std::string&)> log;

    /// Optional. Called on the worker thread whenever status() changes.
    std::function<void(Status)> on_status;
};

/// Aggregate counters, safe to read from any thread.
struct Stats {
    int queued = 0;
    int sent = 0;
    int dropped = 0;
    int batches = 0;
    int sessions = 0;
    /// The last error code seen, for example "invalid_join_token" or
    /// "offline". Empty when none.
    std::string last_error;
};

/// Result of a feedback submission, delivered to the callback.
struct FeedbackResult {
    bool ok = false;
    /// Machine readable reason when ok is false. Local refusals, made
    /// without a request: empty_message, invalid_category, invalid_rating,
    /// shutting_down, or the current status code (disabled,
    /// tracking_disabled, playtest_job_closed, ...). Otherwise the server's
    /// error code, or no_session when no session could be opened.
    std::string code;
    /// HTTP status, 0 when the request never reached the server.
    long status = 0;
};

using FeedbackCallback = std::function<void(const FeedbackResult&)>;

/// Receives the suggestions array (empty on failure, while the client is not
/// sending, or when the game has no digest data yet). ok is false when no
/// request was made or the request failed.
using SuggestionsCallback = std::function<void(bool ok, const json::Value& suggestions)>;

/// A fresh random device id: "dev_" plus 32 hex characters from 16 bytes of
/// the operating system's random source.
std::string generate_device_id();

/// The per user directory where a Client keeps its device id and the
/// player's opt-out by default, or empty when the platform has none:
///   Windows: %APPDATA%\ravensight\<hash>
///   macOS and iOS: ~/Library/Application Support/ravensight/<hash>
///   elsewhere: $XDG_DATA_HOME (else ~/.local/share)/ravensight/<hash>
/// <hash> is the first 12 hex characters of SHA-256 of the ingest key.
std::string default_storage_dir(const std::string& ingest_key);

/// Saves the player's analytics choice for the client this config would
/// build, without a Client: call it from a settings screen before the
/// client exists. A saved opt-out wins over Config::enabled. Returns false
/// when the file could not be written, or during a playtest run (no-op).
bool set_tracking_enabled(const Config& config, bool enabled);

/// Copies --ravensight-playtest-token=... and --ravensight-playtest-run-id=...
/// from the command line into config, for any field that is still empty and
/// whose environment variable is unset (an explicit value, then the
/// environment, then the command line). Returns true when a token was taken.
bool apply_playtest_args(Config& config, int argc, const char* const* argv);

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

    /// Queues an event. Never sends by itself: the worker sends on its timer,
    /// as soon as 50 events are queued, and on flush and shutdown. Returns
    /// false when the client is not sending (see status()) or shutting down.
    bool track(const std::string& event_name, const EventData& data = {});

    /// Asks the worker to flush now instead of waiting for the timer.
    /// Non blocking. After a refused key this makes one more attempt.
    void flush();

    /// Flushes and waits until the queue drains or timeout_seconds passes.
    /// Returns true when the queue is empty afterwards. Blocking: call it at
    /// a natural pause such as the end of a level, not from a callback.
    bool flush_and_wait(double timeout_seconds = 5.0);

    // --- Typed events ------------------------------------------------------
    //
    // Thin helpers over track(): each builds the reserved rs_* properties
    // and calls track(), so queueing, batching, retries and opt-out are
    // unchanged and the return value is track()'s. `extra` is merged first,
    // so a reserved key always wins over a caller key of the same name.
    // `event_name` overrides the default name when not empty. Nothing is
    // validated or thrown: a bad value is sent as given and the server
    // flags the event (it shows on the Implementation health card).

    /// A real money purchase. `amount` is whole minor units (499 is 4.99):
    /// an integer or a digit string. `currency` is upper-cased. `cart` and
    /// `transaction_id` are left out when empty. Default name: purchase.
    bool track_purchase(const Field& amount, const std::string& currency,
                        const std::string& item_type, const std::string& item_id,
                        const std::string& cart = "", const std::string& transaction_id = "",
                        const EventData& extra = {}, const std::string& event_name = "");

    /// In game currency or material moving. `flow` is "source" (gained) or
    /// "sink" (spent). `item_type` and `item_id` are left out when empty.
    /// Default name: resource_source or resource_sink.
    bool track_resource(const std::string& flow, const std::string& resource, double amount,
                        const std::string& item_type = "", const std::string& item_id = "",
                        const EventData& extra = {}, const std::string& event_name = "");

    /// A step through the game's structure. `status` is "start", "complete"
    /// or "fail". `p2` and `p3` are left out when empty. Default name:
    /// progression_start, progression_complete or progression_fail.
    bool track_progression(const std::string& status, const std::string& p1,
                           const std::string& p2 = "", const std::string& p3 = "",
                           std::optional<double> score = std::nullopt,
                           const EventData& extra = {}, const std::string& event_name = "");

    /// An error the game hit. `severity` is "debug", "info", "warning",
    /// "error" or "critical". The message is cut to 200 characters and the
    /// stack (left out when empty) to 2000. Default name: error.
    bool track_error(const std::string& severity, const std::string& message,
                     const std::string& stack = "", const EventData& extra = {},
                     const std::string& event_name = "");

    // --- Performance sampling (Config::performance) ------------------------

    /// Call once per rendered frame with the frame time in seconds. Takes at
    /// most one sample per second, keeps at most 600, and does nothing
    /// unless Config::performance is true. Cheap enough for the game loop
    /// and thread safe. shutdown() sends one `health` event from the
    /// samples, and nothing when none were taken.
    void sample_frame(double dt_seconds);

    // --- Feedback and suggestions ------------------------------------------

    /// Submits free form player feedback. category is optional (empty) or one
    /// of feedback_categories; rating is 0 to omit it, or 1 to 5. A local
    /// refusal (see FeedbackResult::code) calls back at once on the calling
    /// thread with no request; otherwise the callback runs on the worker.
    void submit_feedback(std::string message, std::string category = "", int rating = 0,
                         FeedbackCallback callback = nullptr);

    /// EXPERIMENTAL. Fetches AI generated design suggestions for this game.
    /// Empty until the game has enough data for a weekly digest, and the
    /// shape of each entry may change, so do not build game logic on it.
    /// While the client is not sending (or the key was refused) it calls back
    /// at once with (false, empty array) and makes no request; otherwise the callback
    /// runs on the worker thread.
    void fetch_suggestions(SuggestionsCallback callback);

    // --- Opt-out and identity ----------------------------------------------

    /// Runtime on and off, not saved (the game's own switch). Disabling
    /// discards anything still queued. Enabling a client that started
    /// disabled runs the settings check and queues game_started then.
    void set_enabled(bool enabled);
    bool is_enabled() const;

    /// The player's choice, saved to state_file at once and read by every
    /// later Client. Turning it off discards the queue and stops sending.
    /// A no-op that logs one line during a playtest run.
    void set_tracking_enabled(bool enabled);

    /// False when the player has opted out with set_tracking_enabled(false).
    bool is_tracking_enabled_by_player() const;

    /// Mints and saves a new random device id, discards the queue and drops
    /// the session, so nothing links the old id to the new one. While the
    /// client is disabled or opted out the old saved id is deleted and the
    /// new one is saved once the client may send again. Returns the
    /// new id. A no-op that logs one line during a playtest run (it returns
    /// the current id).
    std::string reset_device_id();

    // --- State -------------------------------------------------------------

    /// Why the client is or is not sending. Changes are also reported to
    /// Config::on_status on the worker thread.
    Status status() const;

    /// False once sending stopped for the rest of the run: the server kill
    /// switch (GET /settings or a 403 tracking_disabled) or a closed or
    /// invalid playtest. status() says which.
    bool tracking_enabled() const;

    /// True once a valid session token is held.
    bool is_ready() const;

    /// The current session token, empty until a session opens. Hand it to
    /// a game server, which passes it as Config::join_token.
    std::string session_token() const;

    /// True when this run is a Ravensight Playtest run (a token was found).
    bool is_playtest() const;

    /// The device id in use. "pt-<run id>" during a playtest run.
    std::string device_id() const;
    const std::string& api_url() const { return api_url_; }

    Stats stats() const;

    /// Queues the one `health` summary (Config::performance, only if a
    /// sample was taken), game_exited (when lifecycle events are on), makes one final
    /// flush attempt and joins the worker. Blocking for at most about 1.5
    /// seconds past any request already in flight with the built in
    /// transport. Idempotent; the destructor calls it too.
    void shutdown();

    /// Trims whitespace, strips trailing slashes and a trailing /api/v1,
    /// then appends /api/v1. Empty gives https://api.ravensight.io/api/v1.
    static std::string normalize_api_url(const std::string& url);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    std::string api_url_;
};

} // namespace ravensight
