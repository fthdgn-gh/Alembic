// SPDX-License-Identifier: GPL-3.0-or-later
#include "name_formats.hpp"

#include "json_value.hpp"
#include "text_file.hpp"

#include "alembic/io.hpp"

#include <string_view>

namespace alembic::engine {

namespace {

// ---- the pattern, parsed ----

struct Node {
    enum Type { text, field, group } type = text;
    std::string value;              // text, or the field's name
    std::vector<std::string> mods;  // a field's modifiers
    std::vector<Node> kids;         // a group's parts
};

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}
std::string upper_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
    return s;
}
bool all_digits(const std::string& s) {
    if (s.empty() || s.size() > 3) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// A field's name as written ("Title_ID") -> its name ("titleid"), or "".
std::string field_name(const std::string& written) {
    std::string n;
    for (char c : lower_ascii(written))
        if (c != '_' && c != '-') n += c;
    if (n == "id") return "titleid";
    if (n == "ver") return "version";
    if (n == "kind") return "format";
    for (const char* known : {"title", "titleid", "contentid", "version", "format", "platform", "name"})
        if (n == known) return n;
    return "";
}
bool known_mod(const std::string& m) {
    for (const char* known : {"upper", "lower", "nospace", "under", "dash", "dot", "short"})
        if (m == known) return true;
    return all_digits(m) && m != "0" && m != "00" && m != "000";
}

// "title:upper:20" -> a field node; the error says what is wrong.
Result<Node> parse_field(const std::string& spec) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        std::size_t colon = spec.find(':', start);
        parts.push_back(spec.substr(start, colon == std::string::npos ? std::string::npos : colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    Node n;
    n.type = Node::field;
    n.value = field_name(parts[0]);
    if (n.value.empty())
        return make_error(Errc::invalid_argument, "unknown field " + parts[0] +
                                                      ": use title, titleid, contentid, version, format, platform or name");
    for (std::size_t i = 1; i < parts.size(); ++i) {
        std::string m = lower_ascii(parts[i]);
        if (!known_mod(m))
            return make_error(Errc::invalid_argument, "unknown modifier " + parts[i] +
                                                          ": use upper, lower, nospace, under, dash, dot, short or a number");
        n.mods.push_back(m);
    }
    return n;
}

// [spec]: a field only when spec is one (letters, digits, '_', '-', ':').
std::optional<Node> bracket_field(const std::string& spec) {
    if (spec.empty()) return std::nullopt;
    for (char c : spec)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ':'))
            return std::nullopt;
    auto n = parse_field(spec);
    if (!n.ok()) return std::nullopt;
    return *n;
}

Status parse(const std::string& p, std::size_t& i, bool in_group, std::vector<Node>& out) {
    auto text = [&](char c) {
        if (out.empty() || out.back().type != Node::text) out.push_back(Node{});
        out.back().value += c;
    };
    while (i < p.size()) {
        const char c = p[i];
        if (c == '{') {
            if (i + 1 < p.size() && p[i + 1] == '{') {
                text('{');
                i += 2;
                continue;
            }
            const std::size_t close = p.find('}', i + 1);
            if (close == std::string::npos) return make_error(Errc::invalid_argument, "a brace in the format is not closed");
            ALEMBIC_ASSIGN(Node n, parse_field(p.substr(i + 1, close - i - 1)));
            out.push_back(std::move(n));
            i = close + 1;
        } else if (c == '}') {
            if (i + 1 < p.size() && p[i + 1] == '}') {
                text('}');
                i += 2;
                continue;
            }
            return make_error(Errc::invalid_argument, "a closing brace in the format has no opening one");
        } else if (c == '[') {
            const std::size_t close = p.find(']', i + 1);
            std::optional<Node> n = close == std::string::npos ? std::nullopt : bracket_field(p.substr(i + 1, close - i - 1));
            if (n) {
                out.push_back(std::move(*n));
                i = close + 1;
            } else {
                text('[');
                ++i;
            }
        } else if (c == '<') {
            Node g;
            g.type = Node::group;
            ++i;
            ALEMBIC_TRY(parse(p, i, true, g.kids));
            if (i >= p.size() || p[i] != '>') return make_error(Errc::invalid_argument, "a < in the format is not closed");
            ++i;
            out.push_back(std::move(g));
        } else if (c == '>') {
            if (in_group) return {};
            return make_error(Errc::invalid_argument, "a > in the format has no <");
        } else {
            text(c);
            ++i;
        }
    }
    return {};
}

Result<std::vector<Node>> parse_pattern(const std::string& pattern) {
    std::vector<Node> nodes;
    std::size_t i = 0;
    ALEMBIC_TRY(parse(pattern, i, false, nodes));
    return nodes;
}

bool has_field(const std::vector<Node>& nodes) {
    for (const Node& n : nodes)
        if (n.type == Node::field || (n.type == Node::group && has_field(n.kids))) return true;
    return false;
}

// ---- rendering ----

// 01.004.000 -> 1.4, 01.000.003 -> 1.0.3
std::string short_version(const std::string& v) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        std::size_t dot = v.find('.', start);
        std::string part = v.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!part.empty() && part.find_first_not_of("0123456789") == std::string::npos) {
            std::size_t nz = part.find_first_not_of('0');
            part = nz == std::string::npos ? "0" : part.substr(nz);
        }
        parts.push_back(part);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    while (parts.size() > 2 && parts.back() == "0") parts.pop_back();
    if (parts.size() == 2 && parts.back() == "0") parts.pop_back();
    std::string out;
    for (std::size_t k = 0; k < parts.size(); ++k) out += (k ? "." : "") + parts[k];
    return out;
}

// The first `n` characters (UTF-8), without spaces at the end.
std::string first_chars(const std::string& s, std::size_t n) {
    std::size_t i = 0, count = 0;
    while (i < s.size() && count < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        ++count;
    }
    std::string out = s.substr(0, std::min(i, s.size()));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string replace_spaces(const std::string& s, const std::string& with) {
    std::string out;
    for (char c : s) {
        if (c == ' ') out += with;
        else out += c;
    }
    return out;
}

std::string field_value(const Node& n, const NameValues& v) {
    const std::string& raw = n.value == "title"       ? v.title
                             : n.value == "titleid"   ? v.title_id
                             : n.value == "contentid" ? v.content_id
                             : n.value == "version"   ? v.version
                             : n.value == "format"    ? v.format
                             : n.value == "platform"  ? v.platform
                                                      : v.name;
    std::string s = safe_name(raw);
    for (const std::string& m : n.mods) {
        if (m == "upper") s = upper_ascii(s);
        else if (m == "lower") s = lower_ascii(s);
        else if (m == "nospace") s = replace_spaces(s, "");
        else if (m == "under") s = replace_spaces(s, "_");
        else if (m == "dash") s = replace_spaces(s, "-");
        else if (m == "dot") s = replace_spaces(s, ".");
        else if (m == "short") s = short_version(s);
        else s = first_chars(s, static_cast<std::size_t>(std::atoi(m.c_str())));
    }
    return s;
}

std::string render(const std::vector<Node>& nodes, const NameValues& v, bool* missing) {
    std::string out;
    for (const Node& n : nodes) {
        if (n.type == Node::text) {
            out += n.value;
        } else if (n.type == Node::field) {
            std::string s = field_value(n, v);
            if (s.empty()) *missing = true;
            out += s;
        } else {
            bool group_missing = false;
            std::string s = render(n.kids, v, &group_missing);
            if (!group_missing) out += s;
        }
    }
    return out;
}

bool valid_spaces(const std::string& s) { return s == "keep" || s == "none" || s == "_" || s == "." || s == "-"; }
bool valid_letters(const std::string& s) { return s == "keep" || s == "upper" || s == "lower"; }

} // namespace

std::string safe_name(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        bool space = c < 0x20 || c == 0x7f || c == ' ';
        // U+00A0 and U+3000 are spaces too.
        if (c == 0xC2 && i + 1 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0xA0) {
            space = true;
            i += 1;
        } else if (c == 0xE3 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(s[i + 2]) == 0x80) {
            space = true;
            i += 2;
        }
        if (space) {
            if (!out.empty() && out.back() != ' ') out += ' ';
        } else if (c == ':') {
            while (!out.empty() && out.back() == ' ') out.pop_back();
            out += " - ";
        } else if (std::string_view("\\/*?\"<>|").find(static_cast<char>(c)) == std::string_view::npos) {
            out += static_cast<char>(c);
        }
    }
    std::size_t a = out.find_first_not_of(" ."), b = out.find_last_not_of(" .");
    return a == std::string::npos ? "" : out.substr(a, b - a + 1);
}

NameValues name_values(const std::string& title, const std::string& title_id, const std::string& content_id,
                       const std::string& version, const std::string& platform, const std::string& entry_name,
                       const std::string& kind) {
    NameValues v{title, title_id, content_id, version, "", upper_ascii(platform), ""};
    if (kind == "fpkg") v.format = "PKG";
    else if (kind != "folder" && kind != "file") v.format = upper_ascii(kind);
    std::string plain = entry_name.size() > 1 && entry_name[0] == '.' ? entry_name.substr(1) : entry_name;
    const std::size_t dot = plain.rfind('.');
    if (kind != "folder" && dot != std::string::npos && dot > 0) plain = plain.substr(0, dot);
    v.name = plain;
    return v;
}

Status check_name_format(const NameFormat& f) {
    if (f.pattern.empty()) return make_error(Errc::invalid_argument, "a format is empty");
    if (f.pattern.size() > NameFormats::kMaxPattern)
        return make_error(Errc::invalid_argument, "a format has at most 200 characters");
    ALEMBIC_ASSIGN(auto nodes, parse_pattern(f.pattern));
    if (!has_field(nodes)) return make_error(Errc::invalid_argument, "a format needs at least one field, such as title");
    if (!valid_spaces(f.spaces)) return make_error(Errc::invalid_argument, "spaces must be keep, none, _, . or -");
    if (!valid_letters(f.letters)) return make_error(Errc::invalid_argument, "letters must be keep, upper or lower");
    return {};
}

std::optional<std::string> render_name(const NameFormat& f, const NameValues& v) {
    if (!check_name_format(f).ok()) return std::nullopt;
    auto nodes = parse_pattern(f.pattern);
    bool missing = false;
    std::string s = safe_name(render(*nodes, v, &missing));
    if (missing || s.empty()) return std::nullopt;
    if (f.letters == "upper") s = upper_ascii(s);
    else if (f.letters == "lower") s = lower_ascii(s);
    if (f.spaces != "keep") s = replace_spaces(s, f.spaces == "none" ? "" : f.spaces);
    if (s.size() > 240) return std::nullopt; // file systems take 255 bytes, with the extension
    return s;
}

namespace {
// A new name with the old one's leading "." and extension.
struct NameParts {
    std::string hidden, ext;
};
NameParts parts_of(const std::string& entry_name, bool folder) {
    NameParts p;
    p.hidden = entry_name.size() > 1 && entry_name[0] == '.' ? "." : "";
    const std::string plain = entry_name.substr(p.hidden.size());
    const std::size_t dot = plain.rfind('.');
    if (!folder && dot != std::string::npos && dot > 0) p.ext = plain.substr(dot);
    return p;
}
std::size_t char_count(const std::string& s) {
    std::size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}
// Without spaces, dots, dashes or commas at the end (left by a cut).
std::string trim_cut(std::string s) {
    while (!s.empty() && std::string_view(" .-,_").find(s.back()) != std::string_view::npos) s.pop_back();
    return s;
}
} // namespace

std::optional<std::string> suggested_name(const NameFormat& f, const NameValues& v, const std::string& entry_name,
                                          bool folder) {
    auto stem = render_name(f, v);
    if (!stem) return std::nullopt;
    const NameParts p = parts_of(entry_name, folder);
    return p.hidden + *stem + p.ext;
}

std::optional<std::string> fitted_name(const NameFormat& f, const NameValues& v, const std::string& entry_name,
                                       bool folder, std::size_t limit) {
    const NameParts p = parts_of(entry_name, folder);
    auto stem = render_name(f, v);
    if (!stem || !limit || stem->size() <= limit) return std::nullopt;
    const std::size_t room = limit;
    // The title, shorter and shorter: whole words first, then any character.
    NameValues cut = v;
    const std::string title = safe_name(v.title);
    // Which characters are spaces (where whole words end).
    std::vector<bool> space;
    for (std::size_t i = 0; i < title.size(); ++i)
        if ((static_cast<unsigned char>(title[i]) & 0xC0) != 0x80) space.push_back(title[i] == ' ');
    const std::size_t total = space.size();
    for (bool words : {true, false}) {
        for (std::size_t n = total; n-- > 1;) {
            if (words && !space[n]) continue; // the cut is not before a space
            cut.title = trim_cut(first_chars(title, n));
            if (cut.title.empty()) break;
            auto s = render_name(f, cut);
            if (s && s->size() <= room) return p.hidden + *s + p.ext;
        }
    }
    // No title to cut (or not enough): the end of the name.
    std::size_t keep = room;
    while (keep > 0 && (static_cast<unsigned char>((*stem)[keep]) & 0xC0) == 0x80) --keep;
    std::string s = trim_cut(stem->substr(0, keep));
    if (s.empty()) return std::nullopt;
    return p.hidden + s + p.ext;
}

std::vector<NameFormat> default_name_formats() {
    return {
        {"{title} [{titleid}]"},
        {"{title} [{titleid}] [v{version}]"},
        {"{title} [{titleid}] [{format}]"},
        {"{titleid} - {title}"},
        {"{titleid}"},
    };
}

NameFormats::NameFormats(std::string state_dir) : file_(std::move(state_dir) + "/name_formats.json") {
    formats_ = default_name_formats();
    auto text = read_text(file_, 1u << 20);
    if (!text.ok()) return;
    auto v = parse_json(*text);
    if (!v.ok() || !v->has("formats")) return;
    std::vector<NameFormat> list;
    for (const JsonValue& item : (*v)["formats"].items()) {
        NameFormat f{item["pattern"].str(), item["spaces"].str("keep"), item["letters"].str("keep")};
        if (check_name_format(f).ok() && list.size() < kMax) list.push_back(std::move(f));
    }
    formats_ = std::move(list);
}

std::vector<NameFormat> NameFormats::list() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return formats_;
}

Status NameFormats::set(std::vector<NameFormat> formats) {
    if (formats.size() > kMax) return make_error(Errc::too_large, "at most 32 formats");
    for (const NameFormat& f : formats) ALEMBIC_TRY(check_name_format(f));
    std::lock_guard<std::mutex> lock(mutex_);
    formats_ = std::move(formats);
    return save_locked();
}

Status NameFormats::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    formats_ = default_name_formats();
    if (Status s = remove_file(file_); !s.ok() && path_exists(file_)) return s;
    return {};
}

Status NameFormats::save_locked() const {
    JsonValue list = JsonValue::array();
    for (const NameFormat& f : formats_) {
        JsonValue v = JsonValue::object();
        v.set("pattern", f.pattern);
        v.set("spaces", f.spaces);
        v.set("letters", f.letters);
        list.push(std::move(v));
    }
    JsonValue root = JsonValue::object();
    root.set("formats", std::move(list));
    (void)make_parent_directories(file_);
    return write_text(file_, root.dump());
}

} // namespace alembic::engine
