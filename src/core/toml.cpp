// SPDX-License-Identifier: GPL-3.0-or-later
// TOML subset reader. Follows the TOML 1.0 grammar for the parts it supports;
// the supported subset matches PS5-LZ4-Compression's src/ampr_pack_toml.c
// (GPL-3.0-or-later), plus inline tables.
#include "alembic/toml.hpp"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace alembic::toml {

const Value* Value::get(std::string_view key) const {
    if (type != Type::table) return nullptr;
    for (const auto& [k, v] : fields)
        if (k == key) return &v;
    return nullptr;
}

namespace {

// Tables get a flag for "defined by a [header] or by key assignments", so a
// second [a] is an error while [a.b] after [a] is fine.
struct Parser {
    std::string_view text;
    std::size_t pos = 0;
    int line = 1;
    std::string error;

    bool done() const { return pos >= text.size(); }
    char peek(std::size_t ahead = 0) const { return pos + ahead < text.size() ? text[pos + ahead] : '\0'; }

    bool fail(const std::string& message) {
        if (error.empty()) error = "TOML line " + std::to_string(line) + ": " + message;
        return false;
    }

    void skip_spaces() {
        while (!done() && (peek() == ' ' || peek() == '\t')) ++pos;
    }
    void skip_comment() {
        if (peek() == '#')
            while (!done() && peek() != '\n') ++pos;
    }
    // Spaces, comments and line breaks (inside arrays, between statements).
    void skip_blank() {
        for (;;) {
            skip_spaces();
            skip_comment();
            if (peek() == '\r' && peek(1) == '\n') ++pos;
            if (peek() == '\n') {
                ++pos;
                ++line;
                continue;
            }
            return;
        }
    }
    bool end_of_line() {
        skip_spaces();
        skip_comment();
        if (done()) return true;
        if (peek() == '\r' && peek(1) == '\n') ++pos;
        if (peek() == '\n') {
            ++pos;
            ++line;
            return true;
        }
        return fail("unexpected text after a value");
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
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool string(std::string& out) {
        char quote = peek();
        if (peek(1) == quote && peek(2) == quote) return fail("multi-line strings are not supported");
        ++pos;
        out.clear();
        while (!done() && peek() != quote) {
            char c = peek();
            if (c == '\n' || c == '\r') return fail("unterminated string");
            if (quote == '"' && c == '\\') {
                ++pos;
                char e = peek();
                ++pos;
                int digits = 0;
                switch (e) {
                case 'b': out += '\b'; continue;
                case 't': out += '\t'; continue;
                case 'n': out += '\n'; continue;
                case 'f': out += '\f'; continue;
                case 'r': out += '\r'; continue;
                case '"': out += '"'; continue;
                case '\\': out += '\\'; continue;
                case 'u': digits = 4; break;
                case 'U': digits = 8; break;
                default: return fail("bad escape in a string");
                }
                std::uint32_t cp = 0;
                for (int i = 0; i < digits; ++i) {
                    int h = hex(peek());
                    if (h < 0) return fail("bad unicode escape");
                    cp = (cp << 4) | static_cast<std::uint32_t>(h);
                    ++pos;
                }
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return fail("bad unicode escape");
                put_utf8(out, cp);
                continue;
            }
            out += c;
            ++pos;
        }
        if (done()) return fail("unterminated string");
        ++pos;
        return true;
    }

    static bool bare_key_char(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    bool key_path(std::vector<std::string>& out) {
        out.clear();
        for (;;) {
            skip_spaces();
            std::string segment;
            if (peek() == '"' || peek() == '\'') {
                if (!string(segment)) return false;
            } else {
                std::size_t start = pos;
                while (!done() && bare_key_char(peek())) ++pos;
                if (pos == start) return fail("bad key");
                segment.assign(text.substr(start, pos - start));
            }
            out.push_back(std::move(segment));
            skip_spaces();
            if (peek() != '.') return true;
            ++pos;
        }
    }

    bool number(Value& out) {
        std::size_t start = pos;
        while (!done()) {
            char c = peek();
            if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '+' ||
                c == '-' || c == '.')
                ++pos;
            else
                break;
        }
        std::string token(text.substr(start, pos - start));
        if (token.empty()) return fail("missing value");
        if (token == "true" || token == "false") {
            out.type = Value::Type::boolean;
            out.boolean = token == "true";
            return true;
        }
        std::string body = token;
        std::string sign;
        if (body[0] == '+' || body[0] == '-') {
            sign = body.substr(0, 1);
            body = body.substr(1);
        }
        if (body == "inf" || body == "nan") {
            out.type = Value::Type::floating;
            out.floating = body == "inf" ? std::numeric_limits<double>::infinity()
                                         : std::numeric_limits<double>::quiet_NaN();
            if (sign == "-") out.floating = -out.floating;
            return true;
        }
        // Underscores only between digits.
        std::string digits;
        for (std::size_t i = 0; i < body.size(); ++i) {
            if (body[i] != '_') {
                digits += body[i];
                continue;
            }
            auto is_digit = [&](std::size_t j) { return j < body.size() && hex(body[j]) >= 0; };
            if (i == 0 || !is_digit(i - 1) || !is_digit(i + 1)) return fail("bad number: " + token);
        }
        if (digits.empty()) return fail("bad value: " + token);
        int base = 10;
        if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'o' || digits[1] == 'b')) {
            if (!sign.empty()) return fail("bad number: " + token);
            base = digits[1] == 'x' ? 16 : digits[1] == 'o' ? 8 : 2;
            digits = digits.substr(2);
        }
        bool is_float = base == 10 && digits.find_first_of(".eE") != std::string::npos;
        for (char c : digits) {
            bool ok = base == 16   ? hex(c) >= 0
                      : base == 8  ? (c >= '0' && c <= '7')
                      : base == 2  ? (c == '0' || c == '1')
                      : is_float   ? ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-')
                                   : (c >= '0' && c <= '9');
            if (!ok) return fail("bad value: " + token);
        }
        if (base == 10 && digits.size() > 1 && digits[0] == '0' && digits[1] != '.' && digits[1] != 'e' &&
            digits[1] != 'E')
            return fail("leading zeros are not allowed: " + token);
        std::string full = sign + digits;
        errno = 0;
        char* end = nullptr;
        if (is_float) {
            double v = std::strtod(full.c_str(), &end);
            if (errno || !end || *end) return fail("bad float: " + token);
            out.type = Value::Type::floating;
            out.floating = v;
        } else {
            long long v = std::strtoll(full.c_str(), &end, base);
            if (errno || !end || *end) return fail("bad integer: " + token);
            out.type = Value::Type::integer;
            out.integer = v;
        }
        return true;
    }

    bool value(Value& out, int depth) {
        if (depth > 32) return fail("values nest too deeply");
        skip_spaces();
        char c = peek();
        if (done() || c == '\n' || c == '\r') return fail("missing value");
        if (c == '"' || c == '\'') {
            out.type = Value::Type::string;
            return string(out.string);
        }
        if (c == '[') {
            ++pos;
            out.type = Value::Type::array;
            for (;;) {
                skip_blank();
                if (done()) return fail("unterminated array");
                if (peek() == ']') {
                    ++pos;
                    return true;
                }
                Value item;
                if (!value(item, depth + 1)) return false;
                out.items.push_back(std::move(item));
                skip_blank();
                if (peek() == ',') {
                    ++pos;
                    continue;
                }
                if (peek() == ']') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or ']' in an array");
            }
        }
        if (c == '{') {
            ++pos;
            out.type = Value::Type::table;
            skip_spaces();
            if (peek() == '}') {
                ++pos;
                return true;
            }
            for (;;) {
                std::vector<std::string> path;
                if (!key_path(path)) return false;
                skip_spaces();
                if (peek() != '=') return fail("expected '=' after a key");
                ++pos;
                Value v;
                if (!value(v, depth + 1)) return false;
                if (!assign(out, path, std::move(v))) return false;
                skip_spaces();
                if (peek() == ',') {
                    ++pos;
                    continue;
                }
                if (peek() == '}') {
                    ++pos;
                    return true;
                }
                return fail("expected ',' or '}' in an inline table");
            }
        }
        if (c >= '0' && c <= '9' && peek(1) >= '0' && peek(1) <= '9' && peek(2) >= '0' && peek(2) <= '9' &&
            peek(3) >= '0' && peek(3) <= '9' && peek(4) == '-')
            return fail("dates are not supported");
        return number(out);
    }

    static Value* field(Value& table, const std::string& key) {
        for (auto& [k, v] : table.fields)
            if (k == key) return &v;
        return nullptr;
    }

    // Puts `v` at `path` under `table`, creating intermediate tables.
    bool assign(Value& table, const std::vector<std::string>& path, Value v) {
        Value* t = &table;
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            Value* next = field(*t, path[i]);
            if (!next) {
                t->fields.emplace_back(path[i], Value{});
                next = &t->fields.back().second;
            } else if (!next->is_table()) {
                return fail("key '" + path[i] + "' is not a table");
            }
            t = next;
        }
        if (field(*t, path.back())) return fail("duplicate key '" + path.back() + "'");
        t->fields.emplace_back(path.back(), std::move(v));
        return true;
    }

    // Walks to the table a [header] or [[header]] names.
    bool header(Value& root, Value*& current, std::vector<std::vector<std::string>>& defined) {
        bool array = peek(1) == '[';
        pos += array ? 2 : 1;
        std::vector<std::string> path;
        if (!key_path(path)) return false;
        skip_spaces();
        if (peek() != ']' || (array && peek(1) != ']')) return fail("unterminated table header");
        pos += array ? 2 : 1;
        if (!end_of_line()) return false;

        Value* t = &root;
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            Value* next = field(*t, path[i]);
            if (!next) {
                t->fields.emplace_back(path[i], Value{});
                next = &t->fields.back().second;
            }
            if (next->type == Value::Type::array && next->table_array && !next->items.empty())
                next = &next->items.back();
            else if (!next->is_table())
                return fail("key '" + path[i] + "' is not a table");
            t = next;
        }
        Value* last = field(*t, path.back());
        if (array) {
            if (!last) {
                Value a;
                a.type = Value::Type::array;
                a.table_array = true;
                t->fields.emplace_back(path.back(), std::move(a));
                last = &t->fields.back().second;
            } else if (last->type != Value::Type::array || !last->table_array) {
                return fail("'" + path.back() + "' is already defined and is not an array of tables");
            }
            last->items.emplace_back();
            current = &last->items.back();
            return true;
        }
        // A header may name a table that only exists implicitly so far,
        // never one defined before.
        for (const auto& d : defined)
            if (d == path) return fail("table '" + path.back() + "' is defined twice");
        if (!last) {
            t->fields.emplace_back(path.back(), Value{});
            last = &t->fields.back().second;
        } else if (!last->is_table()) {
            return fail("key '" + path.back() + "' is not a table");
        }
        defined.push_back(path);
        current = last;
        return true;
    }

    bool document(Value& root) {
        // Byte-order mark.
        if (text.substr(0, 3) == "\xEF\xBB\xBF") pos = 3;
        Value* current = &root;
        std::vector<std::vector<std::string>> defined;
        for (;;) {
            skip_blank();
            if (done()) return true;
            if (peek() == '[') {
                if (!header(root, current, defined)) return false;
                continue;
            }
            std::vector<std::string> path;
            if (!key_path(path)) return false;
            skip_spaces();
            if (peek() != '=') return fail("expected '=' after a key");
            ++pos;
            Value v;
            if (!value(v, 0)) return false;
            if (!assign(*current, path, std::move(v))) return false;
            if (!end_of_line()) return false;
        }
    }
};

} // namespace

Result<Value> parse(std::string_view text) {
    Parser p;
    p.text = text;
    Value root;
    if (!p.document(root)) return make_error(Errc::invalid_data, p.error);
    return root;
}

} // namespace alembic::toml
