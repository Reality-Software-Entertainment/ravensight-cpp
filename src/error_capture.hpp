// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Internal pieces of automatic error capture, kept pure where they can be so
// the unit tests drive them directly: the contract's scrub, the message
// template behind the local dedupe key, the per run caps, the trail ring and
// the crash record format. Plus the two process wide hooks (the terminate
// handler and the opt-in signal handlers), which only ever write a small file
// for the next launch to send. Not part of the public API.

#pragma once

#include <cstddef>
#include <deque>
#include <map>
#include <string>

namespace ravensight {
namespace detail {

// --- Contract limits (docs/errors/CONTRACT.md) ------------------------------

inline constexpr std::size_t error_message_chars = 200;
inline constexpr std::size_t error_stack_chars = 2000;
inline constexpr std::size_t error_type_chars = 100;
inline constexpr std::size_t trail_lines = 20;
inline constexpr std::size_t trail_line_chars = 120;
inline constexpr std::size_t trail_chars = 1500;
inline constexpr int captures_per_run = 30;
inline constexpr int captures_per_key = 3;
inline constexpr int trails_per_run = 10;
inline constexpr int captures_per_minute = 6;
inline constexpr std::size_t max_held_captures = 5;

/// Cuts to at most max_chars characters (UTF-8 code points, so a cut never
/// leaves half a character).
std::string cut_chars(const std::string& text, std::size_t max_chars);

/// The contract's scrub, in the server's order: a URL loses its query,
/// fragment and user:password@; Bearer tokens, JWT-shaped strings and gt_
/// keys become [token]; emails [email]; home directories ~; IPv4 and IPv6
/// (with a digit) [ip]; a run of 32 or more characters from [A-Za-z0-9+/=_-]
/// containing a digit [token], unless every '/' piece of the run is a path
/// word (so file paths in a stack survive). Idempotent.
std::string scrub(const std::string& text);

/// The message with its variable parts replaced (UUIDs, URLs, quoted text,
/// hex runs of 8 or more, numbers), for the local dedupe key.
std::string message_template(const std::string& message);

/// The per run device caps: 30 captures, 3 per dedupe key, a token bucket of
/// 6 a minute, and 10 trails. Clock injected (seconds, any epoch).
class CaptureLimiter {
public:
    /// True when one more capture with this key may be sent; counts it.
    bool allow(const std::string& key, double now_seconds);
    /// True when one more trail may be attached; counts it.
    bool take_trail();
    void reset();
    int captured() const { return captured_; }

private:
    int captured_ = 0;
    int trails_ = 0;
    std::map<std::string, int> by_key_;
    double tokens_ = captures_per_minute;
    double tokens_at_ = -1.0;
};

/// The in-memory trail ring: the last 20 lines, `+<ms> <level> <text>`, each
/// scrubbed and cut to 120 characters. Never written to disk.
class Trail {
public:
    void add(long long ms_since_start, const std::string& level, const std::string& text);
    /// Oldest first, joined by \n, dropping the oldest lines to fit 1,500.
    std::string text() const;
    bool empty() const { return lines_.empty(); }
    std::size_t size() const { return lines_.size(); }
    void clear() { lines_.clear(); }

private:
    std::deque<std::string> lines_;
};

// --- Crash records ------------------------------------------------------------

/// What the previous run left behind: one fatal error, already cut but not
/// yet scrubbed (scrubbing happens before queueing, on the next launch).
struct CrashRecord {
    std::string type;
    std::string message;
    long long timestamp = 0;
};

/// The terminate record, a small JSON object.
std::string serialize_crash(const CrashRecord& record);
bool parse_crash(const std::string& text, CrashRecord& out);

/// The signal record, "signal <number> <unix seconds>", as written by the
/// async-signal-safe handler.
bool parse_signal_record(const std::string& text, CrashRecord& out);

/// The name of a fatal signal ("SIGSEGV"), or "SIG<number>".
std::string signal_name(int signal_number);

/// What the active exception, if any, says about itself: its dynamic type
/// (demangled where the compiler allows) and what(). Call only from a
/// terminate handler or a catch block.
CrashRecord describe_current_exception();

// --- Process wide hooks ---------------------------------------------------------

/// Installs the std::terminate handler, once per process, chaining the
/// handler that was there before. It writes one crash record to path and
/// then calls the previous handler (std::abort when there was none). A
/// second install only updates the path. Returns true when installed.
bool install_terminate_handler(const std::string& path);
/// Disarms it, and restores the previous handler when ours is still the
/// current one.
void uninstall_terminate_handler();
bool terminate_handler_installed();

/// POSIX only: handlers for SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT that
/// write "signal <n> <time>" to path with open and write only, then chain to
/// the previous action. Skipped when the terminate handler already wrote a
/// record for this crash. Returns false where unsupported (Windows).
bool install_signal_handlers(const std::string& path);
void uninstall_signal_handlers();

} // namespace detail
} // namespace ravensight
