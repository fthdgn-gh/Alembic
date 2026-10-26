// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/sfo.hpp"

#include "alembic/bytes.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstdio>
#include <map>

#include <cstring>

namespace alembic::sfo {

namespace {

constexpr std::size_t kHeader = 0x14;
constexpr std::size_t kRow = 0x10;
constexpr std::uint32_t kMaxEntries = 4096;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, "param.sfo: " + message);
}

} // namespace

bool looks_like_sfo(Bytes data) {
    return data.size() >= kHeader && std::memcmp(data.data(), "\0PSF", 4) == 0;
}

Result<Param> parse(Bytes data) {
    if (!looks_like_sfo(data)) return invalid("no PSF header");
    std::uint32_t keys = get_u32(data.data() + 0x08);
    std::uint32_t values = get_u32(data.data() + 0x0C);
    std::uint32_t count = get_u32(data.data() + 0x10);
    if (count > kMaxEntries || kHeader + std::uint64_t(count) * kRow > data.size() || keys > data.size() ||
        values > data.size())
        return invalid("the tables lie outside the file");
    Param out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* row = data.data() + kHeader + std::size_t(i) * kRow;
        std::uint64_t key_at = std::uint64_t(keys) + get_u16(row);
        std::uint16_t format = get_u16(row + 2);
        std::uint32_t length = get_u32(row + 4);
        std::uint64_t value_at = std::uint64_t(values) + get_u32(row + 12);
        if (key_at >= data.size() || value_at > data.size() || length > data.size() - value_at)
            return invalid("an entry lies outside the file");
        const char* key_start = reinterpret_cast<const char*>(data.data() + key_at);
        const void* key_end = std::memchr(key_start, 0, data.size() - key_at);
        if (!key_end) return invalid("a key is not terminated");
        std::string key(key_start, static_cast<const char*>(key_end));
        const std::uint8_t* value = data.data() + value_at;
        if (format == 0x0404) {
            if (length < 4) return invalid("a number is too short");
            out.numbers[key] = get_u32(value);
        } else if (format == 0x0004 || format == 0x0204) {
            std::size_t n = 0;
            while (n < length && value[n] != 0) ++n;
            out.text[key] = std::string(reinterpret_cast<const char*>(value), n);
        }
    }
    return out;
}

Result<Document> Document::read(Bytes data) {
    if (!looks_like_sfo(data)) return invalid("no PSF header");
    std::uint32_t keys = get_u32(data.data() + 0x08);
    std::uint32_t values = get_u32(data.data() + 0x0C);
    std::uint32_t count = get_u32(data.data() + 0x10);
    if (count > kMaxEntries || kHeader + std::uint64_t(count) * kRow > data.size() || keys > data.size() ||
        values > data.size())
        return invalid("the tables lie outside the file");
    Document doc;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* row = data.data() + kHeader + std::size_t(i) * kRow;
        std::uint64_t key_at = std::uint64_t(keys) + get_u16(row);
        Value v;
        v.format = get_u16(row + 2);
        std::uint32_t length = get_u32(row + 4);
        v.max_length = get_u32(row + 8);
        std::uint64_t value_at = std::uint64_t(values) + get_u32(row + 12);
        if (key_at >= data.size() || value_at > data.size()) return invalid("an entry lies outside the file");
        const char* key_start = reinterpret_cast<const char*>(data.data() + key_at);
        const void* key_end = std::memchr(key_start, 0, data.size() - key_at);
        if (!key_end) return invalid("a key is not terminated");
        v.key.assign(key_start, static_cast<const char*>(key_end));
        const std::uint8_t* value = data.data() + value_at;
        std::uint64_t left = data.size() - value_at;
        if (v.format == kInteger) {
            if (left < 4) return invalid("a number lies outside the file");
            v.number = get_u32(value);
            v.max_length = 4;
        } else if (v.format == kUtf8 || v.format == kUtf8Special) {
            // ParamSfo reads length - 1 bytes of UTF-8 text (all of them for the special format).
            std::uint32_t take = v.format == kUtf8 && length > 0 ? length - 1 : length;
            if (take > left) return invalid("a value lies outside the file");
            v.text.assign(reinterpret_cast<const char*>(value), take);
        } else {
            char hex[8];
            std::snprintf(hex, sizeof hex, "%04X", v.format);
            return invalid(std::string("unknown value format ") + hex);
        }
        doc.values_.push_back(std::move(v));
    }
    return doc;
}

const Value* Document::find(const std::string& key) const {
    for (const Value& v : values_)
        if (v.key == key) return &v;
    return nullptr;
}

void Document::sort() {
    std::sort(values_.begin(), values_.end(),
              [](const Value& a, const Value& b) { return unicode::dotnet_compare(a.key, b.key) < 0; });
}

void Document::remove(const std::string& key) {
    values_.erase(std::remove_if(values_.begin(), values_.end(), [&](const Value& v) { return v.key == key; }),
                  values_.end());
}

void Document::set_text(const std::string& key, const std::string& text, std::uint32_t max_length) {
    remove(key);
    Value v;
    v.key = key;
    v.format = kUtf8;
    v.text = text;
    v.max_length = max_length;
    values_.push_back(std::move(v));
    sort();
}

void Document::set_number(const std::string& key, std::uint32_t number) {
    remove(key);
    Value v;
    v.key = key;
    v.format = kInteger;
    v.number = number;
    v.max_length = 4;
    values_.push_back(std::move(v));
    sort();
}

std::string Document::display(const std::string& key) const {
    const Value* v = find(key);
    if (!v) return "";
    if (v->format != kInteger) return v->text;
    char text[16];
    std::snprintf(text, sizeof text, "0x%08x", v->number);
    return text;
}

std::vector<std::uint8_t> Document::serialize() const {
    std::vector<Value> sorted = values_;
    std::sort(sorted.begin(), sorted.end(),
              [](const Value& a, const Value& b) { return unicode::dotnet_compare(a.key, b.key) < 0; });
    std::size_t key_table = kHeader + sorted.size() * kRow;
    std::size_t key_size = 0, data_size = 0;
    for (const Value& v : sorted) {
        key_size += v.key.size() + 1;
        data_size += v.max_length;
    }
    std::size_t data_table = key_table + key_size;
    if (data_table % 4) data_table += 4 - data_table % 4;
    std::vector<std::uint8_t> out(data_table + data_size, 0);
    // Writes may run past a slot (a value longer than its maximum); like a
    // MemoryStream, the file then grows and the next value overwrites.
    auto put = [&](std::size_t at, const std::uint8_t* p, std::size_t n) {
        if (at + n > out.size()) out.resize(at + n, 0);
        std::memcpy(out.data() + at, p, n);
    };
    std::memcpy(out.data(), "\0PSF", 4);
    put_u32(&out[4], 0x101);
    put_u32(&out[8], static_cast<std::uint32_t>(key_table));
    put_u32(&out[12], static_cast<std::uint32_t>(data_table));
    put_u32(&out[16], static_cast<std::uint32_t>(sorted.size()));
    std::size_t key_offset = 0, data_offset = 0;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const Value& v = sorted[i];
        std::vector<std::uint8_t> bytes;
        std::uint32_t length;
        if (v.format == kInteger) {
            bytes.resize(4);
            put_u32(bytes.data(), v.number);
            length = 4;
        } else {
            bytes.assign(v.text.begin(), v.text.end());
            if (v.format == kUtf8) bytes.push_back(0);
            length = static_cast<std::uint32_t>(bytes.size());
        }
        std::uint8_t row[16] = {};
        put_u16(row, static_cast<std::uint16_t>(key_offset));
        put_u16(row + 2, v.format);
        put_u32(row + 4, length);
        put_u32(row + 8, v.max_length);
        put_u32(row + 12, static_cast<std::uint32_t>(data_offset));
        put(kHeader + i * kRow, row, sizeof row);
        put(key_table + key_offset, reinterpret_cast<const std::uint8_t*>(v.key.data()), v.key.size());
        std::uint8_t zero = 0;
        put(key_table + key_offset + v.key.size(), &zero, 1);
        if (!bytes.empty()) put(data_table + data_offset, bytes.data(), bytes.size());
        key_offset += v.key.size() + 1;
        data_offset += v.max_length;
    }
    return out;
}

namespace {

// SCE system-language order of TITLE_00..TITLE_29, as appmeta spells them.
const char* const kTitleLocales[30] = {
    "ja-JP", "en-US", "fr-FR", "es-ES", "de-DE", "it-IT", "nl-NL",   "pt-PT", "ru-RU", "ko-KR",
    "zh-Hant", "zh-Hans", "fi-FI", "sv-SE", "da-DK", "no-NO", "pl-PL", "pt-BR", "en-GB", "tr-TR",
    "es-419", "ar-AE", "fr-CA", "cs-CZ", "hu-HU", "el-GR", "ro-RO", "th-TH", "vi-VN", "id-ID"};

std::string strip(const std::string& s) {
    const char* ws = " \t\n\r\f\v";
    std::size_t a = s.find_first_not_of(ws);
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// A JSON string as Python's json.dumps(ensure_ascii=False) writes it.
std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char hex[8];
                std::snprintf(hex, sizeof hex, "\\u%04x", c);
                out += hex;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

} // namespace

std::string ps4_param_json(const Document& sfo, const std::string& title_id) {
    auto text = [&](const std::string& key) -> std::string {
        const Value* v = sfo.find(key);
        return v && v->format != kInteger ? strip(v->text) : std::string();
    };
    char key[32]; // up to "USER_DEFINED_PARAM_4"
    // choose_title: TITLE, else TITLE_01, else the first other TITLE_NN, else the IDs.
    std::string title = text("TITLE");
    if (title.empty()) title = text("TITLE_01");
    for (int i = 0; i < 30 && title.empty(); ++i) {
        if (i == 1) continue;
        std::snprintf(key, sizeof key, "TITLE_%02d", i);
        title = text(key);
    }
    if (title.empty()) title = text("CONTENT_ID");
    if (title.empty()) title = text("TITLE_ID");
    if (title.empty()) title = "Unknown Game";

    // localizedParameters, its keys sorted by code point (Python's sort_keys).
    std::map<std::string, std::string> names; // locale -> titleName
    for (int i = 0; i < 30; ++i) {
        std::snprintf(key, sizeof key, "TITLE_%02d", i);
        std::string t = text(key);
        if (!t.empty()) names[kTitleLocales[i]] = t;
    }
    std::string english = text("TITLE_01");
    names["en-US"] = english.empty() ? title : english;
    std::map<std::string, std::string> localized; // key -> rendered value
    for (const auto& [locale, name] : names)
        localized[locale] = "{\n      \"titleName\": " + quoted(name) + "\n    }";
    localized["defaultLanguage"] = quoted("en-US");

    std::map<std::string, std::string> top;
    std::string body = "{\n";
    bool first = true;
    for (const auto& [k, v] : localized) {
        body += (first ? "" : ",\n") + std::string("    ") + quoted(k) + ": " + v;
        first = false;
    }
    top["localizedParameters"] = body + "\n  }";
    top["titleId"] = quoted(title_id);
    top["titleName"] = quoted(title);
    for (int i = 1; i <= 4; ++i) {
        std::snprintf(key, sizeof key, "USER_DEFINED_PARAM_%d", i);
        const Value* v = sfo.find(key);
        if (v && v->format == kInteger && v->number != 0)
            top["userDefinedParam" + std::to_string(i)] = std::to_string(v->number);
    }
    std::string out = "{\n";
    first = true;
    for (const auto& [k, v] : top) {
        out += (first ? "" : ",\n") + std::string("  ") + quoted(k) + ": " + v;
        first = false;
    }
    return out + "\n}\n";
}

} // namespace alembic::sfo
