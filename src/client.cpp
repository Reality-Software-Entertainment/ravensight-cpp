// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "ravensight/client.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

#include "error_capture.hpp"
#include "delivery.hpp"
#include "health.hpp"
#include "ravensight/error_capture.hpp"
#include "remote_config.hpp"
#include "ravensight/curl_transport.hpp"
#include "ravensight/version.hpp"
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

long long now_unix_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool is_lower_alnum(char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9'); }

/// ^[a-z][a-z0-9_]{0,63}$, an experiment key.
bool valid_experiment_key(const std::string& key) {
    if (key.empty() || key.size() > 64 || !(key[0] >= 'a' && key[0] <= 'z')) return false;
    for (char ch : key) if (!is_lower_alnum(ch) && ch != '_') return false;
    return true;
}

/// ^[a-z][a-z0-9_-]{0,39}$, a variant name.
bool valid_variant(const std::string& variant) {
    if (variant.empty() || variant.size() > 40 || !(variant[0] >= 'a' && variant[0] <= 'z')) return false;
    for (char ch : variant) if (!is_lower_alnum(ch) && ch != '_' && ch != '-') return false;
    return true;
}

bool all_digits(const std::string& text, std::size_t max) {
    if (text.empty() || text.size() > max) return false;
    for (char ch : text) if (ch < '0' || ch > '9') return false;
    return true;
}

bool in_list(const std::string& value, const std::array<std::string_view, 6>& list) {
    for (std::string_view item : list) if (value == item) return true;
    return false;
}

bool in_list(const std::string& value, const std::array<std::string_view, 4>& list) {
    for (std::string_view item : list) if (value == item) return true;
    return false;
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

// --- Log routing and capture ownership --------------------------------------
//
// ravensight::log reaches one client: the first one built with a key. The
// route mutex is held while a line is delivered, so a client detaching in
// shutdown waits for a delivery in progress. Nothing that holds a client's
// own mutex ever takes the route mutex.

/// What a log line needs, decided before any scrubbing.
struct LogPlan {
    bool capture = false; // an error line and the caps have room
    bool trail = false;   // the trail switch is on
    long long ms = 0;     // since the client started, for the trail line
};

struct LogTarget {
    virtual ~LogTarget() = default;
    /// Cheap, under the client's lock: may this line be captured or kept?
    virtual LogPlan plan_host_log(LogLevel level) = 0;
    /// Under the client's lock, with everything already scrubbed.
    virtual void deliver_host_log(const detail::PreparedCapture* capture,
                                  std::string trail_line) = 0;
};

std::mutex g_route_mutex;
LogTarget* g_route = nullptr;

// The client holding the process wide hooks (the terminate handler).
std::atomic<const void*> g_capture_owner{nullptr};

// True on a thread while it hands the SDK's own lines to Config::log, so a
// host that routes Config::log back into ravensight::log makes no loop.
thread_local bool t_in_sdk_log = false;

bool is_sdk_line(std::string_view message) {
    return message.substr(0, 10) == "Ravensight";
}

} // namespace

const char* to_string(LogLevel level) {
    switch (level) {
        case LogLevel::debug:
            return "debug";
        case LogLevel::info:
            return "info";
        case LogLevel::warning:
            return "warning";
        case LogLevel::error:
            return "error";
        case LogLevel::critical:
            return "critical";
    }
    return "info";
}

bool parse_log_level(std::string_view name, LogLevel& out) {
    std::string lower(name);
    for (char& c : lower) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    lower = detail::trim(lower);
    if (lower == "debug" || lower == "trace") {
        out = LogLevel::debug;
    } else if (lower == "info") {
        out = LogLevel::info;
    } else if (lower == "warn" || lower == "warning") {
        out = LogLevel::warning;
    } else if (lower == "error" || lower == "err") {
        out = LogLevel::error;
    } else if (lower == "critical" || lower == "fatal") {
        out = LogLevel::critical;
    } else {
        return false;
    }
    return true;
}

void log(LogLevel level, std::string_view message) {
    if (t_in_sdk_log || message.empty() || is_sdk_line(message)) {
        return;
    }
    try {
        // 1. The caps and switches first, under the locks, without scrubbing.
        LogPlan plan;
        {
            std::lock_guard<std::mutex> lock(g_route_mutex);
            if (g_route == nullptr) {
                return;
            }
            plan = g_route->plan_host_log(level);
        }
        if (!plan.capture && !plan.trail) {
            return;
        }
        // 2. Scrubbing and the dedupe key outside every lock.
        const std::string text(message);
        detail::PreparedCapture capture;
        // A logged error is a warning by the contract, like a console error:
        // the game reported it and kept running.
        const bool has_capture =
            plan.capture &&
            detail::prepare_capture("warning", "log", "", text, "",
                                    static_cast<long long>(now_unix_seconds()), capture);
        std::string line =
            plan.trail ? detail::Trail::format_line(plan.ms, to_string(level), text) : std::string();
        // 3. Count against the per key cap and queue, under the locks again.
        std::lock_guard<std::mutex> lock(g_route_mutex);
        if (g_route != nullptr) {
            g_route->deliver_host_log(has_capture ? &capture : nullptr, std::move(line));
        }
    } catch (...) {
        // A failure inside the capture path is dropped, never reported.
    }
}

LogSink log_sink() {
    return [](LogLevel level, std::string_view message) { ravensight::log(level, message); };
}

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

std::string micros_from_decimal_string(const std::string& text) {
    return detail::micros_from_decimal_string(text);
}

std::string micros_from_double(double major_units) { return detail::micros_from_double(major_units); }

std::string map_ad_format(const std::string& label) { return detail::map_ad_format(label); }

namespace {
AdEvent shown_ad(std::string network, std::string format, std::string placement,
                 std::string micros, std::string currency) {
    AdEvent ad;
    ad.network = detail::trim(network);
    ad.format = std::move(format);
    ad.result = "shown";
    ad.placement = detail::trim(placement);
    ad.revenue_micros = std::move(micros);
    for (char& ch : currency) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    ad.currency = detail::trim(currency);
    return ad;
}
} // namespace

AdEvent ad_from_admob(long long value_micros, const std::string& currency_code,
                      const std::string& ad_source_name, const std::string& format,
                      const std::string& placement) {
    const std::string network = detail::trim(ad_source_name).empty() ? "admob" : ad_source_name;
    return shown_ad(network, map_ad_format(format), placement,
                    value_micros >= 0 ? std::to_string(value_micros) : std::string(), currency_code);
}

AdEvent ad_from_applovin_max(double revenue, const std::string& network_name,
                             const std::string& format_label, const std::string& placement) {
    return shown_ad(network_name, map_ad_format(format_label), placement, micros_from_double(revenue), "USD");
}

AdEvent ad_from_ironsource(double revenue, const std::string& ad_network, const std::string& ad_unit,
                           const std::string& placement) {
    return shown_ad(ad_network, map_ad_format(ad_unit), placement, micros_from_double(revenue), "USD");
}

AdEvent ad_from_topon(double publisher_revenue, const std::string& currency,
                      const std::string& network_name, const std::string& ad_type,
                      const std::string& placement) {
    return shown_ad(network_name, map_ad_format(ad_type), placement,
                    micros_from_double(publisher_revenue), currency);
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

struct PreparedReport {
    std::shared_ptr<int> owner;
    unsigned long long generation = 0;
    std::string payload, image, token, id;
    bool uploaded = false;
};

class Client::Impl : public LogTarget {
public:
    Impl(Config config, std::unique_ptr<Transport> transport, std::string api_url)
        : config_(std::move(config)),
          transport_(std::move(transport)),
          api_url_(std::move(api_url)),
          platform_(config_.platform.empty() ? detect_platform() : config_.platform) {
        // The worker is not running yet, so the *_locked helpers are safe
        // to call here without the mutex.
        queue_.set_max_queue_size(config_.max_queue_size);
        if (config_.performance) {
            // Process start to this call, where the platform tells us when
            // the process started.
            const double started = process_start_unix_seconds();
            const double now = now_unix_seconds();
            if (started > 0.0 && now >= started) {
                boot_ms_ = std::round((now - started) * 1000.0);
            }
        }
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

        capture_wanted_ = config_.error_capture;
        if (!is_playtest_ && !paths_.state_file.empty()) {
            const std::string dir = detail::parent_dir(paths_.state_file);
            crash_path_ = detail::join_path(dir, "crash");
            signal_path_ = detail::join_path(dir, "crash_signal");
            if (capture_wanted_) {
                // Read and scrubbed here, before the worker exists, so no
                // lock is held while scrubbing. Sent (and the files
                // deleted) only once capture installs.
                load_crash_records();
            }
            if (config_.persist_queue) {
                queue_path_ = detail::join_path(dir, detail::queue_file_name(detail::trim(config_.ingest_key)));
            }
            attribution_path_ = detail::join_path(dir, "attribution_sent");
        }
        device_ = build_device_json(config_.device);
        if (opted_out_) {
            // The player said no: nothing tracked before may be sent later.
            forget_delivery_files_locked();
        }

        apply_local_state_locked();
        if (queue_.is_active()) {
            on_locally_enabled_locked();
        }
        update_capture_locked();
        {
            std::lock_guard<std::mutex> route(g_route_mutex);
            if (g_route == nullptr) {
                g_route = this;
            }
        }
        worker_ = std::thread([this] { worker_main(); });
    }

    ~Impl() override { shutdown(); }

    // --- Automatic error capture ----------------------------------------------

    void set_error_capture(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        capture_wanted_ = enabled;
        update_capture_locked();
    }

    bool is_error_capture_active() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capture_installed_;
    }

    // Any thread, through ravensight::log, with the route mutex held.
    LogPlan plan_host_log(LogLevel level) override {
        std::lock_guard<std::mutex> lock(mutex_);
        LogPlan plan;
        if (!capture_installed_) {
            return plan;
        }
        plan.capture = (level == LogLevel::error || level == LogLevel::critical) &&
                       limiter_.has_room(now_unix_seconds());
        plan.trail = capture_trail_;
        plan.ms = ms_since_start();
        return plan;
    }

    void deliver_host_log(const detail::PreparedCapture* capture, std::string trail_line) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!capture_installed_) {
            return;
        }
        if (capture != nullptr) {
            capture_locked(*capture);
        }
        // The line itself is context for the next error.
        if (capture_trail_) {
            trail_.push(std::move(trail_line));
        }
    }

    bool track(const std::string& event_name, const EventData& data) {
        if (event_name.empty()) {
            return false;
        }
        Event event;
        event.name = event_name;
        EventData clean;
        for (const auto& member : data) {
            // A well-formed rs_exp_ tag from the game is the game's to keep;
            // a malformed one is removed (and the SDK's own, if any, used).
            if (member.first.rfind("rs_exp_", 0) == 0) {
                const std::string value = member.second.to_json();
                const bool ok = valid_experiment_key(member.first.substr(7)) && value.size() >= 2 &&
                                value.front() == '"' && value.back() == '"' &&
                                valid_variant(value.substr(1, value.size() - 2));
                if (!ok) continue;
            }
            clean.push_back(member);
        }
        event.data_json = write_fields(clean);
        event.timestamp = now_unix_ms();

        // The trail line is scrubbed before taking the lock.
        std::string trail_line;
        if (trail_live_.load() && event.data_json.find("\"rs_kind\":\"error\"") == std::string::npos) {
            trail_line = detail::Trail::format_line(ms_since_start(), "event", event.name);
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_ || !queue_.is_active()) {
            return false;
        }
        if (capture_installed_ && capture_trail_ && !trail_line.empty()) {
            trail_.push(std::move(trail_line));
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

    void sample_frame(double dt_seconds) {
        if (config_.performance) {
            health_.add_frame(dt_seconds);
        }
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

    ReportHandle prepare_report(const std::string& id, const std::string& message,
                                const std::string& category, const std::string& image) {
        auto safe = [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'; };
        if (id.size() < 8 || id.size() > 80 || !std::all_of(id.begin(), id.end(), safe) ||
            detail::trim(message).empty() || message.size() > 500 || !is_feedback_category(category) || image.size() > 1000000) return nullptr;
        if (!image.empty() && !(image.compare(0, 4, "\x89PNG", 4) == 0 || image.compare(0, 3, "\xff\xd8\xff", 3) == 0 ||
            (image.size() >= 12 && image.compare(0, 4, "RIFF") == 0 && image.compare(8, 4, "WEBP") == 0))) return nullptr;
        auto report = std::make_shared<PreparedReport>();
        { std::lock_guard<std::mutex> lock(mutex_); report->owner = report_owner_; report->generation = identity_generation_; }
        json::ObjectWriter body;
        body.add("clientId", id).add("message", detail::trim(message)).add("category", category)
            .add("capturedAt", static_cast<long long>(now_unix_seconds()));
        if (!image.empty()) body.add("image", true).add("bytes", static_cast<long long>(image.size())).add("sha256", detail::sha256_hex(image));
        report->payload = body.str(); report->image = image;
        return report;
    }

    void report_command(ReportHandle report, const std::string& operation, ReportCallback callback) {
        bool accepted = false;
        { std::lock_guard<std::mutex> lock(mutex_);
          if (report && report->owner == report_owner_ && report->generation == identity_generation_ && queue_.is_active() && !shutdown_requested_ && !is_playtest_) {
            commands_.push_back([this, report, operation, callback] { auto result = run_report(report, operation); if (callback) callback(result); });
            cv_.notify_all(); accepted = true;
          }
        }
        if (!accepted && callback) callback(ReportResult{false, "report_unavailable", "", "", "", 0});
    }

    ReportResult run_report(const ReportHandle& report, const std::string& operation) {
        auto valid = [&] { std::lock_guard<std::mutex> lock(mutex_); return report->generation == identity_generation_ && queue_.is_active() && !shutdown_requested_ && !is_playtest_; };
        auto failed = [](const std::string& code, long status = 0) { return ReportResult{false, code, "", "", "", status}; };
        if (!valid()) return failed("report_session_changed");
        if (report->token.empty()) {
            if (operation != "submit" || !ensure_session(false) || !valid()) return failed("session_required");
            std::lock_guard<std::mutex> lock(mutex_); report->token = queue_.session_token();
        }
        auto send = [&](const char* method, const std::string& path, const std::string& body = "") {
            if (!valid()) return Response::network_failure("report_session_changed");
            Request request = make_request(method, path.c_str(), false);
            request.with_header("X-Session-Token", report->token).with_header("Content-Type", "application/json"); request.body = body;
            return transport_->send(request);
        };
        if (operation != "submit" && report->id.empty()) return failed("report_not_submitted");
        Response response = report->id.empty() ? send("POST", "/reports", report->payload)
            : send(operation == "cancel" ? "DELETE" : "GET", "/reports/" + report->id);
        auto receipt = [&] {
            if (!valid()) return failed("report_session_changed");
            if (response.status < 200 || response.status >= 300) return failed(response.error_code("report_failed"), response.status);
            const auto body = response.parse_json();
            const auto id = body.get_string("id", ""), status = body.get_string("status", "");
            if (id.size() != 24 || id.find_first_not_of("0123456789abcdef") != std::string::npos ||
                (status != "pending" && status != "processing" && status != "complete" && status != "canceled")) return failed("invalid_response");
            if (!report->id.empty() && report->id != id) return failed("invalid_response");
            report->id = id;
            return ReportResult{true, "", id, status, body.get_string("reportId", ""), response.status};
        };
        auto result = receipt();
        if (!result.ok || operation != "submit" || report->image.empty() || result.status != "pending") return result;
        if (!report->uploaded) {
            response = send("POST", "/reports/" + report->id + "/upload", "{}");
            if (!valid()) return failed("report_session_changed");
            if (response.status != 200) return failed("upload_authorization_failed", response.status);
            const auto grant = response.parse_json(); const auto url = grant.get_string("url", ""); const auto* fields = grant.find("fields");
            if (url.size() < 9) return failed("invalid_upload");
            const auto authority_end = url.find('/', 8); const auto authority = url.substr(8, authority_end == std::string::npos ? std::string::npos : authority_end - 8);
            if (url.compare(0, 8, "https://") != 0 || authority.empty() || authority.find('@') != std::string::npos ||
                url.find_first_of("\r\n\t \\") != std::string::npos || !fields || !fields->is_object() || fields->size() > 32) return failed("invalid_upload");
            const std::string boundary = "rs-" + detail::sha256_hex(report->payload + report->image);
            std::string multipart;
            for (const auto& field : fields->members()) {
                if (field.first.empty() || field.first.size() > 100 || field.first.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos || !field.second.is_string() || field.second.as_string().size() > 16000) return failed("invalid_upload");
                multipart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"" + field.first + "\"\r\n\r\n" + field.second.as_string() + "\r\n";
            }
            multipart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"screenshot\"\r\nContent-Type: application/octet-stream\r\n\r\n" + report->image + "\r\n--" + boundary + "--\r\n";
            Request upload; upload.method = "POST"; upload.url = url; upload.body = std::move(multipart); upload.abort_at_unix_seconds = &abort_at_;
            upload.with_header("Content-Type", "multipart/form-data; boundary=" + boundary);
            if (!valid()) return failed("report_session_changed");
            response = transport_->send(upload);
            if (!valid()) return failed("report_session_changed");
            if (response.status < 200 || response.status >= 300) return failed("upload_failed", response.status);
            report->uploaded = true;
        }
        response = send("POST", "/reports/" + report->id + "/finalize", "{}");
        return receipt();
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

    void fetch_config() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.is_active() || shutdown_requested_ || config_pending_) return;
        config_pending_ = true;
        const auto generation = identity_generation_;
        commands_.push_back([this, generation] { run_config(generation); });
        cv_.notify_all();
    }

    // A typed read: the value, and exposure for every assigned experiment
    // not yet exposed this run (one rs_exposure event for them).
    json::Value config_value(const std::string& key) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!queue_.is_active()) return {};
        expose_locked();
        const auto* value = remote_values_.find(key);
        return value ? *value : json::Value{};
    }

    void expose_locked() {
        std::vector<std::pair<std::string, std::string>> fresh;
        for (const auto& experiment : remote_experiments_.items()) {
            const std::string key = experiment.get_string("key", "");
            const std::string variant = experiment.get_string("variant", "");
            if (!valid_experiment_key(key) || !valid_variant(variant)) continue;
            bool known = false;
            for (const auto& exposed : exposed_) known = known || exposed.first == key;
            if (!known) fresh.emplace_back(key, variant);
        }
        if (fresh.empty() || shutdown_requested_) return;
        exposed_.insert(exposed_.end(), fresh.begin(), fresh.end());
        Event event;
        event.name = "rs_exposure";
        event.timestamp = now_unix_ms();
        enqueue_locked(std::move(event));
    }

    // A fetch replaced the assignments: a stopped experiment is no longer
    // tagged, and a changed variant is tagged as now assigned.
    void prune_exposed_locked() {
        std::vector<std::pair<std::string, std::string>> kept;
        for (const auto& exposed : exposed_) {
            for (const auto& experiment : remote_experiments_.items()) {
                if (experiment.get_string("key", "") == exposed.first) {
                    kept.emplace_back(exposed.first, experiment.get_string("variant", ""));
                }
            }
        }
        exposed_.swap(kept);
    }

    void clear_config_locked() {
        remote_values_ = {}; remote_experiments_ = {}; remote_etag_.clear();
        exposed_.clear();
        config_pending_ = false; ++identity_generation_;
    }

    std::string config_cache_path_locked() const {
        if (is_playtest_ || paths_.state_file.empty()) return {};
        json::ObjectWriter scope;
        scope.add("endpoint", api_url_).add("key", config_.ingest_key).add("device", device_id_)
             .add("platform", platform_).add("build", config_.game_version);
        return detail::join_path(detail::parent_dir(paths_.state_file), "config_" + detail::sha256_hex(scope.str()) + ".json");
    }

    void run_config(unsigned long long generation) {
        std::string cache_path, etag, device;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != identity_generation_ || !queue_.is_active() || shutdown_requested_) return;
            cache_path = config_cache_path_locked();
            device = device_id_;
            if (remote_etag_.empty() && !cache_path.empty()) {
                detail::ConfigSnapshot cached;
                if (detail::read_config_cache(cache_path, cached)) {
                    remote_values_ = cached.values; remote_experiments_ = cached.experiments; remote_etag_ = cached.etag;
                    prune_exposed_locked();
                }
            }
            etag = remote_etag_;
        }
        const std::string path = "/ingest/config?deviceId=" + detail::url_component(device) +
            "&platform=" + detail::url_component(platform_) + "&gameVersion=" + detail::url_component(config_.game_version);
        auto request = make_request("GET", path.c_str(), true);
        if (!etag.empty()) request.with_header("If-None-Match", etag);
        const auto response = transport_->send(request);
        detail::ConfigSnapshot parsed;
        const bool fresh = response.status == 200 && detail::parse_config(response.body, parsed);
        bool ok = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != identity_generation_ || !queue_.is_active() || shutdown_requested_) return;
            config_pending_ = false;
            if (fresh) {
                remote_values_ = parsed.values; remote_experiments_ = parsed.experiments; remote_etag_ = parsed.etag;
                prune_exposed_locked();
                if (!cache_path.empty()) detail::write_text_file(cache_path, response.body);
            }
            ok = fresh || (response.status == 304 && !etag.empty());
        }
        if (response.status == 401 || response.status == 403) {
            handle_refusal(response);
            std::lock_guard<std::mutex> lock(mutex_);
            if (!queue_.is_active()) return;
        }
        if (config_.on_config_ready) config_.on_config_ready(ok);
    }

    void set_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool was_active = queue_.is_active();
        enabled_ = enabled;
        apply_local_state_locked();
        if (!was_active && queue_.is_active()) {
            on_locally_enabled_locked();
        }
        update_capture_locked();
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
        if (!enabled) {
            forget_delivery_files_locked();
        }
        if (!was_active && queue_.is_active()) {
            on_locally_enabled_locked();
        }
        update_capture_locked();
        if (!enabled) {
            // The player said no: a crash saved earlier is never sent.
            discard_crash_records_locked();
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
            clear_config_locked();
            reset_capture_run_locked();
            discard_crash_records_locked();
            forget_delivery_files_locked();
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
        clear_config_locked();
        reset_capture_run_locked();
        // A crash saved under the old id is never sent.
        discard_crash_records_locked();
        forget_delivery_files_locked();
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
            // Outside mutex_: a delivery in progress finishes first.
            std::lock_guard<std::mutex> route(g_route_mutex);
            if (g_route == this) {
                g_route = nullptr;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!shutdown_requested_) {
                shutdown_requested_ = true;
                abort_at_.store(now_unix_seconds() + shutdown_hold_seconds);
            }
            // Nothing reinstalls after this: update_capture_locked checks
            // shutdown_requested_.
            update_capture_locked();
            cv_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    // --- Ads and attribution ------------------------------------------------

    bool track_ad(const AdEvent& ad) {
        const std::string network = detail::trim(ad.network);
        const std::string placement = detail::trim(ad.placement);
        std::string currency = detail::trim(ad.currency);
        for (char& ch : currency) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const bool has_revenue = !ad.revenue_micros.empty();
        bool currency_ok = currency.size() == 3;
        for (char ch : currency) currency_ok = currency_ok && ch >= 'A' && ch <= 'Z';
        if (network.empty() || detail::utf16_units(network) > 64 || !in_list(ad.format, ad_formats) ||
            !in_list(ad.result, ad_results) || detail::utf16_units(placement) > 100 ||
            (has_revenue && (!all_digits(ad.revenue_micros, 18) || !currency_ok || ad.result != "shown"))) {
            note("ad event refused locally: check network, format, result, and that revenue comes "
                 "with a currency on a shown result");
            return false;
        }
        EventData data{{"rs_kind", "ad"},
                       {"rs_ad_network", network},
                       {"rs_ad_format", ad.format},
                       {"rs_ad_result", ad.result}};
        if (!placement.empty()) data.emplace_back("rs_ad_placement", placement);
        if (has_revenue) {
            data.emplace_back("rs_revenue_micros", ad.revenue_micros);
            data.emplace_back("rs_currency", currency);
        }
        return track("rs_ad", data);
    }

    bool set_attribution(const Attribution& attribution) {
        const std::string source = detail::trim(attribution.source);
        if (source.empty() || detail::utf16_units(source) > 64) {
            note("attribution refused locally: source must be 1 to 64 characters");
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::string flag;
            if (attribution_sent_ ||
                (!attribution_path_.empty() && detail::read_text_file(attribution_path_, flag))) {
                attribution_sent_ = true;
                return false;
            }
        }
        EventData data{{"rs_kind", "attribution"}, {"rs_attr_source", source}};
        const std::pair<const char*, const std::string*> fields[] = {
            {"rs_attr_network", &attribution.network}, {"rs_campaign", &attribution.campaign},
            {"rs_adset", &attribution.adset}, {"rs_creative", &attribution.creative}};
        for (const auto& field : fields) {
            const std::string value = detail::trim(*field.second);
            const std::size_t max = std::string(field.first) == "rs_attr_network" ? 100 : 200;
            if (value.empty()) continue;
            if (detail::utf16_units(value) > max) {
                note(std::string("attribution refused locally: ") + field.first + " is too long");
                return false;
            }
            data.emplace_back(field.first, value);
        }
        if (attribution.touch_time_ms > 0) data.emplace_back("rs_touch_time", attribution.touch_time_ms);
        if (!track("rs_attribution", data)) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        attribution_sent_ = true;
        if (!attribution_path_.empty()) {
            detail::write_text_file(attribution_path_, "sent\n");
        }
        return true;
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
        t_in_sdk_log = true;
        for (const auto& line : lines) {
            const std::string text = "Ravensight: " + line.first;
            if (config_.log) {
                config_.log(text);
            } else if (line.second) {
                std::fprintf(stderr, "%s\n", text.c_str());
            }
        }
        t_in_sdk_log = false;
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
        if (!enabled_ || opted_out_) clear_config_locked();
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
        detail::ConfigSnapshot cached;
        const auto path = config_cache_path_locked();
        if (!path.empty() && detail::read_config_cache(path, cached)) {
            remote_values_ = cached.values; remote_experiments_ = cached.experiments; remote_etag_ = cached.etag;
            prune_exposed_locked();
        }
        if (!delivery_loaded_ && queue_.is_active()) {
            delivery_loaded_ = true;
            load_queue_locked();
            loaded_through_sequence_ = queue_.last_sequence();
        }
        if (config_.track_lifecycle_events && !started_queued_ && queue_.is_active()) {
            started_queued_ = true;
            Event event;
            event.name = "game_started";
            event.timestamp = now_unix_ms();
            enqueue_locked(std::move(event));
        }
    }

    // --- The persisted queue ----------------------------------------------------

    void load_queue_locked() {
        if (!queue_path_.empty()) {
            // A write a crash cut short leaves its temporary file behind.
            detail::remove_temp_files(queue_path_);
        }
        std::string text;
        if (queue_path_.empty() || !detail::read_text_file(queue_path_, text)) {
            return;
        }
        detail::LoadedQueue loaded = detail::read_queue_file(text, now_unix_ms(), queue_.max_queue_size());
        for (Event& event : loaded.events) {
            queue_.enqueue(std::move(event));
        }
        queue_.count_dropped(loaded.too_old + loaded.over_cap);
        if (loaded.too_old + loaded.over_cap > 0) {
            note_locked("dropped " + std::to_string(loaded.too_old + loaded.over_cap) +
                            " saved event(s) older than 89 days or past the queue cap",
                        false);
        } else {
            persisted_revision_ = queue_.revision();
        }
        if (!loaded.events.empty()) {
            note_locked("loaded " + std::to_string(loaded.events.size()) + " unsent event(s)", false);
        }
    }

    // Writes the queue file when the queue changed since the last write: the
    // whole queue, atomically, or no file when it is empty. Never after the
    // run was stopped (the events are kept for the next launch).
    void persist_locked() {
        if (queue_path_.empty() || persist_frozen_ || queue_.revision() == persisted_revision_) {
            return;
        }
        bool ok = false;
        if (queue_.is_empty()) {
            ok = detail::remove_file(queue_path_);
            detail::remove_temp_files(queue_path_);
        } else {
            ok = detail::write_text_file(queue_path_, detail::write_queue_file(queue_.events()));
        }
        persisted_revision_ = queue_.revision();
        if (!ok && !persist_warned_) {
            persist_warned_ = true;
            note_locked("could not write the queue file " + queue_path_ +
                            "; unsent events are kept in memory only",
                        true);
        }
    }

    // The opt-out or a device id reset: nothing tracked before may be sent
    // later, and the origin stamp must not outlive the queue.
    void forget_delivery_files_locked() {
        if (!queue_path_.empty()) {
            detail::remove_file(queue_path_);
            detail::remove_temp_files(queue_path_);
        }
        if (!attribution_path_.empty()) detail::remove_file(attribution_path_);
        attribution_sent_ = false;
        exposed_.clear();
        persisted_revision_ = queue_.revision();
    }

    static std::string build_device_json(const DeviceInfo& device) {
        json::ObjectWriter writer;
        bool any = false;
        const std::pair<const char*, const std::string*> fields[] = {
            {"model", &device.model}, {"osVersion", &device.os_version}, {"gpu", &device.gpu}};
        for (const auto& field : fields) {
            const std::string value = detail::clean_device_field(*field.second);
            if (!value.empty()) {
                writer.add(field.first, value);
                any = true;
            }
        }
        return any ? writer.str() : std::string();
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
        // Append without reparsing: preserve exact integer payloads and the
        // assignment at enqueue time, including across retry attempts. Only
        // experiments exposed this run are tagged, and never over a tag the
        // game set itself.
        for (const auto& exposed : exposed_) {
            const std::string key = json::escape_string("rs_exp_" + exposed.first);
            if (event.data_json.find(key + ":") == std::string::npos) {
                append_member(event.data_json, key + ":" + json::escape_string(exposed.second));
            }
        }
        if (event.origin_json.empty()) {
            event.origin_json = current_origin_;
        }
        if (!playtest_tags_.empty()) {
            append_member(event.data_json, playtest_tags_);
        }
        ++event_counter_;
        append_member(event.data_json,
                      "\"rs_event_id\":" +
                          json::escape_string(make_event_id(device_id_, start_unix_, event_nonce_,
                                                            event_counter_)));
        if (detail::counted_event_bytes(event.data_json) > limits::max_event_bytes) {
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
        const bool was_clean = queue_.revision() == persisted_revision_;
        const int dropped = queue_.enqueue(std::move(event));
        if (dropped > 0) {
            note_locked("queue full, dropped " + std::to_string(dropped) + " oldest event(s)", false);
        }
        if (was_clean && !queue_path_.empty()) {
            // Ask the worker to write the queue file within a second.
            ++persist_kicks_;
            cv_.notify_all();
        }
        return true;
    }

    // --- Automatic error capture, see docs/errors/CONTRACT.md ------------------

    long long ms_since_start() const {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - run_start_)
                                          .count());
    }

    // Installs or removes the hooks so they run exactly while there is a
    // key, consent, the client may send, the game's own Config switch is on
    // and the server's switch is not known to be off. Before the server's
    // answer they run too, holding what they capture.
    void update_capture_locked() {
        const bool wanted = !inert_ && !shutdown_requested_ && capture_wanted_ && queue_.is_active() &&
                            (!capture_known_ || capture_server_enabled_);
        if (wanted && !capture_installed_) {
            install_capture_locked();
        } else if (!wanted && capture_installed_) {
            uninstall_capture_locked();
        }
        if (!capture_installed_) {
            held_captures_.clear();
            trail_.clear();
        }
        trail_live_.store(capture_installed_ && capture_trail_);
    }

    void install_capture_locked() {
        const void* expected = nullptr;
        if (!g_capture_owner.compare_exchange_strong(expected, this) && expected != this) {
            if (!capture_conflict_noted_) {
                capture_conflict_noted_ = true;
                note_locked("another Client holds automatic error capture in this process; "
                            "this one does not capture",
                            false);
            }
            return;
        }
        capture_installed_ = true;
        if (!crash_path_.empty()) {
            detail::install_terminate_handler(crash_path_);
            if (config_.install_signal_handlers) {
                detail::install_signal_handlers(signal_path_);
            }
        }
        send_crash_records_locked();
    }

    void uninstall_capture_locked() {
        if (!crash_path_.empty()) {
            detail::uninstall_terminate_handler();
            if (config_.install_signal_handlers) {
                detail::uninstall_signal_handlers();
            }
        }
        const void* self = this;
        g_capture_owner.compare_exchange_strong(self, nullptr);
        capture_installed_ = false;
        held_captures_.clear();
        trail_.clear();
    }

    // What the previous run saved as it died, prepared once at startup. A
    // record older than 30 days is never sent; its file goes with the rest.
    void load_crash_records() {
        std::string text;
        detail::CrashRecord record;
        detail::PreparedCapture capture;
        const long long now = static_cast<long long>(now_unix_seconds());
        const auto fresh = [now](const detail::CrashRecord& r) {
            return r.timestamp <= 0 || now - r.timestamp <= detail::max_crash_record_age_seconds;
        };
        if (detail::read_text_file(crash_path_, text) && detail::parse_crash(text, record) && fresh(record) &&
            detail::prepare_capture("critical", "crash", record.type, record.message, "",
                                    record.timestamp, capture)) {
            pending_crashes_.push_back(capture);
        }
        if (detail::read_text_file(signal_path_, text) && detail::parse_signal_record(text, record) && fresh(record) &&
            detail::prepare_capture("critical", "crash", record.type, record.message, "",
                                    record.timestamp, capture)) {
            pending_crashes_.push_back(capture);
        }
        crash_files_pending_ = true;
    }

    // One event each, through the same caps and boot hold as anything else
    // (so discarded unsent when the answer says capture is off). The files
    // are removed here, so a crash is sent at most once.
    void send_crash_records_locked() {
        if (!crash_files_pending_) {
            return;
        }
        crash_files_pending_ = false;
        detail::remove_file(crash_path_);
        detail::remove_file(signal_path_);
        std::vector<detail::PreparedCapture> crashes;
        crashes.swap(pending_crashes_);
        for (const auto& capture : crashes) {
            capture_locked(capture);
        }
    }

    void discard_crash_records_locked() {
        pending_crashes_.clear();
        crash_files_pending_ = false;
        if (!crash_path_.empty()) {
            detail::remove_file(crash_path_);
            detail::remove_file(signal_path_);
        }
    }

    // A new identity starts a new run's caps and trail.
    void reset_capture_run_locked() {
        limiter_.reset();
        trail_.clear();
        held_captures_.clear();
    }

    // One automatic capture, already scrubbed (prepare_capture, outside the
    // lock): counted against the caps, then held (before the server's
    // answer) or queued behind gameplay events.
    void capture_locked(const detail::PreparedCapture& capture) {
        if (!capture_installed_) {
            return;
        }
        if (!limiter_.allow(capture.key, now_unix_seconds())) {
            return; // over a device cap: dropped silently
        }
        const std::string& severity = capture.severity;
        const std::string& source = capture.source;
        const std::string& message = capture.message;
        const std::string& error_type = capture.type;
        const std::string& stack = capture.stack;
        const long long timestamp = capture.timestamp;
        EventData data{{"rs_kind", "error"},
                       {"rs_severity", severity},
                       {"rs_source", source},
                       {"rs_message", message}};
        if (!error_type.empty()) {
            data.emplace_back("rs_error_type", error_type);
        }
        if (!stack.empty()) {
            data.emplace_back("rs_stack", stack);
        }
        if (capture_trail_ && !trail_.empty() && limiter_.take_trail()) {
            data.emplace_back("rs_trail", trail_.text());
        }
        Event event;
        event.name = "error";
        event.data_json = write_fields(data);
        // Capture records keep unix seconds; queued events are milliseconds.
        event.timestamp = timestamp > 0 ? timestamp * 1000 : now_unix_ms();
        event.automatic_error = true;
        if (!capture_known_) {
            // A boot error: held until the game's switch is known.
            if (held_captures_.size() < detail::max_held_captures) {
                held_captures_.push_back(std::move(event));
            }
            return;
        }
        enqueue_locked(std::move(event));
    }

    // errorCapture: {enabled, trail} from a settings or session answer.
    // Absent leaves a known switch alone and otherwise means the default:
    // enabled, no trail. Held boot errors are then queued or discarded.
    void apply_capture_answer_locked(const json::Value& answer) {
        const json::Value* value = answer.find("errorCapture");
        if (value != nullptr && value->is_object()) {
            bool enabled = true;
            capture_server_enabled_ = !(value->get_bool("enabled", enabled) && !enabled);
            bool trail = false;
            capture_trail_ = value->get_bool("trail", trail) && trail;
            capture_known_ = true;
        } else if (!capture_known_) {
            capture_server_enabled_ = true;
            capture_trail_ = false;
            capture_known_ = true;
        }
        if (!capture_trail_) {
            trail_.clear();
        }
        trail_live_.store(capture_installed_ && capture_trail_);
        std::vector<Event> held;
        held.swap(held_captures_);
        update_capture_locked();
        if (capture_installed_ && queue_.is_active()) {
            for (Event& event : held) {
                enqueue_locked(std::move(event));
            }
        }
    }

    // --- Failure handling ---------------------------------------------------

    // Stops sending for the rest of the run and clears the queue.
    void stop_locked(Status reason, const std::string& error_code, const std::string& message) {
        if (stopped_) {
            return;
        }
        clear_config_locked();
        // The events stay in the queue file for the next launch.
        persist_locked();
        persist_frozen_ = true;
        stopped_ = true;
        stop_reason_ = reason;
        last_error_ = error_code.empty() ? to_string(reason) : error_code;
        queue_.set_server_tracking_enabled(false);
        queue_.invalidate_session();
        update_capture_locked();
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
                return;
            }
            apply_capture_answer_locked(response.parse_json());
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
            body.add("sdk", "cpp/" RAVENSIGHT_VERSION);
            if (!join_token.empty()) {
                body.add("joinToken", join_token);
            }
            if (!distribution_.empty()) {
                body.add("distribution", distribution_);
            }
            if (!device_.empty()) {
                body.add_raw("device", device_);
            }

            Request request = make_request("POST", "/session", true);
            request.body = body.str();
            request.with_header("Content-Type", "application/json");
            const Response response = transport_->send(request);

            if (response.status == 201) {
                const json::Value parsed = response.parse_json();
                const std::string token = parsed.get_string("token", "");
                std::unique_lock<std::mutex> lock(mutex_);
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
                apply_capture_answer_locked(parsed);
                if (join_token_ == join_token) {
                    join_token_.clear();
                }
                // The session's origin, kept exactly as received: events
                // tracked under it carry it, so a later launch can send them
                // as this session's.
                const json::Value* origin = parsed.find("origin");
                current_origin_ = origin != nullptr && origin->is_object() ? origin->dump() : "";
                current_session_id_ = detail::origin_session(current_origin_);
                if (!current_origin_.empty()) {
                    queue_.stamp_origin(loaded_through_sequence_, current_origin_);
                }
                // A public key's signing secret: memory only, never saved.
                signing_secret_.clear();
                clock_offset_seconds_ = 0;
                const json::Value* signing = parsed.find("signing");
                if (signing != nullptr && signing->is_object() &&
                    signing->get_string("algorithm", "") == "HMAC-SHA256") {
                    signing_secret_ = signing->get_string("secret", "");
                    double server_time = 0;
                    if (signing->get_number("serverTime", server_time) && server_time > 0) {
                        clock_offset_seconds_ =
                            static_cast<long long>(std::llround(server_time - now_unix_seconds()));
                    }
                }
                transient_ = Status::ok;
                note_locked("session ready", false);
                if (!config_pending_ && !shutdown_requested_) {
                    config_pending_ = true;
                    lock.unlock();
                    run_config(generation);
                }
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

    /// The batch body. An event tracked under an earlier session than the
    /// current one carries that session's origin, copied verbatim.
    static std::string build_batch_body(const std::vector<Event>& batch,
                                        const std::string& current_session_id) {
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
            if (!batch[i].origin_json.empty() &&
                detail::origin_session(batch[i].origin_json) != current_session_id) {
                body += ",\"origin\":";
                body += batch[i].origin_json;
            }
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

            std::string session_id;
            std::string secret;
            long long offset = 0;
            bool plain_only = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                session_id = current_session_id_;
                secret = signing_secret_;
                offset = clock_offset_seconds_;
                plain_only = gzip_refused_;
            }
            const std::string body = build_batch_body(plan.batch, session_id);
            Request request = make_request("POST", "/track/batch", false);
            request.with_header("Content-Type", "application/json");
            request.with_header("X-Session-Token", token);
            if (!secret.empty()) {
                // Over the exact bytes, before compression.
                const long long timestamp =
                    static_cast<long long>(std::floor(now_unix_seconds())) + offset;
                request.with_header("X-Ravensight-Timestamp", std::to_string(timestamp));
                request.with_header("X-Ravensight-Signature",
                                    detail::signature_header(secret, timestamp, body));
            }
            bool compressed = false;
            if (!plain_only && body.size() >= detail::gzip_threshold_bytes) {
                std::string gzipped = detail::gzip_compress(body);
                if (!gzipped.empty()) {
                    request.body = std::move(gzipped);
                    request.with_header("Content-Encoding", "gzip");
                    compressed = true;
                }
            }
            if (!compressed) {
                request.body = body;
            }

            const Response response = transport_->send(request);
            const std::size_t batch_size = plan.batch.size();

            if (response.status >= 200 && response.status < 300) {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.on_batch_accepted(plan);
                // The removal is on disk before anything else happens.
                persist_locked();
                queue_.end_flush();
                transient_ = Status::ok;
                reauth_attempts = 0;
                continue;
            }

            if (response.status == 401) {
                // Session expired or revoked, or a signature refused. The
                // batch stays queued and is re-sent, with the same event ids,
                // under a fresh session opened once; a second 401 in a row
                // waits for the backoff.
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.on_unauthorized();
                queue_.end_flush();
                if (++reauth_attempts > 1) {
                    queue_.schedule_backoff(now_unix_seconds());
                    record_transient_locked(Status::session_failed, response);
                    note_locked("session refused twice in a row, backing off", false);
                    return;
                }
                note_locked("session refused (" + response.error_code("unauthorized") +
                                "), opening a new one",
                            false);
                continue;
            }

            if (response.status == 403) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.end_flush();
                }
                // The events stay in the queue file for the next launch.
                handle_refusal(response);
                return;
            }

            if (response.status == 415 && compressed) {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.end_flush();
                gzip_refused_ = true;
                note_locked("compressed body refused (HTTP 415), sending uncompressed", false);
                continue;
            }

            if (response.status == 400 || response.status == 413 || response.status == 415) {
                const json::Value parsed = response.parse_json();
                const std::string code = parsed.get_string("error", "bad_request");
                double index = -1;
                const bool indexed = response.status == 400 && parsed.get_number("index", index) &&
                                     index >= 0 && index < static_cast<double>(batch_size);
                const bool too_large = response.status == 413 || code == "batch_too_large";
                std::lock_guard<std::mutex> lock(mutex_);
                int dropped = 0;
                if (indexed) {
                    dropped = queue_.drop_indexed(plan, static_cast<std::size_t>(index)) ? 1 : 0;
                } else if (too_large && batch_size > 1) {
                    queue_.on_bad_request(batch_size);
                    note_locked("batch too large (HTTP " + std::to_string(response.status) +
                                    "), splitting",
                                false);
                } else {
                    dropped = queue_.drop_batch(plan);
                }
                persist_locked();
                queue_.end_flush();
                if (dropped > 0) {
                    last_error_ = code;
                    if (!bad_event_warned_) {
                        bad_event_warned_ = true;
                        note_locked("dropped " + std::to_string(dropped) +
                                        " event(s) the server refused with HTTP " +
                                        std::to_string(response.status) + " (" + code +
                                        "); counted in stats().dropped",
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

        const double interval =
            config_.flush_interval_seconds > 0.0 ? config_.flush_interval_seconds : 3600.0;
        double next_timer_flush = now_unix_seconds() + interval;
        for (;;) {
            // With the Microsoft runtime std::set_terminate is per thread:
            // the worker gets the handler too, once, while it is armed.
            detail::install_terminate_handler_on_this_thread();
            std::function<void()> command;
            bool do_shutdown = false;
            bool force = false;
            bool wake = false;
            bool persist_only = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                persist_locked();
                if (commands_.empty() && !shutdown_requested_ && !nudge_ && !wake_) {
                    const double now = now_unix_seconds();
                    double wait = std::max(0.0, next_timer_flush - now);
                    if (!queue_.is_empty()) {
                        const double retry = queue_.seconds_until_next_attempt(now);
                        if (retry > 0.0 && retry < wait) {
                            wait = std::max(retry, limits::min_retry_seconds);
                        }
                    }
                    const unsigned long long kicks = persist_kicks_;
                    const bool fired = cv_.wait_for(lock, std::chrono::duration<double>(wait), [&] {
                        return shutdown_requested_ || nudge_ || wake_ || !commands_.empty() ||
                               persist_kicks_ != kicks;
                    });
                    if (fired && persist_kicks_ != kicks && !shutdown_requested_ && !nudge_ &&
                        !wake_ && commands_.empty()) {
                        // Only a new event to save: give it a second to
                        // collect company, then write once.
                        cv_.wait_for(lock, std::chrono::duration<double>(
                                               std::min(1.0, std::max(0.0, next_timer_flush - now_unix_seconds()))),
                                     [&] { return shutdown_requested_ || nudge_ || wake_ || !commands_.empty(); });
                        persist_locked();
                        persist_only = now_unix_seconds() < next_timer_flush && !shutdown_requested_ &&
                                       !nudge_ && !wake_ && commands_.empty();
                    }
                }
                if (!persist_only) {
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
            }
            if (persist_only) {
                continue;
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
            next_timer_flush = now_unix_seconds() + interval;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                persist_locked();
            }
            update_status();
        }

        // Quit path: game_exited once, then one best effort flush bounded by
        // the shutdown deadline. A refused key gets no further attempt here.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.is_active() && config_.performance && !health_queued_) {
                health_queued_ = true;
                double fps_median = 0.0;
                double fps_p10 = 0.0;
                if (health_.summarize(fps_median, fps_p10)) {
                    EventData summary{{"rs_kind", "health"},
                                      {"rs_fps_median", fps_median},
                                      {"rs_fps_p10", fps_p10}};
                    const double memory_mb = process_memory_mb();
                    if (memory_mb >= 0.0) {
                        summary.emplace_back("rs_memory_mb", memory_mb);
                    }
                    if (boot_ms_ >= 0.0) {
                        summary.emplace_back("rs_boot_ms", boot_ms_);
                    }
                    Event event;
                    event.name = "health";
                    event.data_json = write_fields(summary);
                    event.timestamp = now_unix_ms();
                    enqueue_locked(std::move(event));
                }
            }
            if (queue_.is_active() && config_.track_lifecycle_events && !exited_queued_) {
                exited_queued_ = true;
                Event event;
                event.name = "game_exited";
                event.timestamp = now_unix_ms();
                enqueue_locked(std::move(event));
            }
        }
        flush_cycle(true);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            persist_locked();
        }
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
    json::Value remote_values_, remote_experiments_;
    std::string remote_etag_;
    bool config_pending_ = false;
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
    bool health_queued_ = false;
    HealthSampler health_;
    double boot_ms_ = -1.0;
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
    const std::shared_ptr<int> report_owner_ = std::make_shared<int>(0);
    // Automatic error capture, guarded by mutex_.
    bool capture_wanted_ = true;
    bool capture_installed_ = false;
    bool capture_known_ = false;
    bool capture_server_enabled_ = true;
    bool capture_trail_ = false;
    bool capture_conflict_noted_ = false;
    std::vector<Event> held_captures_;
    std::vector<detail::PreparedCapture> pending_crashes_;
    bool crash_files_pending_ = false;
    // Mirrors capture_installed_ && capture_trail_ for track(), which
    // formats its trail line before taking the lock.
    std::atomic<bool> trail_live_{false};
    detail::CaptureLimiter limiter_;
    detail::Trail trail_;
    std::string crash_path_;
    std::string signal_path_;
    const std::chrono::steady_clock::time_point run_start_ = std::chrono::steady_clock::now();
    bool shutdown_requested_ = false;

    // Delivery contract state, guarded by mutex_.
    std::string queue_path_;
    std::string attribution_path_;
    std::string device_;
    std::string current_origin_;
    std::string current_session_id_;
    std::string signing_secret_;
    long long clock_offset_seconds_ = 0;
    unsigned long long persisted_revision_ = 0;
    unsigned long long persist_kicks_ = 0;
    unsigned long long loaded_through_sequence_ = 0;
    bool persist_frozen_ = false;
    bool persist_warned_ = false;
    bool delivery_loaded_ = false;
    bool gzip_refused_ = false;
    bool attribution_sent_ = false;
    std::vector<std::pair<std::string, std::string>> exposed_;

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

void Client::sample_frame(double dt_seconds) { impl_->sample_frame(dt_seconds); }

void Client::set_error_capture(bool enabled) { impl_->set_error_capture(enabled); }
bool Client::is_error_capture_active() const { return impl_->is_error_capture_active(); }

void Client::flush() { impl_->flush(); }

bool Client::flush_and_wait(double timeout_seconds) {
    return impl_->flush_and_wait(timeout_seconds);
}

ReportHandle Client::prepare_report(const std::string& id, const std::string& message, const std::string& category, const std::string& image) { return impl_->prepare_report(id, message, category, image); }
void Client::submit_report(ReportHandle report, ReportCallback callback) { impl_->report_command(std::move(report), "submit", std::move(callback)); }
void Client::report_status(ReportHandle report, ReportCallback callback) { impl_->report_command(std::move(report), "status", std::move(callback)); }
void Client::cancel_report(ReportHandle report, ReportCallback callback) { impl_->report_command(std::move(report), "cancel", std::move(callback)); }

void Client::submit_feedback(std::string message, std::string category, int rating,
                             FeedbackCallback callback) {
    impl_->submit_feedback(std::move(message), std::move(category), rating, std::move(callback));
}

void Client::fetch_suggestions(SuggestionsCallback callback) {
    impl_->fetch_suggestions(std::move(callback));
}

void Client::fetch_config() { impl_->fetch_config(); }
bool Client::get_config_bool(const std::string& key, bool fallback) const { return impl_->config_value(key).as_bool(fallback); }
double Client::get_config_number(const std::string& key, double fallback) const { return impl_->config_value(key).as_number(fallback); }
std::string Client::get_config_string(const std::string& key, const std::string& fallback) const {
    auto value = impl_->config_value(key); return value.is_string() ? value.as_string() : fallback;
}
json::Value Client::get_config_json(const std::string& key, const json::Value& fallback) const {
    auto value = impl_->config_value(key); return value.is_object() || value.is_array() ? value : fallback;
}

bool Client::track_ad(const AdEvent& ad) { return impl_->track_ad(ad); }
bool Client::track_ad_revenue(const std::string& network, const std::string& format,
                              const std::string& placement, const std::string& revenue_micros,
                              const std::string& currency) {
    AdEvent ad;
    ad.network = network;
    ad.format = format;
    ad.result = "shown";
    ad.placement = placement;
    ad.revenue_micros = revenue_micros;
    ad.currency = currency;
    return impl_->track_ad(ad);
}
bool Client::set_attribution(const Attribution& attribution) { return impl_->set_attribution(attribution); }

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
