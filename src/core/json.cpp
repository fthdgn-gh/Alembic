// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/json.hpp"

#include "alembic/unicode.hpp"

namespace alembic::json {

namespace {

constexpr int kMaxDepth = 64;

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    Result<std::vector<Property>> run() {
        // A UTF-8 byte order mark is invalid here, as in .NET's
        // JsonDocument.Parse(bytes), which PS5PKGTool uses on param.json.
        skip_ws();
        std::vector<Property> props;
        bool root_is_object = peek() == '{';
        if (!value(1, root_is_object ? &props : nullptr)) return fail();
        skip_ws();
        if (pos_ != s_.size()) return fail();
        return props;
    }

private:
    Error fail() const { return make_error(Errc::invalid_data, "invalid JSON near byte " + std::to_string(pos_)); }
    char peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    bool eat(char c) {
        if (peek() != c || pos_ >= s_.size()) return false;
        ++pos_;
        return true;
    }
    void skip_ws() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) ++pos_;
    }
    bool literal(std::string_view word) {
        if (s_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    // `props` is non-null only for the root object.
    bool value(int depth, std::vector<Property>* props) {
        if (depth > kMaxDepth) return false;
        char c = peek();
        if (c == '{') return object(depth, props);
        if (c == '[') return array(depth);
        if (c == '"') return string(nullptr);
        if (c == 't') return literal("true");
        if (c == 'f') return literal("false");
        if (c == 'n') return literal("null");
        return number();
    }

    bool object(int depth, std::vector<Property>* props) {
        ++pos_;
        skip_ws();
        if (eat('}')) return true;
        for (;;) {
            skip_ws();
            std::string name;
            if (peek() != '"' || !string(&name)) return false;
            skip_ws();
            if (!eat(':')) return false;
            skip_ws();
            if (props && peek() == '"') {
                std::string text;
                if (!string(&text)) return false;
                props->push_back({std::move(name), std::move(text)});
            } else {
                if (!value(depth + 1, nullptr)) return false;
                if (props) props->push_back({std::move(name), std::nullopt});
            }
            skip_ws();
            if (eat('}')) return true;
            if (!eat(',')) return false;
        }
    }

    bool array(int depth) {
        ++pos_;
        skip_ws();
        if (eat(']')) return true;
        for (;;) {
            skip_ws();
            if (!value(depth + 1, nullptr)) return false;
            skip_ws();
            if (eat(']')) return true;
            if (!eat(',')) return false;
        }
    }

    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool hex4(std::uint32_t& out) {
        if (pos_ + 4 > s_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            int h = hex(s_[pos_++]);
            if (h < 0) return false;
            out = (out << 4) | static_cast<std::uint32_t>(h);
        }
        return true;
    }

    // Reads a string; decodes into `out` when given. Raw bytes must be valid UTF-8.
    bool string(std::string* out) {
        ++pos_; // opening quote
        std::u16string decoded;
        for (;;) {
            if (pos_ >= s_.size()) return false;
            auto c = static_cast<unsigned char>(s_[pos_]);
            if (c == '"') break;
            if (c < 0x20) return false;
            if (c != '\\') {
                // Copy a run of plain characters, validating UTF-8.
                std::size_t run = pos_;
                while (pos_ < s_.size() && s_[pos_] != '"' && s_[pos_] != '\\' &&
                       static_cast<unsigned char>(s_[pos_]) >= 0x20)
                    ++pos_;
                auto utf16 = unicode::utf8_to_utf16(s_.substr(run, pos_ - run));
                if (!utf16.ok()) return false;
                decoded += *utf16;
                continue;
            }
            ++pos_;
            char e = peek();
            ++pos_;
            switch (e) {
            case '"': decoded += u'"'; break;
            case '\\': decoded += u'\\'; break;
            case '/': decoded += u'/'; break;
            case 'b': decoded += u'\b'; break;
            case 'f': decoded += u'\f'; break;
            case 'n': decoded += u'\n'; break;
            case 'r': decoded += u'\r'; break;
            case 't': decoded += u'\t'; break;
            case 'u': {
                std::uint32_t unit;
                if (!hex4(unit)) return false;
                decoded += static_cast<char16_t>(unit);
                break;
            }
            default: return false;
            }
        }
        ++pos_; // closing quote
        if (out) *out = unicode::utf16_to_utf8(decoded);
        return true;
    }

    bool digits() {
        std::size_t start = pos_;
        while (peek() >= '0' && peek() <= '9' && pos_ < s_.size()) ++pos_;
        return pos_ > start;
    }

    bool number() {
        eat('-');
        if (peek() == '0' && pos_ < s_.size()) ++pos_;
        else if (!digits()) return false;
        if (eat('.') && !digits()) return false;
        if (peek() == 'e' || peek() == 'E') {
            ++pos_;
            if (!eat('+')) eat('-');
            if (!digits()) return false;
        }
        return true;
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

} // namespace

Result<std::vector<Property>> top_level_properties(std::string_view document) {
    return Parser(document).run();
}

} // namespace alembic::json
