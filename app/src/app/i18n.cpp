// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/i18n.hpp"

#include "json_value.hpp"

#include <map>

namespace alembic::app {

namespace {

struct Tables {
    std::map<std::string, std::map<std::string, std::string, std::less<>>> langs;
    std::vector<std::string> messages;
    std::string current = "en";
};
Tables& tables() {
    static Tables t;
    return t;
}

const std::map<std::string, std::string, std::less<>>* table() {
    Tables& T = tables();
    if (!language_drawable(T.current)) return nullptr;
    auto it = T.langs.find(T.current);
    return it == T.langs.end() ? nullptr : &it->second;
}

std::string fill(std::string out, Vars vars) {
    std::string result;
    for (std::size_t i = 0; i < out.size();) {
        if (out[i] == '{') {
            const std::size_t end = out.find('}', i);
            if (end != std::string::npos) {
                const std::string key = out.substr(i + 1, end - i - 1);
                bool found = false;
                for (const auto& [k, v] : vars) {
                    if (key == k) {
                        result += v;
                        found = true;
                        break;
                    }
                }
                if (found) {
                    i = end + 1;
                    continue;
                }
            }
        }
        result += out[i++];
    }
    return result;
}

// {name: text} when text has the shape of pattern ("cannot write '{path}': {reason}").
bool match(const std::string& pattern, std::string_view text, std::vector<std::pair<std::string, std::string>>* vars) {
    std::vector<std::string> parts;  // literal, name, literal, ..., literal
    std::size_t at = 0;
    for (;;) {
        const std::size_t open = pattern.find('{', at);
        if (open == std::string::npos) {
            parts.push_back(pattern.substr(at));
            break;
        }
        const std::size_t close = pattern.find('}', open);
        if (close == std::string::npos) {
            parts.push_back(pattern.substr(at));
            break;
        }
        parts.push_back(pattern.substr(at, open - at));
        parts.push_back(pattern.substr(open + 1, close - open - 1));
        at = close + 1;
    }
    if (parts.size() == 1 || text.compare(0, parts[0].size(), parts[0]) != 0) return false;
    std::size_t pos = parts[0].size();
    for (std::size_t i = 1; i < parts.size(); i += 2) {
        const std::string& next = parts[i + 1];
        const bool last = i + 2 >= parts.size();
        std::size_t end;
        if (last) {
            if (text.size() < next.size()) return false;
            end = text.size() - next.size();
            if (end <= pos || text.substr(end) != next) return false;
        } else {
            end = text.find(next, pos + 1);
            if (end == std::string_view::npos) return false;
        }
        vars->push_back({parts[i], std::string(text.substr(pos, end - pos))});
        pos = end + next.size();
    }
    return true;
}

}  // namespace

const std::vector<std::pair<std::string, std::string>>& languages() {
    static const std::vector<std::pair<std::string, std::string>> list = {
        {"en", "English"}, {"es", "Español"}, {"fr", "Français"}, {"de", "Deutsch"}, {"ja", "日本語"}, {"tr", "Türkçe"}};
    return list;
}

bool load_i18n(std::string_view script) {
    const std::size_t eq = script.find('=');
    if (eq == std::string_view::npos) return false;
    std::string_view json = script.substr(eq + 1);
    while (!json.empty() && (json.back() == ';' || json.back() == '\n' || json.back() == ' ' || json.back() == '\r'))
        json.remove_suffix(1);
    auto parsed = engine::parse_json(json);
    if (!parsed || !parsed->is_object()) return false;
    Tables& T = tables();
    T.langs.clear();
    T.messages.clear();
    for (const auto& [key, value] : parsed->fields()) {
        if (key == "messages") {
            for (const auto& m : value.items()) T.messages.push_back(m.str());
            continue;
        }
        auto& dest = T.langs[key];
        for (const auto& [k, v] : value.fields()) dest[k] = v.str();
    }
    return true;
}

void set_language(const std::string& code) { tables().current = code.empty() ? "en" : code; }
const std::string& language() { return tables().current; }
bool language_drawable(const std::string& code) { return code != "ja"; }

std::string t(std::string_view text) {
    const auto* d = table();
    if (d) {
        auto it = d->find(text);
        if (it != d->end()) return it->second;
    }
    return std::string(text);
}

std::string t(std::string_view text, Vars vars) { return fill(t(text), vars); }

std::string number(long long n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    const std::string& lang = language();
    const char* sep = lang == "en" || lang == "ja" ? "," : lang == "fr" ? "\xE2\x80\xAF" : ".";
    std::string out;
    const int len = static_cast<int>(digits.size());
    for (int i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0 && !(lang == "es" && len == 4)) out += sep;
        out += digits[static_cast<std::size_t>(i)];
    }
    return n < 0 ? "-" + out : out;
}

std::string plural(long long n, std::string_view one, std::string_view many) {
    return t(n == 1 ? one : many, {{"n", number(n)}});
}

std::string t_msg(std::string_view text) {
    if (text.empty()) return "";
    const auto* d = table();
    if (!d) return std::string(text);
    auto it = d->find(text);
    if (it != d->end()) return it->second;
    for (const std::string& p : tables().messages) {
        std::vector<std::pair<std::string, std::string>> vars;
        if (!match(p, text, &vars)) continue;
        std::string out = t(p);
        for (auto& [k, v] : vars) {
            const std::string key = "{" + k + "}";
            const std::string value = t_msg(v);
            for (std::size_t at = out.find(key); at != std::string::npos; at = out.find(key, at + value.size()))
                out.replace(at, key.size(), value);
        }
        return out;
    }
    return std::string(text);
}

std::string place_label(std::string_view label) {
    std::string s(label);
    if (s == "Internal storage") return t("Internal storage");
    if (s.compare(0, 8, "Internal") == 0 && (s.size() == 8 || s[8] == ' ')) return t("Internal") + s.substr(8);
    for (const char* prefix : {"USB ", "Extended "}) {
        const std::size_t n = std::char_traits<char>::length(prefix);
        if (s.compare(0, n, prefix) == 0 && s.size() > n && s[n] >= '0' && s[n] <= '9') {
            std::size_t end = n;
            while (end < s.size() && s[end] >= '0' && s[end] <= '9') ++end;
            const std::string num = s.substr(n, end - n);
            return t(prefix[0] == 'U' ? "USB {n}" : "Extended {n}", {{"n", num}}) + s.substr(end);
        }
    }
    return s;
}

std::string t_method(std::string_view m) {
    if (m.empty()) return "";
    for (const char* head : {"LZ4-packed", "LZ4-unpacked", "added the AMPR emulator"}) {
        const std::string h = std::string(head) + " and ";
        if (m.compare(0, h.size(), h) == 0)
            return t(std::string(head) + " and {rest}", {{"rest", t_method(m.substr(h.size()))}});
    }
    for (const char* kind : {"exFAT", "UFS2", "PFS"}) {
        const std::string a = std::string("compressed the ") + kind + " image unchanged";
        if (m == a) return t("compressed the {kind} image unchanged", {{"kind", kind}});
        if (m == "re" + a) return t("recompressed the {kind} image unchanged", {{"kind", kind}});
    }
    return t(m);
}

std::string t_status(std::string_view s) {
    // "message (at start ...; ...)": the message translated, the rest kept.
    const std::size_t open = s.rfind(" (");
    if (open != std::string_view::npos && s.back() == ')') {
        const std::string_view inner = s.substr(open + 2);
        if (inner.compare(0, 8, "at start") == 0 || inner.compare(0, 8, "added by") == 0 ||
            inner.compare(0, 18, "registered again by") == 0)
            return t_msg(s.substr(0, open)) + std::string(s.substr(open));
    }
    return t_msg(s);
}

}  // namespace alembic::app
