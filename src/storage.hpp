// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Internal helpers: the per user storage directory, small text files,
// SHA-256 (only to name that directory after the ingest key) and the
// platform random source behind device ids. Not part of the public API; the
// unit tests include it directly.

#pragma once

#include <cstddef>
#include <string>

namespace ravensight {
namespace detail {

/// Lower case hex SHA-256 of data.
std::string sha256_hex(const std::string& data);

/// Fills out with n bytes from the operating system's random source:
/// BCryptGenRandom on Windows, getentropy where it exists, else
/// /dev/urandom. Returns false when none of them worked.
bool os_random_bytes(unsigned char* out, std::size_t n);

/// os_random_bytes, falling back to std::random_device when it fails.
void random_bytes(unsigned char* out, std::size_t n);

/// bytes random bytes as lower case hex (twice as many characters).
std::string random_hex(std::size_t bytes);

/// A draw in [0, 1) from the random source above, for backoff jitter.
double random_unit();

/// The default storage directory for this ingest key, or empty when the
/// platform gives no per user location (no APPDATA or HOME). The directory
/// is not created here.
///   Windows: %APPDATA%\ravensight\<hash>
///   Apple:   ~/Library/Application Support/ravensight/<hash>
///   Other:   $XDG_DATA_HOME (else ~/.local/share)/ravensight/<hash>
/// <hash> is the first 12 hex characters of SHA-256 of the ingest key.
std::string default_storage_dir(const std::string& ingest_key);

/// Joins two path parts with the platform separator.
std::string join_path(const std::string& dir, const std::string& name);

/// The directory part of path, empty for a bare file name.
std::string parent_dir(const std::string& path);

/// Creates dir and its parents. True when it exists afterwards (an empty
/// dir means the current directory and is always true).
bool ensure_directory(const std::string& dir);

/// Reads a whole file. False when it cannot be opened.
bool read_text_file(const std::string& path, std::string& out);

/// Writes a whole file through a temporary file and a rename, creating the
/// parent directory first. False on any failure.
bool write_text_file(const std::string& path, const std::string& text);

/// Deletes a file. True when it is gone afterwards (or never existed).
bool remove_file(const std::string& path);

/// An environment variable, empty when unset.
std::string env_value(const char* name);

/// Removes surrounding whitespace.
std::string trim(const std::string& text);

} // namespace detail
} // namespace ravensight
