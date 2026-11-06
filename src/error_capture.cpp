// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "error_capture.hpp"

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <mutex>
#include <regex>
#include <typeinfo>

#include "ravensight/json.hpp"
#include "storage.hpp"

#if defined(__GNUG__) || defined(__clang__)
#include <cxxabi.h>
#endif

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#define RAVENSIGHT_POSIX_SIGNALS 1
#endif

namespace ravensight {
namespace detail {

std::string cut_chars(const std::string& text, std::size_t max_chars) {
    std::size_t chars = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if ((static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
            if (chars == max_chars) {
                return text.substr(0, i);
            }
            ++chars;
        }
    }
    return text;
}

// --- Scrub --------------------------------------------------------------------

namespace {

using std::regex;
using std::regex_constants::ECMAScript;
using std::regex_constants::icase;

const regex& url_query() {
    static const regex re(
        R"(\b([a-z][a-z0-9+.-]*://)(?:[^\s/?#@'"<>]*@)?([^\s?#'"<>]*)(?:[?#][^\s'"<>]*)?)",
        ECMAScript | icase);
    return re;
}

/// Replaces every match of re with what fn returns for it.
template <typename Fn>
std::string replace_each(const std::string& text, const regex& re, Fn fn) {
    std::string out;
    std::size_t last = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator();
         ++it) {
        const std::smatch& match = *it;
        const auto position = static_cast<std::size_t>(match.position(0));
        out.append(text, last, position - last);
        out += fn(match);
        last = position + static_cast<std::size_t>(match.length(0));
    }
    out.append(text, last, std::string::npos);
    return out;
}

bool has_digit(const std::string& text) {
    return std::any_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool in_token_class(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' ||
           c == '/' || c == '=' || c == '_' || c == '-';
}

/// One '/' piece of a path: empty, snake or kebab lower case, PascalCase,
/// or a short upper case word, under 32 characters.
bool is_path_word(const std::string& piece) {
    static const regex lower(R"(^[a-z0-9]+([_-][a-z0-9]+)*$)");
    static const regex pascal(R"(^[A-Z][a-z0-9]*(?:[A-Z][a-z0-9]+)*$)");
    static const regex upper(R"(^[A-Z0-9]{1,6}$)");
    if (piece.size() >= 32) {
        return false;
    }
    return piece.empty() || std::regex_match(piece, lower) || std::regex_match(piece, pascal) ||
           std::regex_match(piece, upper);
}

/// A run with a '/' whose every piece is a path word: a file path, kept.
bool is_path_run(const std::string& run) {
    if (run.find('/') == std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = run.find('/', start);
        const std::string piece =
            run.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (!is_path_word(piece)) {
            return false;
        }
        if (slash == std::string::npos) {
            return true;
        }
        start = slash + 1;
    }
}

/// Runs of 32 or more [A-Za-z0-9+/=_-] containing a digit become [token],
/// unless the run is a file path (see is_path_run). So a secret with a '/'
/// in it is still caught, and res://addons/inventory_system2/... survives.
std::string scrub_long_runs(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        if (!in_token_class(text[i])) {
            out.push_back(text[i++]);
            continue;
        }
        std::size_t end = i;
        while (end < text.size() && in_token_class(text[end])) {
            ++end;
        }
        const std::string run = text.substr(i, end - i);
        if (run.size() >= 32 && has_digit(run) && !is_path_run(run)) {
            out += "[token]";
        } else {
            out += run;
        }
        i = end;
    }
    return out;
}

} // namespace

std::string scrub(const std::string& input) {
    if (input.empty()) {
        return input;
    }
    static const regex bearer(R"(\bBearer\s+[A-Za-z0-9\-._~+/]+=*)", ECMAScript | icase);
    static const regex jwt(R"(\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]*)");
    static const regex gt_key(R"(\bgt_[A-Za-z0-9_]+)");
    static const regex email(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})");
    static const regex home_windows(R"(\b[A-Za-z]:[\\/]Users[\\/][^\\/\s'"]+)", ECMAScript | icase);
    static const regex home_unix(R"(/(?:Users|home)/[^/\s'"]+)");
    static const regex ipv4(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)");
    static const regex ipv6_full(R"(\b(?:[0-9a-f]{1,4}:){7}[0-9a-f]{1,4}\b)", ECMAScript | icase);
    static const regex ipv6_short(
        R"((?:\b[0-9a-f]{1,4}(?::[0-9a-f]{1,4}){0,6})?::(?:[0-9a-f]{1,4}(?::[0-9a-f]{1,4}){0,6}\b)?)",
        ECMAScript | icase);

    std::string out = std::regex_replace(input, url_query(), "$1$2");
    out = std::regex_replace(out, bearer, "[token]");
    out = std::regex_replace(out, jwt, "[token]");
    out = std::regex_replace(out, gt_key, "[token]");
    out = std::regex_replace(out, email, "[email]");
    out = std::regex_replace(out, home_windows, "~");
    out = std::regex_replace(out, home_unix, "~");
    out = std::regex_replace(out, ipv4, "[ip]");
    out = std::regex_replace(out, ipv6_full, "[ip]");
    out = replace_each(out, ipv6_short, [](const std::smatch& m) {
        const std::string text = m.str(0);
        return has_digit(text) ? std::string("[ip]") : text;
    });
    return scrub_long_runs(out);
}

std::string message_template(const std::string& message) {
    static const regex uuid(
        R"([0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})");
    static const regex url(R"(\b[a-zA-Z][a-zA-Z0-9+.-]*://\S+)");
    static const regex quoted(R"("[^"]*"|'[^']*')");
    static const regex hex(R"(\b[0-9a-fA-F]{8,}\b)");
    static const regex number(R"(\d+(\.\d+)?)");
    std::string out = std::regex_replace(message, uuid, "<uuid>");
    out = std::regex_replace(out, url, "<url>");
    out = std::regex_replace(out, quoted, "<str>");
    out = std::regex_replace(out, hex, "<hex>");
    return std::regex_replace(out, number, "<n>");
}

// --- Caps ---------------------------------------------------------------------

bool CaptureLimiter::allow(const std::string& key, double now_seconds) {
    if (captured_ >= captures_per_run) {
        return false;
    }
    auto found = by_key_.find(key);
    if (found != by_key_.end() && found->second >= captures_per_key) {
        return false;
    }
    if (tokens_at_ >= 0.0 && now_seconds > tokens_at_) {
        tokens_ = std::min(static_cast<double>(captures_per_minute),
                           tokens_ + (now_seconds - tokens_at_) * captures_per_minute / 60.0);
    }
    tokens_at_ = now_seconds;
    if (tokens_ < 1.0) {
        return false;
    }
    tokens_ -= 1.0;
    ++captured_;
    ++by_key_[key];
    return true;
}

bool CaptureLimiter::take_trail() {
    if (trails_ >= trails_per_run) {
        return false;
    }
    ++trails_;
    return true;
}

void CaptureLimiter::reset() {
    *this = CaptureLimiter();
}

// --- Trail ----------------------------------------------------------------------

void Trail::add(long long ms_since_start, const std::string& level, const std::string& text) {
    std::string flat = text;
    std::replace(flat.begin(), flat.end(), '\n', ' ');
    std::replace(flat.begin(), flat.end(), '\r', ' ');
    flat = trim(cut_chars(flat, 1000));
    if (flat.empty()) {
        return;
    }
    const std::string line = "+" + std::to_string(std::max(0LL, ms_since_start)) + " " + level +
                             " " + scrub(flat);
    lines_.push_back(cut_chars(line, trail_line_chars));
    while (lines_.size() > trail_lines) {
        lines_.pop_front();
    }
}

std::string Trail::text() const {
    std::size_t first = 0;
    for (;;) {
        std::string joined;
        std::size_t chars = 0;
        for (std::size_t i = first; i < lines_.size(); ++i) {
            if (i > first) {
                joined.push_back('\n');
            }
            joined += lines_[i];
        }
        for (char c : joined) {
            chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
        }
        if (chars <= trail_chars || first + 1 >= lines_.size()) {
            return cut_chars(joined, trail_chars);
        }
        ++first;
    }
}

// --- Crash records ----------------------------------------------------------------

std::string serialize_crash(const CrashRecord& record) {
    json::ObjectWriter writer;
    writer.add("type", record.type)
        .add("message", record.message)
        .add("timestamp", record.timestamp);
    return writer.str() + "\n";
}

bool parse_crash(const std::string& text, CrashRecord& out) {
    const json::Value value = json::Value::parse(text);
    if (!value.is_object()) {
        return false;
    }
    out.type = cut_chars(value.get_string("type", ""), error_type_chars);
    out.message = cut_chars(value.get_string("message", ""), error_stack_chars);
    double timestamp = 0.0;
    out.timestamp = value.get_number("timestamp", timestamp) ? static_cast<long long>(timestamp) : 0;
    return !out.message.empty() || !out.type.empty();
}

std::string signal_name(int signal_number) {
    switch (signal_number) {
        case SIGSEGV:
            return "SIGSEGV";
        case SIGFPE:
            return "SIGFPE";
        case SIGILL:
            return "SIGILL";
        case SIGABRT:
            return "SIGABRT";
#if defined(SIGBUS)
        case SIGBUS:
            return "SIGBUS";
#endif
        default:
            return "SIG" + std::to_string(signal_number);
    }
}

bool parse_signal_record(const std::string& text, CrashRecord& out) {
    int number = 0;
    long long timestamp = 0;
    if (std::sscanf(text.c_str(), "signal %d %lld", &number, &timestamp) < 1 || number <= 0) {
        return false;
    }
    out.type = signal_name(number);
    out.message = "Fatal signal " + out.type;
    out.timestamp = timestamp;
    return true;
}

namespace {

std::string demangle(const char* name) {
#if defined(__GNUG__) || defined(__clang__)
    int status = 0;
    char* readable = abi::__cxa_demangle(name, nullptr, nullptr, &status);
    if (status == 0 && readable != nullptr) {
        std::string out = readable;
        std::free(readable);
        return out;
    }
#endif
    std::string out = name == nullptr ? "" : name;
    // MSVC writes "class std::runtime_error".
    for (const char* prefix : {"class ", "struct "}) {
        if (out.rfind(prefix, 0) == 0) {
            out.erase(0, std::strlen(prefix));
        }
    }
    return out;
}

} // namespace

CrashRecord describe_current_exception() {
    CrashRecord record;
    const std::exception_ptr active = std::current_exception();
    if (!active) {
        record.type = "std::terminate";
        record.message = "std::terminate called without an active exception";
        return record;
    }
    try {
        std::rethrow_exception(active);
    } catch (const std::exception& error) {
        record.type = demangle(typeid(error).name());
        record.message = error.what() == nullptr ? "" : error.what();
    } catch (const char* text) {
        record.type = "const char*";
        record.message = text == nullptr ? "" : text;
    } catch (const std::string& text) {
        record.type = "std::string";
        record.message = text;
    } catch (...) {
        record.type = "unknown";
    }
    if (trim(record.message).empty()) {
        record.message = "uncaught exception of type " + record.type;
    }
    record.type = cut_chars(record.type, error_type_chars);
    record.message = cut_chars(record.message, error_stack_chars);
    return record;
}

// --- Process wide hooks ------------------------------------------------------------

namespace {

constexpr std::size_t max_path = 2048;

std::mutex g_hook_mutex;
std::terminate_handler g_previous_terminate = nullptr;
bool g_terminate_installed = false;
std::atomic<bool> g_terminate_armed{false};
std::atomic_flag g_in_terminate = ATOMIC_FLAG_INIT;
char g_terminate_path[max_path] = {0};
// Set once a record for this crash is on disk, so the SIGABRT that
// std::abort raises after the terminate handler does not write a second.
volatile std::sig_atomic_t g_crash_written = 0;

bool copy_path(char* target, const std::string& path) {
    if (path.empty() || path.size() >= max_path) {
        return false;
    }
    std::memcpy(target, path.c_str(), path.size() + 1);
    return true;
}

void write_now(const char* path, const std::string& text) {
    std::FILE* file = std::fopen(path, "wb");
    if (file == nullptr) {
        return;
    }
    std::fwrite(text.data(), 1, text.size(), file);
    std::fflush(file);
    std::fclose(file);
}

[[noreturn]] void on_terminate() {
    if (g_in_terminate.test_and_set()) {
        // Re-entered: another handler chained back to us. Stop here.
        std::abort();
    }
    if (g_terminate_armed.load()) {
        try {
            CrashRecord record = describe_current_exception();
            record.timestamp = static_cast<long long>(std::time(nullptr));
            write_now(g_terminate_path, serialize_crash(record));
            g_crash_written = 1;
        } catch (...) {
            // Never let the capture path change how the game dies.
        }
    }
    const std::terminate_handler previous = g_previous_terminate;
    if (previous != nullptr && previous != &on_terminate) {
        previous();
    }
    std::abort();
}

} // namespace

bool install_terminate_handler(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (g_terminate_armed.load() && g_terminate_installed) {
        return true; // one client holds it already
    }
    if (!copy_path(g_terminate_path, path)) {
        return false;
    }
    ensure_directory(parent_dir(path));
    // Installed once: if a handler set after ours is on top, ours stays in
    // its chain and is only re-armed, so the two never call each other in a
    // loop.
    if (!g_terminate_installed) {
        const std::terminate_handler previous = std::set_terminate(&on_terminate);
        g_previous_terminate = previous == &on_terminate ? nullptr : previous;
        g_terminate_installed = true;
    }
    g_terminate_armed.store(true);
    return true;
}

void uninstall_terminate_handler() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_terminate_armed.store(false);
    if (g_terminate_installed && std::get_terminate() == &on_terminate) {
        std::set_terminate(g_previous_terminate);
        g_previous_terminate = nullptr;
        g_terminate_installed = false;
    }
}

bool terminate_handler_installed() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    return g_terminate_installed && g_terminate_armed.load() && std::get_terminate() == &on_terminate;
}

#if defined(RAVENSIGHT_POSIX_SIGNALS)

namespace {

const int g_signals[] = {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT};
constexpr int g_signal_count = 5;
struct sigaction g_old_actions[g_signal_count];
bool g_signals_installed = false;
volatile std::sig_atomic_t g_signals_armed = 0;
char g_signal_path[max_path] = {0};

/// Async-signal-safe: digits of a non-negative number into buf.
std::size_t write_digits(char* buf, long long value) {
    char tmp[24];
    std::size_t n = 0;
    if (value <= 0) {
        tmp[n++] = '0';
    }
    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    for (std::size_t i = 0; i < n; ++i) {
        buf[i] = tmp[n - 1 - i];
    }
    return n;
}

void on_signal(int number, siginfo_t* info, void* context) {
    int index = -1;
    for (int i = 0; i < g_signal_count; ++i) {
        if (g_signals[i] == number) {
            index = i;
        }
    }
    if (g_signals_armed && !g_crash_written) {
        g_crash_written = 1;
        // open, write and close only: all three are async-signal-safe.
        const int fd = open(g_signal_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            char buf[64] = "signal ";
            std::size_t n = 7;
            n += write_digits(buf + n, number);
            buf[n++] = ' ';
            n += write_digits(buf + n, static_cast<long long>(time(nullptr)));
            buf[n++] = '\n';
            ssize_t ignored = write(fd, buf, n);
            (void)ignored;
            close(fd);
        }
    }
    if (index < 0) {
        return;
    }
    struct sigaction& old = g_old_actions[index];
    if ((old.sa_flags & SA_SIGINFO) != 0 && old.sa_sigaction != nullptr) {
        old.sa_sigaction(number, info, context);
        return;
    }
    if ((old.sa_flags & SA_SIGINFO) == 0 && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN &&
        old.sa_handler != nullptr) {
        old.sa_handler(number);
        return;
    }
    // The default action: put it back and raise again, so the process dies
    // exactly as it would have without the SDK.
    struct sigaction fallback;
    std::memset(&fallback, 0, sizeof(fallback));
    fallback.sa_handler = SIG_DFL;
    sigemptyset(&fallback.sa_mask);
    sigaction(number, &fallback, nullptr);
    raise(number);
}

} // namespace

bool install_signal_handlers(const std::string& path) {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    if (!copy_path(g_signal_path, path)) {
        return false;
    }
    if (g_signals_installed) {
        g_signals_armed = 1;
        return true;
    }
    ensure_directory(parent_dir(path));
    struct sigaction action;
    std::memset(&action, 0, sizeof(action));
    action.sa_sigaction = &on_signal;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    for (int i = 0; i < g_signal_count; ++i) {
        sigaction(g_signals[i], &action, &g_old_actions[i]);
    }
    g_signals_installed = true;
    g_signals_armed = 1;
    return true;
}

void uninstall_signal_handlers() {
    std::lock_guard<std::mutex> lock(g_hook_mutex);
    g_signals_armed = 0;
    if (!g_signals_installed) {
        return;
    }
    bool restored_all = true;
    for (int i = 0; i < g_signal_count; ++i) {
        struct sigaction current;
        sigaction(g_signals[i], nullptr, &current);
        // Restore only where ours is still the handler; a handler installed
        // after ours keeps its place (and ours stays disarmed behind it, to
        // be re-armed rather than installed twice).
        if ((current.sa_flags & SA_SIGINFO) != 0 && current.sa_sigaction == &on_signal) {
            sigaction(g_signals[i], &g_old_actions[i], nullptr);
        } else {
            restored_all = false;
        }
    }
    g_signals_installed = !restored_all;
}

#else

bool install_signal_handlers(const std::string&) { return false; }
void uninstall_signal_handlers() {}

#endif

} // namespace detail
} // namespace ravensight
