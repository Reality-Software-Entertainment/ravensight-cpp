#include "remote_config.hpp"
#include <cmath>
#include <fstream>
#include <set>
#include "storage.hpp"
namespace ravensight { namespace detail {
namespace {
bool name(const std::string& s, size_t max, bool hyphen = false) {
    if (s.empty() || s.size() > max || s[0] < 'a' || s[0] > 'z') return false;
    for (auto c : s) if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '_' && !(hyphen && c == '-')) return false;
    return s != "__proto__" && s != "constructor" && s != "prototype";
}
bool safe(const json::Value& v, int depth = 0) {
    if (depth > 20 || (v.is_number() && !std::isfinite(v.as_number()))) return false;
    std::set<std::string> names;
    for (const auto& m : v.members()) {
        if (m.first == "__proto__" || m.first == "constructor" || m.first == "prototype" || !names.insert(m.first).second || !safe(m.second, depth + 1)) return false;
    }
    for (const auto& x : v.items()) if (!safe(x, depth + 1)) return false;
    return true;
}
}
bool parse_config(const std::string& body, ConfigSnapshot& out) {
    if (body.size() > 450000) return false;
    auto root = json::Value::parse_strict(body);
    auto* values = root.find("values"); auto* experiments = root.find("experiments");
    auto etag = root.get_string("etag", "");
    if (!values || !values->is_object() || values->size() > 50 || !experiments || !experiments->is_array() || experiments->size() > 10 || etag.size() != 66 || etag.front() != '"' || etag.back() != '"') return false;
    for (size_t i = 1; i < 65; ++i) if (!(etag[i] >= '0' && etag[i] <= '9') && !(etag[i] >= 'a' && etag[i] <= 'f')) return false;
    std::set<std::string> keys;
    for (const auto& m : values->members()) if (!name(m.first, 64) || !keys.insert(m.first).second || m.second.dump().size() > 8192 || !safe(m.second)) return false;
    keys.clear();
    for (const auto& x : experiments->items()) {
        auto key = x.get_string("key", "");
        if (!name(key, 64) || !name(x.get_string("variant", ""), 40, true) || !keys.insert(key).second) return false;
    }
    out = {*values, *experiments, etag}; return true;
}
bool read_config_cache(const std::string& path, ConfigSnapshot& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 450000) return false;
    std::string body(static_cast<size_t>(file.tellg()), '\0'); file.seekg(0);
    if (!body.empty() && !file.read(&body[0], body.size())) return false;
    return parse_config(body, out);
}
std::string url_component(const std::string& value) {
    const char* hex = "0123456789ABCDEF"; std::string out;
    for (unsigned char c : value) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') out += c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
} }
