// Copyright 2026 Reality Software Entertainment. MIT licensed.
//
// Tiny JSON writer and reader with no third party dependency.
//
// The writer serializes the SDK's own request bodies, which are flat
// key/value payloads. The reader is a small, tolerant parser used only to
// pull a handful of fields out of API responses (trackingEnabled, token,
// expiresAt, error, suggestions). Malformed input never throws: parsing
// failure yields a null Value, mirroring the Unity and Godot SDKs.

#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ravensight {
namespace json {

/// Returns the value as a quoted JSON string literal, escaping quotes,
/// backslashes, the named control escapes and every other control character
/// as \uXXXX. Bytes at or above 0x20 (including multi byte UTF-8 sequences)
/// pass through unchanged.
std::string escape_string(const std::string& value);

/// Serializes a double. NaN and infinity become null. Integral values inside
/// the safe integer range are written without a decimal point.
std::string write_number(double value);

/// Streaming writer for one flat JSON object.
class ObjectWriter {
public:
    ObjectWriter& add(const std::string& key, const std::string& value);
    ObjectWriter& add(const std::string& key, const char* value);
    ObjectWriter& add(const std::string& key, long long value);
    ObjectWriter& add(const std::string& key, int value) { return add(key, static_cast<long long>(value)); }
    ObjectWriter& add(const std::string& key, double value);
    ObjectWriter& add(const std::string& key, bool value);
    ObjectWriter& add_null(const std::string& key);
    /// Inserts already serialized JSON verbatim. The caller guarantees it is valid.
    ObjectWriter& add_raw(const std::string& key, const std::string& raw_json);
    /// Closes the object and returns it. The writer must not be reused after.
    std::string str();

private:
    void write_key(const std::string& key);
    std::string out_ = "{";
    bool first_ = true;
};

/// A parsed JSON value. Only what the SDK's response handling needs.
class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Member = std::pair<std::string, Value>;

    Value() = default;

    /// Parses JSON text. Returns a null Value on malformed input rather than
    /// throwing, because every caller is handling a network response that it
    /// must not crash on. Trailing garbage after a complete value is ignored.
    static Value parse(const std::string& text);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_bool() const { return type_ == Type::Bool; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_string() const { return type_ == Type::String; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const { return is_bool() ? bool_ : fallback; }
    double as_number(double fallback = 0.0) const { return is_number() ? number_ : fallback; }
    /// The string payload, or an empty string when this is not a string.
    const std::string& as_string() const { return string_; }

    /// Element count for arrays, member count for objects, otherwise 0.
    std::size_t size() const;
    /// Array element by index, or a shared null Value when out of range.
    const Value& at(std::size_t index) const;
    /// Object member by key, or nullptr when absent or not an object.
    const Value* find(const std::string& key) const;

    /// Typed member lookups. Each returns false (or the fallback) when the
    /// key is absent or holds a different type, never coercing.
    bool get_bool(const std::string& key, bool& out) const;
    bool get_number(const std::string& key, double& out) const;
    std::string get_string(const std::string& key, const std::string& fallback) const;

    const std::vector<Value>& items() const { return items_; }
    const std::vector<Member>& members() const { return members_; }

    /// Re-serializes this value. Useful for handing raw suggestion entries on
    /// to code that wants to parse them with its own JSON library.
    std::string dump() const;

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string string_;
    std::vector<Value> items_;
    std::vector<Member> members_;

    static const Value& null_value();
};

} // namespace json
} // namespace ravensight
