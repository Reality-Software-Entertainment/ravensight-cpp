// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "identify.hpp"

#include "delivery.hpp"
#include "ravensight/json.hpp"

namespace ravensight {
namespace detail {

const char* identify_refusal(const std::string& account_id) {
    if (account_id.empty()) return "empty";
    if (account_id.size() > identify_max_bytes) return "too_long";
    if (account_id.find('@') != std::string::npos) return "looks_like_email";
    return nullptr;
}

std::string identify_account_hash(const std::string& salt, const std::string& account_id) {
    return hmac_sha256_hex(salt, account_id);
}

bool is_identify_hex(const std::string& text) {
    if (text.size() != 64) return false;
    for (char ch : text) {
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    }
    return true;
}

std::string write_identity(const std::string& salt, const std::string& account_hash) {
    json::ObjectWriter writer;
    writer.add("salt", salt).add("accountHash", account_hash);
    return writer.str();
}

bool read_identity(const std::string& text, std::string& salt, std::string& account_hash) {
    const json::Value parsed = json::Value::parse(text);
    salt = parsed.get_string("salt", "");
    account_hash = parsed.get_string("accountHash", "");
    if (is_identify_hex(salt) && is_identify_hex(account_hash)) return true;
    salt.clear();
    account_hash.clear();
    return false;
}

} // namespace detail
} // namespace ravensight
