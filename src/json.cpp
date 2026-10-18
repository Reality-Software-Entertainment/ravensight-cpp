// Copyright 2026 Reality Software Entertainment. MIT licensed.

#include "ravensight/json.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace ravensight {
namespace json {

// --- Writing ---------------------------------------------------------------

std::string escape_string(const std::string& value) {
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    // UTF-8 bytes pass through unchanged.
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    out.push_back('"');
    return out;
}

std::string write_number(double value) {
    if (std::isnan(value) || std::isinf(value)) {
        return "null";
    }
    // Write integral values inside the double safe integer range without a
    // decimal point, so timestamps and counters stay clean on the wire.
    if (value >= -9007199254740992.0 && value <= 9007199254740992.0 && value == std::floor(value)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(value));
        return buf;
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.17g", value);
    return buf;
}

void ObjectWriter::write_key(const std::string& key) {
    if (!first_) {
        out_.push_back(',');
    }
    first_ = false;
    out_ += escape_string(key);
    out_.push_back(':');
}

ObjectWriter& ObjectWriter::add(const std::string& key, const std::string& value) {
    write_key(key);
    out_ += escape_string(value);
    return *this;
}

ObjectWriter& ObjectWriter::add(const std::string& key, const char* value) {
    if (value == nullptr) {
        return add_null(key);
    }
    return add(key, std::string(value));
}

ObjectWriter& ObjectWriter::add(const std::string& key, long long value) {
    write_key(key);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld", value);
    out_ += buf;
    return *this;
}

ObjectWriter& ObjectWriter::add(const std::string& key, double value) {
    write_key(key);
    out_ += write_number(value);
    return *this;
}

ObjectWriter& ObjectWriter::add(const std::string& key, bool value) {
    write_key(key);
    out_ += value ? "true" : "false";
    return *this;
}

ObjectWriter& ObjectWriter::add_null(const std::string& key) {
    write_key(key);
    out_ += "null";
    return *this;
}

ObjectWriter& ObjectWriter::add_raw(const std::string& key, const std::string& raw_json) {
    write_key(key);
    out_ += raw_json;
    return *this;
}

std::string ObjectWriter::str() {
    out_.push_back('}');
    return std::move(out_);
}

// --- Reading ---------------------------------------------------------------

namespace {
constexpr int kMaxDepth = 24;

void append_utf8(std::string& out, unsigned long code_point) {
    if (code_point <= 0x7F) {
        out.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
}
} // namespace

class Parser {
public:
    Parser(const std::string& text) : s_(text) {}

    bool parse(Value& out, bool strict = false) {
        if (!parse_value(out, 0)) {
            return false;
        }
        skip_whitespace();
        return !strict || i_ == s_.size();
    }

private:
    const std::string& s_;
    std::size_t i_ = 0;

    void skip_whitespace() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++i_;
            } else {
                break;
            }
        }
    }

    bool expect(const char* literal) {
        std::size_t n = 0;
        while (literal[n] != '\0') {
            if (i_ + n >= s_.size() || s_[i_ + n] != literal[n]) {
                return false;
            }
            ++n;
        }
        i_ += n;
        return true;
    }

    bool parse_value(Value& out, int depth) {
        if (depth > kMaxDepth) {
            return false;
        }
        skip_whitespace();
        if (i_ >= s_.size()) {
            return false;
        }
        const char c = s_[i_];
        if (c == '{') return parse_object(out, depth);
        if (c == '[') return parse_array(out, depth);
        if (c == '"') {
            out.type_ = Value::Type::String;
            return parse_string(out.string_);
        }
        if (c == 't') {
            if (!expect("true")) return false;
            out.type_ = Value::Type::Bool;
            out.bool_ = true;
            return true;
        }
        if (c == 'f') {
            if (!expect("false")) return false;
            out.type_ = Value::Type::Bool;
            out.bool_ = false;
            return true;
        }
        if (c == 'n') {
            if (!expect("null")) return false;
            out.type_ = Value::Type::Null;
            return true;
        }
        return parse_number(out);
    }

    bool parse_object(Value& out, int depth) {
        out.type_ = Value::Type::Object;
        ++i_; // consume '{'
        skip_whitespace();
        if (i_ < s_.size() && s_[i_] == '}') {
            ++i_;
            return true;
        }
        while (true) {
            skip_whitespace();
            if (i_ >= s_.size() || s_[i_] != '"') {
                return false;
            }
            std::string key;
            if (!parse_string(key)) {
                return false;
            }
            skip_whitespace();
            if (i_ >= s_.size() || s_[i_] != ':') {
                return false;
            }
            ++i_;
            Value child;
            if (!parse_value(child, depth + 1)) {
                return false;
            }
            out.members_.emplace_back(std::move(key), std::move(child));
            skip_whitespace();
            if (i_ >= s_.size()) {
                return false;
            }
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == '}') {
                ++i_;
                return true;
            }
            return false;
        }
    }

    bool parse_array(Value& out, int depth) {
        out.type_ = Value::Type::Array;
        ++i_; // consume '['
        skip_whitespace();
        if (i_ < s_.size() && s_[i_] == ']') {
            ++i_;
            return true;
        }
        while (true) {
            Value child;
            if (!parse_value(child, depth + 1)) {
                return false;
            }
            out.items_.push_back(std::move(child));
            skip_whitespace();
            if (i_ >= s_.size()) {
                return false;
            }
            if (s_[i_] == ',') {
                ++i_;
                continue;
            }
            if (s_[i_] == ']') {
                ++i_;
                return true;
            }
            return false;
        }
    }

    bool parse_hex4(unsigned long& out) {
        if (i_ + 4 > s_.size()) {
            return false;
        }
        out = 0;
        for (int n = 0; n < 4; ++n) {
            const char c = s_[i_ + static_cast<std::size_t>(n)];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned long>(c - 'A' + 10);
            else return false;
        }
        i_ += 4;
        return true;
    }

    bool parse_string(std::string& out) {
        ++i_; // consume opening quote
        while (true) {
            if (i_ >= s_.size()) {
                return false;
            }
            const char c = s_[i_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (i_ >= s_.size()) {
                return false;
            }
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned long code = 0;
                    if (!parse_hex4(code)) {
                        return false;
                    }
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        // High surrogate: a low surrogate must follow.
                        if (i_ + 1 < s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            i_ += 2;
                            unsigned long low = 0;
                            if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
                                return false;
                            }
                            code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        } else {
                            return false;
                        }
                    } else if (code >= 0xDC00 && code <= 0xDFFF) {
                        return false; // lone low surrogate
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    return false;
            }
        }
    }

    bool parse_number(Value& out) {
        const std::size_t start = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
            ++i_;
        }
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-') {
                ++i_;
            } else {
                break;
            }
        }
        if (i_ == start) {
            return false;
        }
        const std::string text = s_.substr(start, i_ - start);
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end == nullptr || *end != '\0') {
            return false;
        }
        out.type_ = Value::Type::Number;
        out.number_ = value;
        return true;
    }
};

Value Value::parse(const std::string& text) {
    if (text.empty()) {
        return Value();
    }
    Value out;
    Parser parser(text);
    if (!parser.parse(out)) {
        return Value();
    }
    return out;
}

Value Value::parse_strict(const std::string& text) {
    Value out; Parser parser(text);
    return parser.parse(out, true) ? out : Value{};
}

const Value& Value::null_value() {
    static const Value instance;
    return instance;
}

std::size_t Value::size() const {
    if (type_ == Type::Array) return items_.size();
    if (type_ == Type::Object) return members_.size();
    return 0;
}

const Value& Value::at(std::size_t index) const {
    if (type_ == Type::Array && index < items_.size()) {
        return items_[index];
    }
    return null_value();
}

const Value* Value::find(const std::string& key) const {
    if (type_ != Type::Object) {
        return nullptr;
    }
    for (const Member& member : members_) {
        if (member.first == key) {
            return &member.second;
        }
    }
    return nullptr;
}

bool Value::get_bool(const std::string& key, bool& out) const {
    const Value* value = find(key);
    if (value == nullptr || !value->is_bool()) {
        return false;
    }
    out = value->bool_;
    return true;
}

bool Value::get_number(const std::string& key, double& out) const {
    const Value* value = find(key);
    if (value == nullptr || !value->is_number()) {
        return false;
    }
    out = value->number_;
    return true;
}

std::string Value::get_string(const std::string& key, const std::string& fallback) const {
    const Value* value = find(key);
    if (value == nullptr || !value->is_string()) {
        return fallback;
    }
    return value->string_;
}

std::string Value::dump() const {
    switch (type_) {
        case Type::Null:
            return "null";
        case Type::Bool:
            return bool_ ? "true" : "false";
        case Type::Number:
            return write_number(number_);
        case Type::String:
            return escape_string(string_);
        case Type::Array: {
            std::string out = "[";
            for (std::size_t i = 0; i < items_.size(); ++i) {
                if (i > 0) out.push_back(',');
                out += items_[i].dump();
            }
            out.push_back(']');
            return out;
        }
        case Type::Object: {
            std::string out = "{";
            for (std::size_t i = 0; i < members_.size(); ++i) {
                if (i > 0) out.push_back(',');
                out += escape_string(members_[i].first);
                out.push_back(':');
                out += members_[i].second.dump();
            }
            out.push_back('}');
            return out;
        }
    }
    return "null";
}

} // namespace json
} // namespace ravensight
