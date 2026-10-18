// SPDX-License-Identifier: GPL-3.0-or-later
#include "l10n.hpp"
#include "json_value.hpp"

#include <map>
#include <mutex>

namespace alembic::engine::l10n {

namespace {

std::mutex g_mutex;
std::map<std::string, std::map<std::string, std::string>> g_tables; // language -> English -> text
std::vector<std::string> g_patterns;                                // longest first, as in the file
std::string g_language = "en";

const std::map<std::string, std::string>* table() {
    auto it = g_tables.find(g_language);
    return it == g_tables.end() ? nullptr : &it->second;
}

std::string fill(std::string text, const Vars& vars) {
    for (const auto& [name, value] : vars) {
        const std::string key = "{" + name + "}";
        for (std::size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + value.size())) {
            text.replace(at, key.size(), value);
        }
    }
    return text;
}

// Pieces of a pattern: literal, name, literal, ..., literal.
std::vector<std::string> split(const std::string& pattern) {
    std::vector<std::string> parts{""};
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] == '{') {
            std::size_t close = pattern.find('}', i);
            bool word = close != std::string::npos && close > i + 1;
            for (std::size_t k = i + 1; word && k < close; ++k) {
                char c = pattern[k];
                word = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
            }
            if (word) {
                parts.push_back(pattern.substr(i + 1, close - i - 1));
                parts.push_back("");
                i = close + 1;
                continue;
            }
        }
        parts.back() += pattern[i++];
    }
    return parts;
}

std::string message_locked(const std::string& text, int depth) {
    const auto* d = table();
    if (!d || text.empty()) return text;
    if (auto it = d->find(text); it != d->end()) return it->second;
    if (depth > 4) return text;
    for (const std::string& p : g_patterns) {
        Vars vars;
        if (!match(p, text, vars)) continue;
        for (auto& [name, value] : vars) value = message_locked(value, depth + 1);
        auto it = d->find(p);
        return fill(it == d->end() ? p : it->second, vars);
    }
    return text;
}

} // namespace

void load(const char* data, std::size_t size) {
    std::string text(data, size);
    std::size_t start = text.find("var I18N =");
    start = start == std::string::npos ? std::string::npos : text.find('{', start);
    std::size_t end = text.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end < start) return;
    auto json = parse_json(text.substr(start, end - start + 1));
    if (!json.ok()) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_tables.clear();
    g_patterns.clear();
    for (const auto& [key, value] : json->fields()) {
        if (key == "messages") {
            for (const JsonValue& p : value.items()) g_patterns.push_back(p.str());
            continue;
        }
        auto& t = g_tables[key];
        for (const auto& [english, translated] : value.fields()) t[english] = translated.str();
    }
}

void set_language(const std::string& code) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_language = code == "en" || g_tables.count(code) ? code : "en";
}

std::string language() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_language;
}

bool has_language(const std::string& code) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return code == "en" || g_tables.count(code) > 0;
}

std::string t(const std::string& english, const Vars& vars) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto* d = table();
    auto it = d ? d->find(english) : decltype(d->end()){};
    return fill(d && it != d->end() ? it->second : english, vars);
}

std::string message(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return message_locked(text, 0);
}

bool match(const std::string& pattern, const std::string& text, Vars& out) {
    std::vector<std::string> parts = split(pattern);
    if (parts.size() == 1 || text.compare(0, parts[0].size(), parts[0]) != 0) return false;
    std::size_t pos = parts[0].size();
    Vars vars;
    for (std::size_t i = 1; i < parts.size(); i += 2) {
        const std::string& next = parts[i + 1];
        bool last = i + 2 >= parts.size();
        std::size_t end;
        if (last) {
            if (text.size() < next.size()) return false;
            end = text.size() - next.size();
            if (end <= pos || text.compare(end, next.size(), next) != 0) return false;
        } else {
            end = text.find(next, pos + 1);
            if (end == std::string::npos) return false;
        }
        vars.emplace_back(parts[i], text.substr(pos, end - pos));
        pos = end + next.size();
    }
    out = std::move(vars);
    return true;
}

} // namespace alembic::engine::l10n
