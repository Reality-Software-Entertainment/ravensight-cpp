// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Internal helpers for Client::identify (docs/identify/CONTRACT.md in the
// platform repository): the on-device checks, the salted hash, and the
// stored identity file. Not part of the public API; the tests include it.

#pragma once

#include <string>

namespace ravensight {
namespace detail {

/// The longest account id, in UTF-8 bytes.
constexpr std::size_t identify_max_bytes = 256;

/// Why an account id is refused ("empty", "too_long" or
/// "looks_like_email"), or nullptr when it is accepted. Never trims.
const char* identify_refusal(const std::string& account_id);

/// Lower case hex HMAC-SHA256 keyed with the salt string's own bytes (the
/// 64 hex characters, not decoded) over the account id's bytes.
std::string identify_account_hash(const std::string& salt, const std::string& account_id);

/// True for 64 lower case hex characters: a salt or an account hash.
bool is_identify_hex(const std::string& text);

/// The identity file text, {"salt":"...","accountHash":"..."}.
std::string write_identity(const std::string& salt, const std::string& account_hash);

/// Reads the identity file text. False (and both outputs empty) when it is
/// malformed.
bool read_identity(const std::string& text, std::string& salt, std::string& account_hash);

} // namespace detail
} // namespace ravensight
