// SPDX-License-Identifier: GPL-3.0-or-later
// A small JSON value for the engine's HTTP API: parse request bodies, build
// and serialise responses. Numbers are kept as double plus, when exact, an
// int64 (sizes and timestamps stay exact).
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace alembic::engine {

class JsonValue {
public:
    enum class Type { null, boolean, number, string, array, object };

    JsonValue() = default;
    JsonValue(std::nullptr_t) {}
    JsonValue(bool b) : type_(Type::boolean), bool_(b) {}
    // Every integer type (int64_t is long on the PS5, long long on macOS).
    template <typename T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
    JsonValue(T v)
        : type_(Type::number), number_(static_cast<double>(v)), integer_(static_cast<std::int64_t>(v)), exact_(true) {}
    JsonValue(double v) : type_(Type::number), number_(v) {}
    JsonValue(const char* s) : type_(Type::string), string_(s) {}
    JsonValue(std::string s) : type_(Type::string), string_(std::move(s)) {}
    JsonValue(std::string_view s) : type_(Type::string), string_(s) {}

    static JsonValue array() {
        JsonValue v;
        v.type_ = Type::array;
        return v;
    }
    static JsonValue object() {
        JsonValue v;
        v.type_ = Type::object;
        return v;
    }

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::null; }
    bool is_object() const { return type_ == Type::object; }

    // Typed reads with a fallback for missing or mismatched values.
    std::string str(std::string fallback = {}) const { return type_ == Type::string ? string_ : fallback; }
    bool boolean(bool fallback = false) const { return type_ == Type::boolean ? bool_ : fallback; }
    std::int64_t integer(std::int64_t fallback = 0) const;
    double number(double fallback = 0) const { return type_ == Type::number ? number_ : fallback; }

    // Objects: a missing key reads as null.
    const JsonValue& operator[](std::string_view key) const;
    JsonValue& set(const std::string& key, JsonValue value);
    bool has(std::string_view key) const;
    const std::vector<std::pair<std::string, JsonValue>>& fields() const { return fields_; }

    // Arrays.
    JsonValue& push(JsonValue value);
    const std::vector<JsonValue>& items() const { return items_; }

    std::string dump() const;

private:
    void dump_to(std::string& out) const;

    Type type_ = Type::null;
    bool bool_ = false;
    double number_ = 0;
    std::int64_t integer_ = 0;
    bool exact_ = false;
    std::string string_;
    std::vector<JsonValue> items_;
    std::vector<std::pair<std::string, JsonValue>> fields_;
};

// Parses a whole document (RFC 8259, depth <= 64).
Result<JsonValue> parse_json(std::string_view text);

// The JSON string literal for `s` (with quotes). Invalid UTF-8 becomes U+FFFD.
std::string json_quote(std::string_view s);

} // namespace alembic::engine
