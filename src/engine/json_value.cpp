// SPDX-License-Identifier: GPL-3.0-or-later
#include "json_value.hpp"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace alembic::engine {

std::int64_t JsonValue::integer(std::int64_t fallback) const {
    if (type_ != Type::number) return fallback;
    if (exact_) return integer_;
    if (!std::isfinite(number_) || number_ != std::trunc(number_) || std::fabs(number_) > 9.0e15) return fallback;
    return static_cast<std::int64_t>(number_);
}

const JsonValue& JsonValue::operator[](std::string_view key) const {
    static const JsonValue null_value;
    if (type_ != Type::object) return null_value;
    for (const auto& [k, v] : fields_)
        if (k == key) return v;
    return null_value;
}

bool JsonValue::has(std::string_view key) const {
    if (type_ != Type::object) return false;
    for (const auto& [k, v] : fields_)
        if (k == key) return true;
    return false;
}

JsonValue& JsonValue::set(const std::string& key, JsonValue value) {
    type_ = Type::object;
    for (auto& [k, v] : fields_)
        if (k == key) {
            v = std::move(value);
            return v;
        }
    fields_.emplace_back(key, std::move(value));
    return fields_.back().second;
}

JsonValue& JsonValue::push(JsonValue value) {
    type_ = Type::array;
    items_.push_back(std::move(value));
    return items_.back();
}

std::string json_quote(std::string_view s) {
    std::string out = "\"";
    for (std::size_t i = 0; i < s.size();) {
        auto c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
            }
            ++i;
            continue;
        }
        // Copy a valid UTF-8 sequence, else U+FFFD.
        std::size_t len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        bool ok = len && i + len <= s.size() && c >= 0xC2 && c <= 0xF4;
        for (std::size_t k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        if (ok) {
            out.append(s.substr(i, len));
            i += len;
        } else {
            out += "\xEF\xBF\xBD";
            ++i;
        }
    }
    return out + "\"";
}

void JsonValue::dump_to(std::string& out) const {
    switch (type_) {
    case Type::null: out += "null"; break;
    case Type::boolean: out += bool_ ? "true" : "false"; break;
    case Type::number: {
        if (exact_) {
            out += std::to_string(integer_);
        } else if (!std::isfinite(number_)) {
            out += "null";
        } else {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.17g", number_);
            out += buf;
        }
        break;
    }
    case Type::string: out += json_quote(string_); break;
    case Type::array:
        out += '[';
        for (std::size_t i = 0; i < items_.size(); ++i) {
            if (i) out += ',';
            items_[i].dump_to(out);
        }
        out += ']';
        break;
    case Type::object:
        out += '{';
        for (std::size_t i = 0; i < fields_.size(); ++i) {
            if (i) out += ',';
            out += json_quote(fields_[i].first);
            out += ':';
            fields_[i].second.dump_to(out);
        }
        out += '}';
        break;
    }
}

std::string JsonValue::dump() const {
    std::string out;
    dump_to(out);
    return out;
}

namespace {

struct Parser {
    std::string_view s;
    std::size_t pos = 0;
    std::string error;

    bool fail(const char* message) {
        if (error.empty()) error = std::string("JSON: ") + message + " at offset " + std::to_string(pos);
        return false;
    }
    void ws() {
        while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) ++pos;
    }
    static void put_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(std::uint32_t& out) {
        if (pos + 4 > s.size()) return fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s[pos++];
            int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (h < 0) return fail("bad \\u escape");
            out = (out << 4) | static_cast<std::uint32_t>(h);
        }
        return true;
    }
    bool string(std::string& out) {
        ++pos; // opening quote
        while (pos < s.size()) {
            char c = s[pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in a string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= s.size()) break;
            char e = s[pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    std::uint32_t low;
                    if (pos + 2 > s.size() || s[pos] != '\\' || s[pos + 1] != 'u') return fail("lone surrogate");
                    pos += 2;
                    if (!hex4(low) || low < 0xDC00 || low > 0xDFFF) return fail("lone surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return fail("lone surrogate");
                }
                put_utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool number(JsonValue& out) {
        std::size_t start = pos;
        if (s[pos] == '-') ++pos;
        if (pos < s.size() && s[pos] == '0') {
            ++pos;
        } else if (pos < s.size() && s[pos] >= '1' && s[pos] <= '9') {
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        } else {
            return fail("bad number");
        }
        bool integral = true;
        if (pos < s.size() && s[pos] == '.') {
            integral = false;
            ++pos;
            if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return fail("bad number");
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        }
        if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
            integral = false;
            ++pos;
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
            if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') return fail("bad number");
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
        }
        std::string_view text = s.substr(start, pos - start);
        if (integral) {
            std::int64_t v = 0;
            auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec == std::errc() && r.ptr == text.data() + text.size()) {
                out = JsonValue(v);
                return true;
            }
        }
        std::string copy(text);
        out = JsonValue(std::strtod(copy.c_str(), nullptr));
        return true;
    }
    bool literal(std::string_view word) {
        if (s.substr(pos, word.size()) != word) return fail("bad literal");
        pos += word.size();
        return true;
    }
    bool value(JsonValue& out, int depth) {
        if (depth > 64) return fail("too deep");
        ws();
        if (pos >= s.size()) return fail("unexpected end");
        char c = s[pos];
        if (c == '{') {
            ++pos;
            out = JsonValue::object();
            ws();
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return true;
            }
            for (;;) {
                ws();
                if (pos >= s.size() || s[pos] != '"') return fail("expected a key");
                std::string key;
                if (!string(key)) return false;
                ws();
                if (pos >= s.size() || s[pos] != ':') return fail("expected ':'");
                ++pos;
                JsonValue v;
                if (!value(v, depth + 1)) return false;
                out.set(key, std::move(v));
                ws();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == '}') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++pos;
            out = JsonValue::array();
            ws();
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return true;
            }
            for (;;) {
                JsonValue v;
                if (!value(v, depth + 1)) return false;
                out.push(std::move(v));
                ws();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == ']') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string str;
            if (!string(str)) return false;
            out = JsonValue(std::move(str));
            return true;
        }
        if (c == 't') return literal("true") && (out = JsonValue(true), true);
        if (c == 'f') return literal("false") && (out = JsonValue(false), true);
        if (c == 'n') return literal("null") && (out = JsonValue(), true);
        return number(out);
    }
};

} // namespace

Result<JsonValue> parse_json(std::string_view text) {
    Parser p;
    p.s = text;
    JsonValue v;
    if (!p.value(v, 0)) return make_error(Errc::invalid_data, p.error);
    p.ws();
    if (p.pos != text.size()) return make_error(Errc::invalid_data, "JSON: trailing data");
    return v;
}

} // namespace alembic::engine
