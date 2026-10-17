// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "ravensight/client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <future>
#include <mutex>
#include <random>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ravensight/curl_transport.hpp"
#include "storage.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace ravensight {

// --- Field and payload serialization ---------------------------------------

Field::Field(unsigned long long value) {
    if (value <= 9223372036854775807ULL) {
        type_ = Type::integer;
        int_ = static_cast<long long>(value);
    } else {
        type_ = Type::real;
        double_ = static_cast<double>(value);
    }
}

Field::Field(const char* value) {
    if (value == nullptr) {
        type_ = Type::null;
    } else {
        type_ = Type::string;
        string_ = value;
    }
}

std::string Field::to_json() const {
    switch (type_) {
        case Type::null:
            return "null";
        case Type::boolean:
            return bool_ ? "true" : "false";
        case Type::integer: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%lld", int_);
            return buf;
        }
        case Type::real:
            return json::write_number(double_);
        case Type::string:
            return json::escape_string(string_);
    }
    return "null";
}

std::string write_fields(const EventData& data) {
    std::string out = "{";
    bool first = true;
    for (const auto& entry : data) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        out += json::escape_string(entry.first);
        out.push_back(':');
        out += entry.second.to_json();
    }
    out.push_back('}');
    return out;
}

// --- Helpers ----------------------------------------------------------------

namespace {

/// How long the final flush may hold the quit.
constexpr double shutdown_hold_seconds = 1.5;

const char* detect_platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return "ios";
#else
    return "macos";
#endif
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

double now_unix_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool starts_with(const std::string& text, const std::string& prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

struct StoragePaths {
    std::string device_file;
    std::string state_file;
};

StoragePaths resolve_paths(const Config& config) {
    const std::string dir = config.storage_dir.empty()
                                ? detail::default_storage_dir(detail::trim(config.ingest_key))
                                : config.storage_dir;
    StoragePaths paths;
    if (!config.device_id_file.empty()) {
        paths.device_file = config.device_id_file;
    } else if (!dir.empty()) {
        paths.device_file = detail::join_path(dir, "device_id");
    }
    if (!config.state_file.empty()) {
        paths.state_file = config.state_file;
    } else if (!config.device_id_file.empty()) {
        paths.state_file =
            detail::join_path(detail::parent_dir(config.device_id_file), "ravensight_state");
    } else if (!dir.empty()) {
        paths.state_file = detail::join_path(dir, "state");
    }
    return paths;
}

struct PlaytestContext {
    std::string token;
    std::string run_id;
    std::string job_id;
    std::string persona;
};

std::string config_or_env(const std::string& value, const char* env_name) {
    const std::string trimmed = detail::trim(value);
    return trimmed.empty() ? detail::trim(detail::env_value(env_name)) : trimmed;
}

PlaytestContext resolve_playtest(const Config& config) {
    PlaytestContext context;
    context.token = config_or_env(config.playtest_token, "RAVENSIGHT_PLAYTEST_TOKEN");
    context.run_id = config_or_env(config.playtest_run_id, "RAVENSIGHT_PLAYTEST_RUN_ID");
    context.job_id = config_or_env(config.playtest_job_id, "RAVENSIGHT_PLAYTEST_JOB_ID");
    context.persona = config_or_env(config.playtest_persona, "RAVENSIGHT_PLAYTEST_PERSONA");
    return context;
}

const char* const opted_out_line = "tracking_enabled=false";
// Written beside "tracking_enabled=true" only for the yes of Consent::opt_in.
const char* const opted_in_line = "opt_in=true";

/// The player's saved choice for the consent mode: true when tracking may
/// run. opt_out: on unless an opt-out was saved. opt_in: off unless a yes
/// was saved.
bool read_allowed(const std::string& state_file, Consent consent) {
    std::string text;
    const bool has = !state_file.empty() && detail::read_text_file(state_file, text);
    if (consent == Consent::opt_in) {
        return has && text.find(opted_in_line) != std::string::npos;
    }
    return !(has && text.find(opted_out_line) != std::string::npos);
}

std::string state_text(bool enabled, Consent consent) {
    if (!enabled) {
        return std::string(opted_out_line) + "\n";
    }
    return consent == Consent::opt_in ? "tracking_enabled=true\n" + std::string(opted_in_line) + "\n"
                                      : std::string("tracking_enabled=true\n");
}

/// The length of a UTF-8 string in UTF-16 units, which is how the server
/// counts characters.
std::size_t utf16_length(const std::string& text) {
    std::size_t units = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if ((c & 0xC0) == 0x80) {
            continue; // continuation byte
        }
        units += c >= 0xF0 ? 2 : 1;
    }
    return units;
}

constexpr std::size_t max_distribution_length = 40;

/// The distribution option, trimmed, when it is valid; empty otherwise (and
/// invalid set when there was something that could not be sent).
std::string validated_distribution(const std::string& raw, bool& invalid) {
    invalid = false;
    const std::string label = detail::trim(raw);
    if (label.empty()) {
        return "";
    }
    bool control = false;
    for (char ch : label) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c == 0x7F) {
            control = true;
        }
    }
    if (control || utf16_length(label) > max_distribution_length) {
        invalid = true;
        return "";
    }
    return label;
}

/// rs_event_id from its parts: the first 16 characters of the device id
/// (anything outside A-Z, a-z, 0-9, _ and - replaced with _), the start
/// time in hex, the per-launch nonce and the counter.
std::string make_event_id(const std::string& device_id, long long start_unix,
                          const std::string& nonce, unsigned long long counter) {
    std::string prefix = device_id.substr(0, 16);
    for (char& ch : prefix) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
        if (!ok) {
            ch = '_';
        }
    }
    char start[32];
    std::snprintf(start, sizeof(start), "%llx", static_cast<unsigned long long>(start_unix));
    return prefix + "-" + start + "-" + nonce + "-" + std::to_string(counter);
}

/// Adds one already serialized "key":value member to a JSON object literal.
void append_member(std::string& object_json, const std::string& member) {
    if (object_json.size() <= 2) {
        object_json = "{" + member + "}";
    } else {
        object_json.pop_back();
        object_json += "," + member + "}";
    }
}

bool is_feedback_category(const std::string& category) {
    for (std::string_view known : feedback_categories) {
        if (category == known) {
            return true;
        }
    }
    return false;
}

const json::Value& empty_array() {
    static const json::Value value = json::Value::parse("[]");
    return value;
}

} // namespace

const char* to_string(Status status) {
    switch (status) {
        case Status::ok:
            return "";
        case Status::disabled:
            return "disabled";
        case Status::invalid_key:
            return "invalid_key";
        case Status::tracking_disabled:
            return "tracking_disabled";
        case Status::offline:
            return "offline";
        case Status::rate_limited:
            return "rate_limited";
        case Status::session_failed:
            return "session_failed";
        case Status::playtest_job_closed:
            return "playtest_job_closed";
        case Status::no_key:
            return "no_key";
    }
    return "";
}

std::string generate_device_id() {
    unsigned char bytes[16];
    if (!detail::os_random_bytes(bytes, sizeof(bytes))) {
        // Last resort: four independent 32 bit draws, never one seed.
        std::random_device device;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t value = static_cast<std::uint32_t>(device());
            std::memcpy(bytes + i * 4, &value, 4);
        }
    }
    static const char hex[] = "0123456789abcdef";
    std::string id = "dev_";
    for (unsigned char byte : bytes) {
        id.push_back(hex[byte >> 4]);
        id.push_back(hex[byte & 0xF]);
    }
    return id;
}

std::string default_storage_dir(const std::string& ingest_key) {
    return detail::default_storage_dir(detail::trim(ingest_key));
}

bool set_tracking_enabled(const Config& config, bool enabled) {
    if (!resolve_playtest(config).token.empty()) {
        if (config.log) {
            config.log("Ravensight: set_tracking_enabled ignored during a playtest run");
        }
        return false;
    }
    const StoragePaths paths = resolve_paths(config);
    if (paths.state_file.empty()) {
        return false;
    }
    return detail::write_text_file(paths.state_file, state_text(enabled, config.consent));
}

bool apply_playtest_args(Config& config, int argc, const char* const* argv) {
    static const std::string token_flag = "--ravensight-playtest-token=";
    static const std::string run_flag = "--ravensight-playtest-run-id=";
    std::string token;
    std::string run_id;
    for (int i = 1; i < argc; ++i) {
        if (argv == nullptr || argv[i] == nullptr) {
            continue;
        }
        const std::string arg = argv[i];
        if (starts_with(arg, token_flag)) {
            token = detail::trim(arg.substr(token_flag.size()));
        } else if (starts_with(arg, run_flag)) {
            run_id = detail::trim(arg.substr(run_flag.size()));
        }
    }
    bool took = false;
    if (!token.empty() && detail::trim(config.playtest_token).empty() &&
        detail::trim(detail::env_value("RAVENSIGHT_PLAYTEST_TOKEN")).empty()) {
        config.playtest_token = token;
        took = true;
    }
    if (!run_id.empty() && detail::trim(config.playtest_run_id).empty() &&
        detail::trim(detail::env_value("RAVENSIGHT_PLAYTEST_RUN_ID")).empty()) {
        config.playtest_run_id = run_id;
    }
    return took;
}

std::string Client::normalize_api_url(const std::string& url) {
    std::string raw = detail::trim(url);
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();
    }
    const std::string suffix = "/api/v1";
    if (ends_with(raw, suffix)) {
        raw.erase(raw.size() - suffix.size());
        while (!raw.empty() && raw.back() == '/') {
            raw.pop_back();
        }
    }
    if (raw.empty()) {
        raw = "https://api.ravensight.io";
    }
    return raw + suffix;
}

// --- Impl -------------------------------------------------------------------

class Client::Impl {
public:
    Impl(Config config, std::unique_ptr<Transport> transport, std::string api_url)
        : config_(std::move(config)),
          transport_(std::move(transport)),
          api_url_(std::move(api_url)),
          platform_(config_.platform.empty() ? detect_platform() : config_.platform) {
        // The worker is not running yet, so the *_locked helpers are safe
        // to call here without the mutex.
        queue_.set_max_queue_size(config_.max_queue_size);
        join_token_ = detail::trim(config_.join_token);
        enabled_ = config_.enabled;
        consent_ = config_.consent;
        start_unix_ = static_cast<long long>(now_unix_seconds());

        if (detail::trim(config_.ingest_key).empty()) {
            // A keyless clone or a development build: inert, not an error.
            // No worker thread, no transport, no file written; the paths are
            // kept only so the player's choice can still be saved.
            inert_ = true;
            paths_ = resolve_paths(config_);
            opted_out_ = !read_allowed(paths_.state_file, consent_);
            apply_local_state_locked();
            if (!config_.optional) {
                const std::string line =
                    "Ravensight: no ingest_key, so analytics is off for this run "
                    "(set Config::optional to silence this line)";
                if (config_.log) {
                    config_.log(line);
                } else {
                    std::fprintf(stderr, "%s\n", line.c_str());
                }
            }
            return;
        }

        queue_.set_random_source(&detail::random_unit);
        event_nonce_ = detail::random_hex(4);
        bool bad_distribution = false;
        distribution_ = validated_distribution(config_.distribution, bad_distribution);
        if (bad_distribution) {
            note_locked("distribution must be at most 40 characters with no control characters; "
                        "it is not sent",
                        true);
        }

        const PlaytestContext playtest = resolve_playtest(config_);
        playtest_token_ = playtest.token;
        is_playtest_ = !playtest_token_.empty();
        if (is_playtest_) {
            // A playtest run never reads or writes the saved id or opt-out.
            device_id_saved_ = true;
            if (playtest.run_id.empty()) {
                stop_locked(Status::session_failed, "",
                            "playtest token found without a run id "
                            "(RAVENSIGHT_PLAYTEST_RUN_ID), sending stopped for this run");
            } else {
                device_id_ = "pt-" + playtest.run_id;
                playtest_tags_ = build_playtest_tags(playtest);
            }
        } else {
            paths_ = resolve_paths(config_);
            opted_out_ = !read_allowed(paths_.state_file, consent_);
            if (!config_.device_id.empty()) {
                device_id_ = config_.device_id;
                device_id_saved_ = true;
            } else {
                std::string saved;
                if (!paths_.device_file.empty() &&
                    detail::read_text_file(paths_.device_file, saved) &&
                    !detail::trim(saved).empty()) {
                    device_id_ = detail::trim(saved);
                    device_id_saved_ = true;
                } else if (!(consent_ == Consent::opt_in && opted_out_)) {
                    // Saved only once the client is enabled and opted in.
                    // With Consent::opt_in and no yes, none is made at all
                    // (on_locally_enabled_locked makes it on the yes).
                    device_id_ = generate_device_id();
                }
            }
        }

        apply_local_state_locked();
        if (queue_.is_active()) {
            on_locally_enabled_locked();
        }
        worker_ = std::thread([this] { worker_main(); });
    }

    ~Impl() { shutdown(); }

    bool track(const std::string& event_name, const EventData& data) {
        if (event_name.empty()) {
            return false;
        }
        Event event;
        event.name = event_name;
        event.data_json = write_fields(data);
        event.timestamp = static_cast<long long>(now_unix_seconds());

        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_ || !queue_.is_active()) {
            return false;
        }
        if (!enqueue_locked(std::move(event))) {
            return false;
        }
        // A full batch goes out now rather than on the timer.
        if (queue_.count() >= limits::max_batch_size) {
            wake_ = true;
            cv_.notify_all();
        }
        return true;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        nudge_ = true;
        cv_.notify_all();
    }

    bool flush_and_wait(double timeout_seconds) {
        if (inert_) {
            return true;
        }
        if (std::this_thread::get_id() == worker_.get_id()) {
            // Called from a callback on the worker itself: run inline.
            flush_cycle(true);
            std::lock_guard<std::mutex> lock(mutex_);
            return queue_.is_empty();
        }

        auto done = std::make_shared<std::promise<bool>>();
        std::future<bool> future = done->get_future();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_requested_) {
                return queue_.is_empty();
            }
            commands_.push_back([this, done] {
                flush_cycle(true);
                bool empty;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    empty = queue_.is_empty();
                }
                done->set_value(empty);
            });
            cv_.notify_all();
        }
        const auto wait = std::chrono::duration<double>(timeout_seconds);
        if (future.wait_for(wait) != std::future_status::ready) {
            return false;
        }
        return future.get();
    }

    void submit_feedback(std::string message, std::string category, int rating,
                         FeedbackCallback callback) {
        message = detail::trim(message);
        std::string local_failure;
        if (message.empty()) {
            local_failure = "empty_message";
        } else if (!category.empty() && !is_feedback_category(category)) {
            local_failure = "invalid_category";
        } else if (rating != 0 && (rating < 1 || rating > 5)) {
            local_failure = "invalid_rating";
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_requested_) {
                local_failure = "shutting_down";
            } else if (!queue_.is_active()) {
                local_failure = to_string(compute_status_locked());
            } else {
                auto msg = std::make_shared<std::string>(std::move(message));
                auto cat = std::make_shared<std::string>(std::move(category));
                commands_.push_back([this, msg, cat, rating, callback] {
                    run_feedback(*msg, *cat, rating, callback);
                });
                cv_.notify_all();
                return;
            }
        }
        // Local failures are reported on the caller's thread, outside the lock.
        if (callback) {
            callback(FeedbackResult{false, local_failure, 0});
        }
    }

    void fetch_suggestions(SuggestionsCallback callback) {
        if (!callback) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutdown_requested_ && queue_.is_active()) {
                commands_.push_back([this, callback] { run_suggestions(callback); });
                cv_.notify_all();
                return;
            }
        }
        // Not sending: no request, same callback shape.
        callback(false, empty_array());
    }

    void set_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool was_active = queue_.is_active();
        enabled_ = enabled;
        apply_local_state_locked();
        if (!was_active && queue_.is_active()) {
            on_locally_enabled_locked();
        }
        schedule_settings_check_locked();
    }

    bool is_enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return enabled_ && !inert_;
    }

    bool is_collecting() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.is_active();
    }

    void set_tracking_enabled(bool enabled) {
        if (is_playtest_) {
            std::lock_guard<std::mutex> lock(mutex_);
            note_locked("set_tracking_enabled ignored during a playtest run", false);
            schedule_settings_check_locked();
            return;
        }
        // Written synchronously, so it holds even if the game quits next.
        const bool saved = !paths_.state_file.empty() &&
                           detail::write_text_file(paths_.state_file, state_text(enabled, consent_));

        std::lock_guard<std::mutex> lock(mutex_);
        if (!saved) {
            note_locked("could not save the player's analytics choice" +
                            (paths_.state_file.empty() ? std::string(" (no storage directory)")
                                                       : " to " + paths_.state_file) +
                            "; it applies to this run only",
                        true);
        }
        const bool was_active = queue_.is_active();
        opted_out_ = !enabled;
        apply_local_state_locked();
        if (!was_active && queue_.is_active()) {
            on_locally_enabled_locked();
        }
        schedule_settings_check_locked();
    }

    bool is_tracking_enabled_by_player() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return !opted_out_;
    }

    std::string reset_device_id() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_playtest_) {
            note_locked("reset_device_id ignored during a playtest run", false);
            schedule_settings_check_locked();
            return device_id_;
        }
        if (inert_) {
            return device_id_;
        }
        if (consent_ == Consent::opt_in && opted_out_ && config_.device_id.empty()) {
            // Not opted in: forget any old id and make none until the yes.
            if (!paths_.device_file.empty() && !detail::remove_file(paths_.device_file)) {
                note_locked("could not delete the saved device id at " + paths_.device_file, true);
            }
            device_id_.clear();
            device_id_saved_ = false;
            queue_.clear();
            queue_.invalidate_session();
            ++identity_generation_;
            schedule_settings_check_locked();
            return device_id_;
        }
        device_id_ = generate_device_id();
        // An id the game supplied in Config::device_id is the game's to keep;
        // the replacement lasts for this run only.
        device_id_saved_ = !config_.device_id.empty();
        if (queue_.is_active()) {
            persist_device_id_locked();
        } else if (!device_id_saved_ && !paths_.device_file.empty()) {
            // Disabled or opted out: the new id is not written yet, but the
            // old one must not come back on a later launch.
            if (!detail::remove_file(paths_.device_file)) {
                note_locked("could not delete the saved device id at " + paths_.device_file, true);
            }
        }
        queue_.clear();
        queue_.invalidate_session();
        // A session request already in flight belongs to the old id.
        ++identity_generation_;
        schedule_settings_check_locked();
        return device_id_;
    }

    Status status() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return compute_status_locked();
    }

    bool tracking_enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.is_server_tracking_enabled();
    }

    bool is_ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.is_session_valid(now_unix_seconds());
    }

    std::string session_token() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.session_token();
    }

    bool is_playtest() const { return is_playtest_; }

    std::string device_id() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return device_id_;
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const QueueStats qs = queue_.stats();
        Stats out;
        out.queued = qs.queued;
        out.sent = qs.sent;
        out.dropped = qs.dropped + too_large_dropped_;
        out.batches = qs.batches;
        out.sessions = qs.sessions;
        out.last_error = last_error_;
        return out;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutdown_requested_) {
                shutdown_requested_ = true;
                abort_at_.store(now_unix_seconds() + shutdown_hold_seconds);
            }
            cv_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    // --- Logging and status, see drain_logs ---------------------------------

    // Queues a log line. Lines are delivered by the worker thread outside the
    // lock, so a log sink may safely call back into the client.
    void note_locked(const std::string& message, bool warning) {
        pending_logs_.emplace_back(message, warning);
    }

    void note(const std::string& message, bool warning = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        note_locked(message, warning);
    }

    void drain_logs() {
        std::vector<std::pair<std::string, bool>> lines;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            lines.swap(pending_logs_);
        }
        for (const auto& line : lines) {
            const std::string text = "Ravensight: " + line.first;
            if (config_.log) {
                config_.log(text);
            } else if (line.second) {
                std::fprintf(stderr, "%s\n", text.c_str());
            }
        }
    }

    Status compute_status_locked() const {
        if (inert_) {
            return Status::no_key;
        }
        if (!enabled_ || opted_out_) {
            return Status::disabled;
        }
        if (stopped_) {
            return stop_reason_;
        }
        return transient_;
    }

    // Worker thread only: flushes queued log lines, then reports a status
    // change to on_status.
    void update_status() {
        drain_logs();
        Status current;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            current = compute_status_locked();
            if (current == last_reported_) {
                return;
            }
            last_reported_ = current;
        }
        if (config_.on_status) {
            config_.on_status(current);
        }
    }

    // --- Local state --------------------------------------------------------

    void apply_local_state_locked() {
        queue_.set_locally_enabled(!inert_ && enabled_ && !opted_out_);
    }

    // The client just became able to send: save the id it will use and queue
    // game_started once per run. With Consent::opt_in this is the first
    // moment a device id exists.
    void on_locally_enabled_locked() {
        if (device_id_.empty()) {
            device_id_ = generate_device_id();
            device_id_saved_ = false;
        }
        persist_device_id_locked();
        if (config_.track_lifecycle_events && !started_queued_ && queue_.is_active()) {
            started_queued_ = true;
            Event event;
            event.name = "game_started";
            event.timestamp = static_cast<long long>(now_unix_seconds());
            enqueue_locked(std::move(event));
        }
    }

    void persist_device_id_locked() {
        if (device_id_saved_) {
            return;
        }
        if (paths_.device_file.empty()) {
            if (!storage_warned_) {
                storage_warned_ = true;
                note_locked("no storage directory (set Config::storage_dir); the device id is "
                            "kept in memory for this run, so each launch counts as a new player",
                            true);
            }
            return;
        }
        if (detail::write_text_file(paths_.device_file, device_id_)) {
            device_id_saved_ = true;
        } else if (!storage_warned_) {
            storage_warned_ = true;
            note_locked("could not save the device id to " + paths_.device_file +
                            "; it is kept in memory for this run",
                        true);
        }
    }

    // Pokes the worker to run the settings check (when it has not run and
    // the client may send) and to report any status change.
    void schedule_settings_check_locked() {
        if (inert_) {
            return; // no worker to run it
        }
        commands_.push_back([this] { maybe_check_settings(); });
        cv_.notify_all();
    }

    static std::string build_playtest_tags(const PlaytestContext& playtest) {
        std::string tags = "\"synthetic\":true,\"pt_source\":\"sdk\",\"pt_run\":" +
                           json::escape_string(playtest.run_id);
        if (!playtest.job_id.empty()) {
            tags += ",\"pt_job\":" + json::escape_string(playtest.job_id);
        }
        if (!playtest.persona.empty()) {
            tags += ",\"persona\":" + json::escape_string(playtest.persona);
        }
        return tags;
    }

    // Adds the playtest tags and the rs_event_id, then queues the event.
    // Returns false when its data is over limits::max_event_bytes as JSON:
    // it is dropped and never sent.
    bool enqueue_locked(Event event) {
        if (!playtest_tags_.empty()) {
            append_member(event.data_json, playtest_tags_);
        }
        ++event_counter_;
        append_member(event.data_json,
                      "\"rs_event_id\":" +
                          json::escape_string(make_event_id(device_id_, start_unix_, event_nonce_,
                                                            event_counter_)));
        if (event.data_json.size() > limits::max_event_bytes) {
            ++too_large_dropped_;
            last_error_ = "event_too_large";
            if (!too_large_warned_) {
                too_large_warned_ = true;
                note_locked("dropped event \"" + event.name + "\": larger than " +
                                std::to_string(limits::max_event_bytes) +
                                " bytes as JSON (further drops are not logged)",
                            true);
            }
            return false;
        }
        const int dropped = queue_.enqueue(std::move(event));
        if (dropped > 0) {
            note_locked("queue full, dropped " + std::to_string(dropped) + " oldest event(s)", false);
        }
        return true;
    }

    // --- Failure handling ---------------------------------------------------

    // Stops sending for the rest of the run and clears the queue.
    void stop_locked(Status reason, const std::string& error_code, const std::string& message) {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        stop_reason_ = reason;
        last_error_ = error_code.empty() ? to_string(reason) : error_code;
        queue_.set_server_tracking_enabled(false);
        queue_.invalidate_session();
        note_locked(message, true);
    }

    // The key itself was refused (revoked, rotated, wrong). Retrying cannot
    // help, so the run ends here: the queue is cleared, nothing more is
    // sent or retried, and nothing is flushed at shutdown.
    void refuse_key_locked() {
        stop_locked(Status::invalid_key, "invalid_key",
                    "ingest key refused (invalid_key), sending stopped for this run. "
                    "Check your gt_live_ key.");
    }

    // Handles a 401 or 403 from an endpoint authenticated by the ingest key
    // (or a 403 from any endpoint). Returns false for any other status.
    bool handle_refusal(const Response& response) {
        if (response.status != 401 && response.status != 403) {
            return false;
        }
        const std::string code = response.error_code("");
        std::lock_guard<std::mutex> lock(mutex_);
        if (response.status == 403 && code == "tracking_disabled") {
            stop_locked(Status::tracking_disabled, code,
                        "tracking disabled by the server kill switch, sending stopped for this run");
        } else if (response.status == 403 && code == "playtest_job_closed") {
            stop_locked(Status::playtest_job_closed, code,
                        "playtest job closed, sending stopped for this run");
        } else if (response.status == 403 && code == "invalid_playtest_token") {
            stop_locked(Status::session_failed, code,
                        "playtest token refused (invalid_playtest_token), sending stopped for this run");
        } else {
            refuse_key_locked();
        }
        return true;
    }

    // 429, or 503 with Retry-After: wait exactly what the server asked.
    // Returns the status to record.
    Status apply_server_wait_locked(const Response& response, Status otherwise) {
        const double wait = response.retry_after_seconds();
        if (response.status == 429) {
            queue_.on_rate_limited(now_unix_seconds(), wait);
            return Status::rate_limited;
        }
        if (response.status == 503 && wait > 0.0) {
            queue_.on_rate_limited(now_unix_seconds(), wait);
            return Status::rate_limited;
        }
        queue_.on_transport_failure(now_unix_seconds());
        return response.status == 0 ? Status::offline : otherwise;
    }

    void record_transient_locked(Status status, const Response& response) {
        transient_ = status;
        last_error_ = response.status == 0 || response.status == 429
                          ? std::string(to_string(status))
                          : response.error_code(to_string(status));
    }

    bool aborted() const {
        const double deadline = abort_at_.load();
        return deadline > 0.0 && now_unix_seconds() >= deadline;
    }

    // --- Requests -----------------------------------------------------------

    Request make_request(const char* method, const char* path, bool with_key) const {
        Request request;
        request.method = method;
        request.url = api_url_ + path;
        request.abort_at_unix_seconds = &abort_at_;
        if (with_key) {
            request.with_header("X-API-Key", config_.ingest_key);
        }
        if (is_playtest_) {
            request.with_header("X-Ravensight-Playtest", playtest_token_);
        }
        return request;
    }

    // Reads the server kill switch, once per run, and only while the client
    // may send: a disabled or opted out client never asks.
    void maybe_check_settings() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (settings_checked_ || !queue_.is_active()) {
                return;
            }
        }
        const Response response = transport_->send(make_request("GET", "/settings", true));

        if (response.status == 200) {
            bool enabled = true;
            if (!response.parse_json().get_bool("trackingEnabled", enabled)) {
                enabled = true;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            settings_checked_ = true;
            if (!enabled) {
                stop_locked(Status::tracking_disabled, "tracking_disabled",
                            "tracking disabled by the server kill switch, sending stopped for this run");
            }
            return;
        }
        if (handle_refusal(response)) {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_checked_ = true;
            return;
        }
        // Unreachable or failing: assume tracking is on (the session request
        // enforces the kill switch too), but honor a server requested wait.
        std::lock_guard<std::mutex> lock(mutex_);
        settings_checked_ = true;
        Status status = response.status == 0 ? Status::offline : Status::session_failed;
        if (response.status == 429 || (response.status == 503 && response.retry_after_seconds() > 0.0)) {
            queue_.on_rate_limited(now_unix_seconds(), response.retry_after_seconds());
            status = Status::rate_limited;
        }
        record_transient_locked(status, response);
        note_locked("settings check failed (HTTP " + std::to_string(response.status) +
                        "), assuming tracking enabled",
                    false);
    }

    // Returns true when a valid session is held on exit. force ignores the
    // retry window. It never ignores the kill switch, a refused key or an
    // opt-out.
    bool ensure_session(bool force) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.is_session_valid(now_unix_seconds())) {
                return true;
            }
            if (!queue_.is_active()) {
                return false;
            }
            if (!force && queue_.seconds_until_next_attempt(now_unix_seconds()) > 0.0) {
                return false;
            }
        }

        // At most two passes: the second only after a refused join token.
        for (int pass = 0; pass < 2; ++pass) {
            if (aborted()) {
                return false;
            }
            std::string join_token;
            unsigned long long generation = 0;
            json::ObjectWriter body;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                join_token = join_token_;
                generation = identity_generation_;
                body.add("deviceId", device_id_);
            }
            body.add("gameVersion", config_.game_version);
            body.add("platform", platform_);
            if (!join_token.empty()) {
                body.add("joinToken", join_token);
            }
            if (!distribution_.empty()) {
                body.add("distribution", distribution_);
            }

            Request request = make_request("POST", "/session", true);
            request.body = body.str();
            request.with_header("Content-Type", "application/json");
            const Response response = transport_->send(request);

            if (response.status == 201) {
                const json::Value parsed = response.parse_json();
                const std::string token = parsed.get_string("token", "");
                std::lock_guard<std::mutex> lock(mutex_);
                if (token.empty()) {
                    note_locked("malformed session response", false);
                    queue_.schedule_backoff(now_unix_seconds());
                    record_transient_locked(Status::session_failed, response);
                    return false;
                }
                double expires_at = 0.0;
                if (!parsed.get_number("expiresAt", expires_at) || expires_at <= 0.0) {
                    double expires_in = 0.0;
                    if (!parsed.get_number("expiresIn", expires_in) || expires_in <= 0.0) {
                        expires_in = 86400.0;
                    }
                    expires_at = now_unix_seconds() + expires_in;
                }
                if (!queue_.is_active()) {
                    // Opted out or stopped while the request was in flight.
                    return false;
                }
                if (generation != identity_generation_) {
                    // reset_device_id() ran meanwhile: this session is the
                    // old identity's. Drop it; the next attempt uses the new id.
                    note_locked("discarded a session opened for the previous device id", false);
                    return false;
                }
                queue_.set_session(token, expires_at);
                if (join_token_ == join_token) {
                    join_token_.clear();
                }
                transient_ = Status::ok;
                note_locked("session ready", false);
                return true;
            }

            const std::string code = response.error_code("");
            if (response.status == 401 && !join_token.empty() && code == "invalid_join_token") {
                std::lock_guard<std::mutex> lock(mutex_);
                if (join_token_ == join_token) {
                    join_token_.clear();
                }
                last_error_ = "invalid_join_token";
                note_locked("join token refused (invalid_join_token), opening a plain session", true);
                continue;
            }
            if (handle_refusal(response)) {
                return false;
            }
            std::lock_guard<std::mutex> lock(mutex_);
            record_transient_locked(apply_server_wait_locked(response, Status::session_failed),
                                    response);
            note_locked("session creation failed (HTTP " + std::to_string(response.status) + ")",
                        false);
            return false;
        }
        return false;
    }

    static std::string build_batch_body(const std::vector<Event>& batch) {
        std::string body = "{\"events\":[";
        for (std::size_t i = 0; i < batch.size(); ++i) {
            if (i > 0) {
                body.push_back(',');
            }
            body += "{\"event\":";
            body += json::escape_string(batch[i].name);
            body += ",\"data\":";
            body += batch[i].data_json;
            body += ",\"timestamp\":";
            body += std::to_string(batch[i].timestamp);
            body.push_back('}');
        }
        body += "]}";
        return body;
    }

    // Sends batches of up to 50 until the queue drains or the server asks us
    // to back off. Events leave the queue only once the server accepts them
    // (or refuses a single one outright with 400).
    void flush_cycle(bool force) {
        maybe_check_settings();

        int reauth_attempts = 0;
        int session_attempts = 0;

        for (;;) {
            if (aborted()) {
                return;
            }
            FlushPlan plan;
            std::string token;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                plan = queue_.begin_flush(now_unix_seconds(), force);
                token = queue_.session_token();
            }

            if (plan.decision == FlushDecision::needs_session) {
                // The first session plus at most two re-opens after a 401.
                if (++session_attempts > 3) {
                    return;
                }
                if (!ensure_session(force)) {
                    return;
                }
                continue;
            }
            if (!plan.should_send()) {
                return;
            }

            Request request = make_request("POST", "/track/batch", false);
            request.body = build_batch_body(plan.batch);
            request.with_header("Content-Type", "application/json");
            request.with_header("X-Session-Token", token);

            const Response response = transport_->send(request);
            const std::size_t batch_size = plan.batch.size();

            if (response.status == 202) {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.on_batch_accepted(plan);
                queue_.end_flush();
                transient_ = Status::ok;
                reauth_attempts = 0;
                continue;
            }

            if (response.status == 401) {
                // Session expired or revoked. The batch stays queued and is
                // re-sent as soon as a fresh session is issued, at most twice
                // in a row; after that, ordinary backoff.
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.on_unauthorized();
                queue_.end_flush();
                if (++reauth_attempts > 2) {
                    queue_.schedule_backoff(now_unix_seconds());
                    record_transient_locked(Status::session_failed, response);
                    note_locked("session refused twice in a row, backing off", false);
                    return;
                }
                note_locked("session expired, re-authenticating", false);
                continue;
            }

            if (response.status == 403) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.end_flush();
                }
                handle_refusal(response);
                return;
            }

            if (response.status == 400) {
                std::lock_guard<std::mutex> lock(mutex_);
                const bool dropped = queue_.on_bad_request(plan);
                queue_.end_flush();
                if (batch_size > 1) {
                    note_locked("batch rejected with HTTP 400, splitting", false);
                } else if (dropped) {
                    last_error_ = response.error_code("bad_request");
                    if (!bad_event_warned_) {
                        bad_event_warned_ = true;
                        note_locked("dropped an event the server refused with HTTP 400 (" +
                                        last_error_ + "); counted in stats().dropped",
                                    true);
                    }
                }
                continue;
            }

            std::lock_guard<std::mutex> lock(mutex_);
            queue_.end_flush();
            record_transient_locked(apply_server_wait_locked(response, Status::offline), response);
            note_locked("batch flush failed (HTTP " + std::to_string(response.status) +
                            "), will retry",
                        false);
            return;
        }
    }

    void run_feedback(const std::string& message, const std::string& category, int rating,
                      const FeedbackCallback& callback) {
        const auto fail = [&](const std::string& code, long status) {
            if (callback) callback(FeedbackResult{false, code, status});
        };
        const auto current_failure = [&]() {
            std::lock_guard<std::mutex> lock(mutex_);
            const Status status = compute_status_locked();
            return status == Status::ok ? std::string("no_session") : std::string(to_string(status));
        };

        maybe_check_settings();
        if (!is_active()) {
            fail(current_failure(), 0);
            return;
        }
        if (!ensure_session(true)) {
            fail(current_failure(), 0);
            return;
        }

        json::ObjectWriter writer;
        writer.add("message", message);
        if (!category.empty()) {
            writer.add("category", category);
        }
        if (rating != 0) {
            writer.add("rating", static_cast<long long>(rating));
        }
        const std::string body = writer.str();

        Response response = send_feedback(body);
        if (response.status == 401) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.invalidate_session();
            }
            if (!ensure_session(true)) {
                fail(current_failure(), 0);
                return;
            }
            response = send_feedback(body);
        }

        if (response.status == 201) {
            if (callback) callback(FeedbackResult{true, "", 201});
            return;
        }
        if (response.status == 403) {
            handle_refusal(response);
        }
        note("feedback submission failed (HTTP " + std::to_string(response.status) + ")");
        fail(response.status == 0 ? std::string("offline")
                                  : response.error_code("http_" + std::to_string(response.status)),
             response.status);
    }

    bool is_active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.is_active();
    }

    Response send_feedback(const std::string& body) {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            token = queue_.session_token();
        }
        Request request = make_request("POST", "/feedback", false);
        request.body = body;
        request.with_header("Content-Type", "application/json");
        request.with_header("X-Session-Token", token);
        return transport_->send(request);
    }

    void run_suggestions(const SuggestionsCallback& callback) {
        maybe_check_settings();
        // Re-checked here: the player may have opted out, or the key been
        // refused, while this waited.
        bool may_send;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            may_send = queue_.is_active();
        }
        if (!may_send) {
            callback(false, empty_array());
            return;
        }
        const Response response =
            transport_->send(make_request("GET", "/agent/suggestions", true));

        if (response.status != 200) {
            handle_refusal(response);
            note("fetch suggestions failed (HTTP " + std::to_string(response.status) + ")");
            callback(false, empty_array());
            return;
        }
        const json::Value parsed = response.parse_json();
        const json::Value* suggestions = parsed.find("suggestions");
        if (suggestions != nullptr && suggestions->is_array()) {
            callback(true, *suggestions);
        } else {
            callback(true, empty_array());
        }
    }

    void worker_main() {
        maybe_check_settings();
        update_status();

        for (;;) {
            std::function<void()> command;
            bool do_shutdown = false;
            bool force = false;
            bool wake = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (commands_.empty() && !shutdown_requested_ && !nudge_ && !wake_) {
                    double wait = config_.flush_interval_seconds > 0.0
                                      ? config_.flush_interval_seconds
                                      : 3600.0;
                    if (!queue_.is_empty()) {
                        const double retry = queue_.seconds_until_next_attempt(now_unix_seconds());
                        if (retry > 0.0 && retry < wait) {
                            wait = std::max(retry, limits::min_retry_seconds);
                        }
                    }
                    cv_.wait_for(lock, std::chrono::duration<double>(wait), [this] {
                        return shutdown_requested_ || nudge_ || wake_ || !commands_.empty();
                    });
                }
                if (!commands_.empty()) {
                    command = std::move(commands_.front());
                    commands_.pop_front();
                } else {
                    force = nudge_;
                    wake = wake_;
                    nudge_ = false;
                    wake_ = false;
                    do_shutdown = shutdown_requested_;
                }
            }
            if (command) {
                command();
                update_status();
                continue;
            }
            if (do_shutdown) {
                break;
            }
            if (config_.flush_interval_seconds > 0.0 || force || wake) {
                flush_cycle(force);
            }
            update_status();
        }

        // Quit path: game_exited once, then one best effort flush bounded by
        // the shutdown deadline. A refused key gets no further attempt here.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.is_active() && config_.track_lifecycle_events && !exited_queued_) {
                exited_queued_ = true;
                Event event;
                event.name = "game_exited";
                event.timestamp = static_cast<long long>(now_unix_seconds());
                enqueue_locked(std::move(event));
            }
        }
        flush_cycle(true);
        update_status();
    }

    Config config_;
    std::unique_ptr<Transport> transport_;
    const std::string api_url_;
    const std::string platform_;
    StoragePaths paths_;
    std::string playtest_token_;
    std::string playtest_tags_;
    bool is_playtest_ = false;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Queue queue_;
    std::deque<std::function<void()>> commands_;
    std::vector<std::pair<std::string, bool>> pending_logs_;
    std::atomic<double> abort_at_{0.0};

    // Guarded by mutex_.
    std::string device_id_;
    std::string join_token_;
    std::string last_error_;
    bool device_id_saved_ = false;
    bool enabled_ = true;
    bool opted_out_ = false;
    bool stopped_ = false;
    Status stop_reason_ = Status::ok;
    Status transient_ = Status::ok;
    Status last_reported_ = Status::ok;
    bool settings_checked_ = false;
    bool started_queued_ = false;
    bool exited_queued_ = false;
    bool bad_event_warned_ = false;
    bool too_large_warned_ = false;
    int too_large_dropped_ = 0;
    bool inert_ = false;
    Consent consent_ = Consent::opt_out;
    std::string distribution_;
    std::string event_nonce_;
    unsigned long long event_counter_ = 0;
    long long start_unix_ = 0;
    bool storage_warned_ = false;
    unsigned long long identity_generation_ = 0;
    bool nudge_ = false;
    bool wake_ = false;
    bool shutdown_requested_ = false;

    std::thread worker_;
};

// --- Client facade ----------------------------------------------------------

Client::Client(Config config)
    : Client(std::move(config), nullptr) {}

Client::Client(Config config, std::unique_ptr<Transport> transport) {
    // No key: inert, so no transport is made (libcurl is never touched).
    if (!transport && !detail::trim(config.ingest_key).empty()) {
        transport = std::make_unique<CurlTransport>(config.request_timeout_seconds);
    }
    api_url_ = normalize_api_url(config.api_url);
    impl_ = std::make_unique<Impl>(std::move(config), std::move(transport), api_url_);
}

Client::~Client() = default;

bool Client::track(const std::string& event_name, const EventData& data) {
    return impl_->track(event_name, data);
}

void Client::flush() { impl_->flush(); }

bool Client::flush_and_wait(double timeout_seconds) {
    return impl_->flush_and_wait(timeout_seconds);
}

void Client::submit_feedback(std::string message, std::string category, int rating,
                             FeedbackCallback callback) {
    impl_->submit_feedback(std::move(message), std::move(category), rating, std::move(callback));
}

void Client::fetch_suggestions(SuggestionsCallback callback) {
    impl_->fetch_suggestions(std::move(callback));
}

void Client::set_enabled(bool enabled) { impl_->set_enabled(enabled); }
bool Client::is_enabled() const { return impl_->is_enabled(); }
bool Client::is_collecting() const { return impl_->is_collecting(); }
void Client::set_tracking_enabled(bool enabled) { impl_->set_tracking_enabled(enabled); }
bool Client::is_tracking_enabled_by_player() const { return impl_->is_tracking_enabled_by_player(); }
std::string Client::reset_device_id() { return impl_->reset_device_id(); }
Status Client::status() const { return impl_->status(); }
bool Client::tracking_enabled() const { return impl_->tracking_enabled(); }
bool Client::is_ready() const { return impl_->is_ready(); }
std::string Client::session_token() const { return impl_->session_token(); }
bool Client::is_playtest() const { return impl_->is_playtest(); }
std::string Client::device_id() const { return impl_->device_id(); }

Stats Client::stats() const { return impl_->stats(); }

void Client::shutdown() { impl_->shutdown(); }

} // namespace ravensight
