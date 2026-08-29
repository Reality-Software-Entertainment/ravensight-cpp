// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "ravensight/client.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <fstream>
#include <future>
#include <mutex>
#include <random>
#include <stdexcept>
#include <thread>

#include "ravensight/curl_transport.hpp"

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

std::string generate_device_id() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    char buf[48];
    std::snprintf(buf, sizeof(buf), "dev_%016llx%016llx",
                  static_cast<unsigned long long>(gen()),
                  static_cast<unsigned long long>(gen()));
    return buf;
}

std::string trim_copy(const std::string& text) {
    const std::size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return "";
    }
    const std::size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

std::string load_or_create_device_id(const std::string& path) {
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::string saved((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            saved = trim_copy(saved);
            if (!saved.empty()) {
                return saved;
            }
        }
    }
    const std::string fresh = generate_device_id();
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (out) {
        out << fresh;
    }
    return fresh;
}

double now_unix_seconds() {
    return std::chrono::duration<double>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

std::string Client::normalize_api_url(const std::string& url) {
    std::string raw = trim_copy(url);
    while (!raw.empty() && raw.back() == '/') {
        raw.pop_back();
    }
    if (raw.empty()) {
        raw = "https://api.ravensight.io";
    }
    const std::string suffix = "/api/v1";
    if (raw.size() >= suffix.size() &&
        raw.compare(raw.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return raw;
    }
    return raw + suffix;
}

// --- Impl -------------------------------------------------------------------

class Client::Impl {
public:
    Impl(Config config, std::unique_ptr<Transport> transport,
         std::string device_id, std::string api_url)
        : config_(std::move(config)),
          transport_(std::move(transport)),
          device_id_(std::move(device_id)),
          api_url_(std::move(api_url)),
          platform_(config_.platform.empty() ? detect_platform() : config_.platform) {
        queue_.set_max_queue_size(config_.max_queue_size);
        queue_.set_locally_enabled(config_.enabled);
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
        const int dropped = queue_.enqueue(std::move(event));
        if (dropped > 0) {
            log("queue full, dropped " + std::to_string(dropped) + " oldest event(s)");
        }
        return true;
    }

    void flush() {
        std::lock_guard<std::mutex> lock(mutex_);
        nudge_ = true;
        cv_.notify_all();
    }

    bool flush_and_wait(double timeout_seconds) {
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
        message = trim_copy(message);
        if (message.empty()) {
            if (callback) callback(FeedbackResult{false, "empty_message", 0});
            return;
        }

        std::string local_failure;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (shutdown_requested_) {
                local_failure = "shutting_down";
            } else if (!queue_.is_active()) {
                local_failure = "tracking_disabled";
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
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutdown_requested_) {
            return;
        }
        commands_.push_back([this, callback] { run_suggestions(callback); });
        cv_.notify_all();
    }

    void set_enabled(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.set_locally_enabled(enabled);
    }

    bool is_enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.is_locally_enabled();
    }

    bool tracking_enabled() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.is_server_tracking_enabled();
    }

    bool is_ready() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.is_session_valid(now_unix_seconds());
    }

    Stats stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        const QueueStats qs = queue_.stats();
        Stats out;
        out.queued = qs.queued;
        out.sent = qs.sent;
        out.dropped = qs.dropped;
        out.batches = qs.batches;
        out.sessions = qs.sessions;
        return out;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_requested_ = true;
            cv_.notify_all();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

private:
    void log(const std::string& message) {
        if (config_.log) {
            config_.log("Ravensight: " + message);
        }
    }

    Request make_request(const char* method, const char* path) const {
        Request request;
        request.method = method;
        request.url = api_url_ + path;
        return request;
    }

    // Reads the server kill switch once at boot. An unreachable or malformed
    // settings endpoint means tracking is assumed on, matching the Godot SDK,
    // so a settings outage does not silently lose a session.
    void check_settings() {
        Request request = make_request("GET", "/settings");
        request.with_header("X-API-Key", config_.ingest_key);
        const Response response = transport_->send(request);

        bool enabled = true;
        bool parsed = false;
        if (response.status == 200) {
            parsed = response.parse_json().get_bool("trackingEnabled", enabled);
        }
        if (response.status != 200) {
            log("settings check failed (HTTP " + std::to_string(response.status) +
                "), assuming tracking enabled");
        } else if (!parsed) {
            enabled = true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        queue_.set_server_tracking_enabled(enabled);
    }

    // Returns true when a valid session is held on exit. force ignores the
    // retry window (shutdown and explicit flushes), never the kill switch.
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

        json::ObjectWriter body;
        body.add("deviceId", device_id_);
        body.add("gameVersion", config_.game_version);
        body.add("platform", platform());

        Request request = make_request("POST", "/session");
        request.body = body.str();
        request.with_header("Content-Type", "application/json");
        request.with_header("X-API-Key", config_.ingest_key);

        const Response response = transport_->send(request);

        if (response.status == 201) {
            const json::Value parsed = response.parse_json();
            const std::string token = parsed.get_string("token", "");
            if (token.empty()) {
                log("malformed session response");
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.schedule_backoff(now_unix_seconds());
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
            {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.set_session(token, expires_at);
            }
            log("session ready");
            return true;
        }

        if (response.status == 429) {
            log("session creation rate limited (" + response.error_code("rate_limited") + ")");
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.on_rate_limited(now_unix_seconds(), response.retry_after_seconds());
            return false;
        }

        log("session creation failed (HTTP " + std::to_string(response.status) + ")");
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.on_transport_failure(now_unix_seconds());
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
    // to back off. Events leave the queue only once the server accepts them.
    void flush_cycle(bool force) {
        int reauth_attempts = 0;
        int session_attempts = 0;

        for (;;) {
            FlushPlan plan;
            std::string token;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                plan = queue_.begin_flush(now_unix_seconds(), force);
                token = queue_.session_token();
            }

            if (plan.decision == FlushDecision::needs_session) {
                if (++session_attempts > 2) {
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

            Request request = make_request("POST", "/track/batch");
            request.body = build_batch_body(plan.batch);
            request.with_header("Content-Type", "application/json");
            request.with_header("X-Session-Token", token);

            const Response response = transport_->send(request);
            const std::size_t batch_size = plan.batch.size();

            if (response.status == 202) {
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.on_batch_accepted(batch_size);
                queue_.end_flush();
                continue;
            }

            if (response.status == 401) {
                // Session expired or revoked. The batch stays queued and is
                // re-sent as soon as a fresh session is issued.
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.on_unauthorized();
                    queue_.end_flush();
                }
                if (++reauth_attempts > 2) {
                    return;
                }
                log("session expired, re-authenticating");
                continue;
            }

            if (response.status == 429) {
                log("track/batch rate limited (" + response.error_code("rate_limited") + ")");
                std::lock_guard<std::mutex> lock(mutex_);
                queue_.on_rate_limited(now_unix_seconds(), response.retry_after_seconds());
                queue_.end_flush();
                return;
            }

            if (response.status == 400) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.on_bad_request(batch_size);
                    queue_.end_flush();
                }
                if (batch_size > 1) {
                    log("batch rejected as oversize, splitting");
                } else {
                    log("dropped one event rejected with HTTP 400");
                }
                continue;
            }

            log("batch flush failed (HTTP " + std::to_string(response.status) + "), will retry");
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.on_transport_failure(now_unix_seconds());
            queue_.end_flush();
            return;
        }
    }

    void run_feedback(const std::string& message, const std::string& category, int rating,
                      const FeedbackCallback& callback) {
        const auto fail = [&](const std::string& code, long status) {
            if (callback) callback(FeedbackResult{false, code, status});
        };
        bool active;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            active = queue_.is_active();
        }
        if (!active) {
            fail("tracking_disabled", 0);
            return;
        }
        if (!ensure_session(true)) {
            fail("no_session", 0);
            return;
        }

        json::ObjectWriter writer;
        writer.add("message", message);
        if (!category.empty()) {
            writer.add("category", category);
        }
        if (rating > 0) {
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
                fail("no_session", 0);
                return;
            }
            response = send_feedback(body);
        }

        if (response.status == 201) {
            if (callback) callback(FeedbackResult{true, "", 201});
            return;
        }
        log("feedback submission failed (HTTP " + std::to_string(response.status) + ")");
        fail(response.error_code("http_" + std::to_string(response.status)), response.status);
    }

    Response send_feedback(const std::string& body) {
        std::string token;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            token = queue_.session_token();
        }
        Request request = make_request("POST", "/feedback");
        request.body = body;
        request.with_header("Content-Type", "application/json");
        request.with_header("X-Session-Token", token);
        return transport_->send(request);
    }

    void run_suggestions(const SuggestionsCallback& callback) {
        Request request = make_request("GET", "/agent/suggestions");
        request.with_header("X-API-Key", config_.ingest_key);
        const Response response = transport_->send(request);

        static const json::Value empty_array = json::Value::parse("[]");
        if (response.status != 200) {
            log("fetch suggestions failed (HTTP " + std::to_string(response.status) + ")");
            callback(false, empty_array);
            return;
        }
        const json::Value parsed = response.parse_json();
        const json::Value* suggestions = parsed.find("suggestions");
        if (suggestions != nullptr && suggestions->is_array()) {
            callback(true, *suggestions);
        } else {
            callback(true, empty_array);
        }
    }

    const std::string& platform() const {
        return platform_;
    }

    void worker_main() {
        check_settings();

        bool active;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_checked_ = true;
            active = queue_.is_active();
        }
        if (!active) {
            log("tracking disabled by server kill switch");
        } else if (config_.track_lifecycle_events) {
            track("game_started", {});
        }

        for (;;) {
            std::function<void()> command;
            bool do_shutdown = false;
            bool force = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (commands_.empty() && !shutdown_requested_ && !nudge_) {
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
                        return shutdown_requested_ || nudge_ || !commands_.empty();
                    });
                }
                if (!commands_.empty()) {
                    command = std::move(commands_.front());
                    commands_.pop_front();
                } else {
                    force = nudge_;
                    nudge_ = false;
                    do_shutdown = shutdown_requested_;
                }
            }
            if (command) {
                command();
                continue;
            }
            if (do_shutdown) {
                break;
            }
            if (config_.flush_interval_seconds > 0.0 || force) {
                flush_cycle(force);
            }
        }

        // Final best effort flush on the way out.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.is_active() && config_.track_lifecycle_events) {
                Event event;
                event.name = "game_exited";
                event.timestamp = static_cast<long long>(now_unix_seconds());
                queue_.enqueue(std::move(event));
            }
        }
        flush_cycle(true);
    }

    friend class Client;

    Config config_;
    std::unique_ptr<Transport> transport_;
    std::string device_id_;
    std::string api_url_;
    std::string platform_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    Queue queue_;
    std::deque<std::function<void()>> commands_;
    bool nudge_ = false;
    bool shutdown_requested_ = false;
    bool settings_checked_ = false;

    std::thread worker_;
};

// --- Client facade ----------------------------------------------------------

namespace {

std::string resolve_device_id(const Config& config) {
    if (!config.device_id.empty()) {
        return config.device_id;
    }
    if (!config.device_id_file.empty()) {
        return load_or_create_device_id(config.device_id_file);
    }
    return generate_device_id();
}

} // namespace

Client::Client(Config config)
    : Client(std::move(config), nullptr) {}

Client::Client(Config config, std::unique_ptr<Transport> transport) {
    if (trim_copy(config.ingest_key).empty()) {
        throw std::invalid_argument(
            "Ravensight: ingest_key is required (your gt_live_... key)");
    }
    if (!transport) {
        transport = std::make_unique<CurlTransport>(config.request_timeout_seconds);
    }
    device_id_ = resolve_device_id(config);
    api_url_ = normalize_api_url(config.api_url);
    impl_ = std::make_unique<Impl>(std::move(config), std::move(transport), device_id_, api_url_);
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
bool Client::tracking_enabled() const { return impl_->tracking_enabled(); }
bool Client::is_ready() const { return impl_->is_ready(); }

Stats Client::stats() const { return impl_->stats(); }

void Client::shutdown() { impl_->shutdown(); }

} // namespace ravensight
