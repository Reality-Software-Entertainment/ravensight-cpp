// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Automatic error capture: the log sink half. C++ has no engine to hook, so
// the SDK cannot see your game's log by itself. Route your logger here (one
// line, see log_sink) and every line feeds the trail, and every line at
// LogLevel::error or above becomes one ordinary `error` event, scrubbed on
// the device and inside the per run caps.
//
// The other half needs no call: a Client built with Config::error_capture
// (on by default) installs a chained std::terminate handler once it has a
// key and consent. See the README's "Error capture" section.

#pragma once

#include <functional>
#include <string_view>

namespace ravensight {

enum class LogLevel { debug, info, warning, error, critical };

/// "debug", "info", "warning", "error" or "critical".
const char* to_string(LogLevel level);

/// Reads a level name, case insensitive: debug, trace (as debug), info,
/// warn or warning, error or err, critical, fatal (as critical). False for
/// anything else.
bool parse_log_level(std::string_view name, LogLevel& out);

/// Hands one line of your game's log to the capturing Client. Thread safe,
/// never blocks on the network, never throws. A no-op when no Client is
/// capturing (no key, no consent, capture off for the game or the run).
/// Lines that start with "Ravensight" (the SDK's own) are ignored.
void log(LogLevel level, std::string_view message);

/// The same as ravensight::log, as a callable your logger can hold:
///   my_logger.add_sink(ravensight::log_sink());
using LogSink = std::function<void(LogLevel, std::string_view)>;
LogSink log_sink();

} // namespace ravensight
