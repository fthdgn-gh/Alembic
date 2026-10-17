// SPDX-License-Identifier: GPL-3.0-or-later
// AMPRPAK4 / AMPRDAT3 / AMPRCRC1 / AMPRCFG1 encoding and validation, glob
// matching and pack configuration. Ported from drakmor/ampr_emu's
// tools/ampr_pack_format.py and the config half of tools/ampr_pack.py
// (GPL-3.0); automatic_profile() follows PS5-LZ4-Compression's
// src/ampr_pack_profile.c (GPL-3.0-or-later).
#include "alembic/lz4pack.hpp"

#include "lz4pack_internal.hpp"

#include "alembic/bytes.hpp"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <set>
#include <zlib.h>

namespace alembic::lz4pack {

namespace {

constexpr std::uint8_t kIndexMagic[8] = {'A', 'M', 'P', 'R', 'P', 'A', 'K', '4'};
constexpr std::uint8_t kDataMagic[8] = {'A', 'M', 'P', 'R', 'D', 'A', 'T', '3'};
constexpr std::uint8_t kCrcMagic[8] = {'A', 'M', 'P', 'R', 'C', 'R', 'C', '1'};
constexpr std::uint8_t kRuntimeMagic[8] = {'A', 'M', 'P', 'R', 'C', 'F', 'G', '1'};
constexpr std::uint32_t kIndexVersion = 4;
constexpr std::uint32_t kDataVersion = 3;
constexpr std::uint32_t kEndianMarker = 0x01020304;
constexpr std::uint32_t kIndexHeaderSize = 128;
constexpr std::uint32_t kCrcHeaderSize = 48;
constexpr std::uint32_t kFileRecordSize = 48;
constexpr std::uint32_t kChunkRecordSize = 12;
constexpr std::uint32_t kPackRecordSize = 32;
constexpr std::uint32_t kMinBlockShift = 14;
constexpr std::uint32_t kMaxBlockShift = 20;
constexpr std::uint32_t kPhysicalAlignment = 64;
constexpr std::uint32_t kFileKnownFlags = 0x1F;
constexpr std::uint32_t kChunkKnownFlags = 0x0F;
constexpr std::uint32_t kPackKnownFlags = 0x03;

} // namespace

namespace detail {
std::uint32_t crc(Bytes data, std::uint32_t seed) {
    std::uint32_t value = seed;
    std::size_t done = 0;
    while (done < data.size()) {
        auto n = static_cast<uInt>(std::min<std::size_t>(data.size() - done, 1u << 30));
        value = static_cast<std::uint32_t>(::crc32(value, data.data() + done, n));
        done += n;
    }
    return value;
}

} // namespace detail

namespace {
using detail::crc;
using detail::canonical_asset_path;
using detail::is_safe_relative;

bool is_power_of_two(std::uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }
std::uint64_t align_down(std::uint64_t v, std::uint64_t a) { return v & ~(a - 1); }

Error bad(const std::string& message) { return make_error(Errc::invalid_data, message); }

} // namespace

std::optional<std::string> detail::canonical_asset_path(std::string_view input) {
    std::string path(input);
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
    std::vector<std::string> parts;
    std::size_t at = 0;
    while (at <= path.size()) {
        std::size_t slash = path.find('/', at);
        if (slash == std::string::npos) slash = path.size();
        std::string component = path.substr(at, slash - at);
        at = slash + 1;
        if (component.empty() || component == ".") continue;
        if (component == "..") {
            if (parts.empty()) return std::nullopt;
            parts.pop_back();
            continue;
        }
        if (component.find('\0') != std::string::npos) return std::nullopt;
        parts.push_back(std::move(component));
    }
    std::string normalized;
    for (const std::string& p : parts) normalized += "/" + p;
    if (normalized.empty()) normalized = "/";
    std::string lower = normalized;
    for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (lower == "/app0") return std::string("/app0");
    if (lower.rfind("/app0/", 0) != 0) return std::nullopt;
    return normalized;
}

bool detail::is_safe_relative(std::string_view name) {
    if (name.empty() || name.find('\\') != std::string_view::npos || name.front() == '/' || name.back() == '/')
        return false;
    std::size_t at = 0;
    while (at <= name.size()) {
        std::size_t slash = name.find('/', at);
        if (slash == std::string_view::npos) slash = name.size();
        std::string_view c = name.substr(at, slash - at);
        if (c.empty() || c == "." || c == "..") return false;
        at = slash + 1;
    }
    return true;
}

namespace {

// ---- glob

std::u32string decode_utf8_lossy(std::string_view s) {
    std::u32string out;
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i]);
        int extra = b < 0x80 ? 0 : (b >> 5) == 6 ? 1 : (b >> 4) == 14 ? 2 : (b >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + extra >= s.size() + (extra == 0 ? 1 : 0)) {
            out.push_back(b);
            ++i;
            continue;
        }
        char32_t cp = extra == 0 ? b : (b & (0x3F >> extra));
        bool ok = true;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!ok) {
            out.push_back(b);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += static_cast<std::size_t>(extra) + 1;
    }
    return out;
}

std::string normalize_glob_path(std::string_view s) {
    std::string out(s);
    std::replace(out.begin(), out.end(), '\\', '/');
    std::size_t start = out.find_first_not_of('/');
    return start == std::string::npos ? std::string() : out.substr(start);
}

// One element of a translated pattern.
struct GlobItem {
    enum Kind { literal, any_one, any_run, set } kind = literal;
    char32_t ch = 0;
    bool negate = false;
    std::vector<std::pair<char32_t, char32_t>> ranges;

    static GlobItem make(Kind kind, char32_t ch = 0) {
        GlobItem item;
        item.kind = kind;
        item.ch = ch;
        return item;
    }
};

// fnmatch.translate(): '[' without a closing ']' is a literal; '!' negates;
// a ']' right after '[' or '[!' is part of the set; "a-z" ranges.
std::vector<GlobItem> translate(const std::u32string& pat) {
    std::vector<GlobItem> items;
    std::size_t i = 0, n = pat.size();
    while (i < n) {
        char32_t c = pat[i++];
        if (c == '*') {
            if (items.empty() || items.back().kind != GlobItem::any_run) items.push_back(GlobItem::make(GlobItem::any_run));
        } else if (c == '?') {
            items.push_back(GlobItem::make(GlobItem::any_one));
        } else if (c == '[') {
            std::size_t j = i;
            if (j < n && pat[j] == '!') ++j;
            if (j < n && pat[j] == ']') ++j;
            while (j < n && pat[j] != ']') ++j;
            if (j >= n) {
                items.push_back(GlobItem::make(GlobItem::literal, '['));
                continue;
            }
            GlobItem item = GlobItem::make(GlobItem::set);
            std::size_t k = i;
            if (pat[k] == '!') {
                item.negate = true;
                ++k;
            }
            // Ranges as Python 3.13 reads them: "x-y" pairs, a trailing or
            // leading '-' is literal, reversed ranges are dropped.
            std::vector<char32_t> chars(pat.begin() + static_cast<std::ptrdiff_t>(k),
                                        pat.begin() + static_cast<std::ptrdiff_t>(j));
            for (std::size_t m = 0; m < chars.size(); ++m) {
                if (m + 2 < chars.size() && chars[m + 1] == '-') {
                    if (chars[m] <= chars[m + 2]) item.ranges.push_back({chars[m], chars[m + 2]});
                    m += 2;
                } else {
                    item.ranges.push_back({chars[m], chars[m]});
                }
            }
            items.push_back(std::move(item));
            i = j + 1;
        } else {
            items.push_back(GlobItem::make(GlobItem::literal, c));
        }
    }
    return items;
}

bool item_matches(const GlobItem& item, char32_t c) {
    switch (item.kind) {
    case GlobItem::literal: return item.ch == c;
    case GlobItem::any_one: return true;
    case GlobItem::set: {
        bool in = false;
        for (auto [lo, hi] : item.ranges)
            if (c >= lo && c <= hi) in = true;
        // An empty set matches nothing; an empty negated set ("[!]") anything.
        return item.negate ? !in : in;
    }
    case GlobItem::any_run: return true;
    }
    return false;
}

bool match_items(const std::vector<GlobItem>& items, const std::u32string& s) {
    std::size_t p = 0, i = 0;
    std::size_t star_p = std::string::npos, star_i = 0;
    while (i < s.size()) {
        if (p < items.size() && items[p].kind == GlobItem::any_run) {
            star_p = p++;
            star_i = i;
        } else if (p < items.size() && item_matches(items[p], s[i])) {
            ++p;
            ++i;
        } else if (star_p != std::string::npos) {
            p = star_p + 1;
            i = ++star_i;
        } else {
            return false;
        }
    }
    while (p < items.size() && items[p].kind == GlobItem::any_run) ++p;
    return p == items.size();
}

// ---- JSON / TOML text

std::string format_double(double v) {
    // Python repr(): shortest round-trip digits, always with a '.' or exponent.
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + sizeof buf, v);
    std::string s(buf, ec == std::errc() ? end : buf);
    std::size_t e = s.find('e');
    if (e != std::string::npos) {
        // to_chars writes "1e-05"; Python too. Make the exponent two digits.
        std::string mant = s.substr(0, e), exp = s.substr(e + 1);
        char sign = '+';
        if (!exp.empty() && (exp[0] == '-' || exp[0] == '+')) {
            sign = exp[0];
            exp = exp.substr(1);
        }
        if (exp.size() < 2) exp.insert(exp.begin(), '0');
        return mant + "e" + sign + exp;
    }
    if (s.find('.') == std::string::npos) s += ".0";
    return s;
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    std::u32string cps = decode_utf8_lossy(s);
    char buf[16];
    for (char32_t c : cps) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20 || c > 0x7E) {
                if (c >= 0x10000) {
                    char32_t v = c - 0x10000;
                    std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
                } else {
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                }
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out + "\"";
}

// A JSON object whose keys are sorted when it is written (sort_keys=True).
struct JsonObject {
    std::vector<std::pair<std::string, std::string>> fields;
    void add(const std::string& k, std::string raw) { fields.emplace_back(k, std::move(raw)); }
    void str(const std::string& k, std::string_view v) { add(k, json_string(v)); }
    void num(const std::string& k, std::uint64_t v) { add(k, std::to_string(v)); }
    void real(const std::string& k, double v) { add(k, format_double(v)); }
    void boolean(const std::string& k, bool v) { add(k, v ? "true" : "false"); }
    void list(const std::string& k, const std::vector<std::string>& items) {
        std::string s = "[";
        for (std::size_t i = 0; i < items.size(); ++i) s += (i ? "," : "") + json_string(items[i]);
        add(k, s + "]");
    }
    std::string text() {
        std::sort(fields.begin(), fields.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::string s = "{";
        for (std::size_t i = 0; i < fields.size(); ++i)
            s += (i ? "," : "") + json_string(fields[i].first) + ":" + fields[i].second;
        return s + "}";
    }
};

const char* action_name(Action a) {
    switch (a) {
    case Action::loose: return "loose";
    case Action::compress: return "compress";
    case Action::store: return "store";
    }
    return "loose";
}
const char* layout_name(Layout l) {
    switch (l) {
    case Layout::automatic: return "auto";
    case Layout::random: return "random";
    case Layout::mixed: return "mixed";
    case Layout::streaming: return "streaming";
    }
    return "auto";
}
const char* assignment_name(Assignment a) {
    switch (a) {
    case Assignment::balanced: return "balanced";
    case Assignment::hash: return "hash";
    case Assignment::round_robin: return "round_robin";
    }
    return "balanced";
}

std::string toml_string(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out + "\"";
}
std::string toml_list(const std::vector<std::string>& items) {
    std::string s = "[";
    for (std::size_t i = 0; i < items.size(); ++i) s += (i ? ", " : "") + toml_string(items[i]);
    return s + "]";
}

} // namespace

// ================================================================ records

std::string Manifest::string_at(std::uint32_t offset, std::uint32_t length) const {
    if (static_cast<std::uint64_t>(offset) + length >= strings.size()) return {};
    return std::string(reinterpret_cast<const char*>(strings.data()) + offset, length);
}
std::string Manifest::file_path(std::size_t index) const {
    return string_at(files[index].path_offset, files[index].path_length);
}
std::string Manifest::pack_name(std::size_t pack_id) const {
    return string_at(packs[pack_id].name_offset, packs[pack_id].name_length);
}

std::uint64_t asset_path_hash(std::string_view path) {
    std::uint64_t h = 0xCBF29CE484222325ull;
    for (char ch : path) {
        auto b = static_cast<std::uint8_t>(ch == '\\' ? '/' : ch);
        if (b >= 'A' && b <= 'Z') b = static_cast<std::uint8_t>(b + 32);
        h ^= b;
        h *= 0x100000001B3ull;
    }
    return h ? h : 1;
}

namespace {

void put_file(std::uint8_t* p, const FileRecord& r) {
    put_u64(p, r.path_hash);
    put_u64(p + 8, r.logical_size);
    put_u64(p + 16, static_cast<std::uint64_t>(r.mtime));
    put_u32(p + 24, r.first_chunk);
    put_u32(p + 28, r.chunk_count);
    put_u32(p + 32, r.path_offset);
    put_u32(p + 36, r.path_length);
    put_u32(p + 40, r.flags);
    p[44] = r.block_shift;
    p[45] = r.packing_class;
    put_u16(p + 46, 0);
}
void put_chunk(std::uint8_t* p, const ChunkRecord& r) {
    put_u64(p, r.offset | (static_cast<std::uint64_t>(r.pack_id) << 48));
    put_u32(p + 8, (r.stored_size - 1) | (static_cast<std::uint32_t>(r.codec) << 20) |
                       (static_cast<std::uint32_t>(r.flags) << 22));
}
void put_pack(std::uint8_t* p, const PackRecord& r) {
    put_u64(p, r.payload_bytes);
    put_u64(p + 8, r.file_size);
    put_u32(p + 16, r.name_offset);
    put_u32(p + 20, r.name_length);
    put_u32(p + 24, r.flags);
    put_u32(p + 28, r.io_page_size);
}

} // namespace

std::vector<std::uint8_t> detail::encode_file_records(const std::vector<FileRecord>& files) {
    std::vector<std::uint8_t> out(files.size() * kFileRecordSize);
    for (std::size_t i = 0; i < files.size(); ++i) put_file(out.data() + i * kFileRecordSize, files[i]);
    return out;
}
std::vector<std::uint8_t> detail::encode_chunk_records(const std::vector<ChunkRecord>& chunks) {
    std::vector<std::uint8_t> out(chunks.size() * kChunkRecordSize);
    for (std::size_t i = 0; i < chunks.size(); ++i) put_chunk(out.data() + i * kChunkRecordSize, chunks[i]);
    return out;
}
std::vector<std::uint8_t> detail::encode_pack_records(const std::vector<PackRecord>& packs) {
    std::vector<std::uint8_t> out(packs.size() * kPackRecordSize);
    for (std::size_t i = 0; i < packs.size(); ++i) put_pack(out.data() + i * kPackRecordSize, packs[i]);
    return out;
}

std::vector<std::uint8_t> encode_manifest(const Manifest& m) {
    std::uint64_t files_offset = kIndexHeaderSize;
    std::uint64_t chunks_offset = files_offset + m.files.size() * kFileRecordSize;
    std::uint64_t packs_offset = chunks_offset + m.chunks.size() * kChunkRecordSize;
    std::uint64_t strings_offset = packs_offset + m.packs.size() * kPackRecordSize;
    std::vector<std::uint8_t> out(strings_offset + m.strings.size());
    for (std::size_t i = 0; i < m.files.size(); ++i) put_file(out.data() + files_offset + i * kFileRecordSize, m.files[i]);
    for (std::size_t i = 0; i < m.chunks.size(); ++i)
        put_chunk(out.data() + chunks_offset + i * kChunkRecordSize, m.chunks[i]);
    for (std::size_t i = 0; i < m.packs.size(); ++i) put_pack(out.data() + packs_offset + i * kPackRecordSize, m.packs[i]);
    if (!m.strings.empty()) std::memcpy(out.data() + strings_offset, m.strings.data(), m.strings.size());
    std::uint32_t payload_crc = crc(Bytes(out.data() + kIndexHeaderSize, out.size() - kIndexHeaderSize));

    std::uint8_t* h = out.data();
    std::memcpy(h, kIndexMagic, 8);
    put_u32(h + 8, kIndexVersion);
    put_u32(h + 12, kIndexHeaderSize);
    put_u32(h + 16, 0);
    put_u32(h + 20, kEndianMarker);
    std::memcpy(h + 24, m.build_id.data(), 16);
    put_u64(h + 40, m.files.size());
    put_u64(h + 48, m.chunks.size());
    put_u32(h + 56, static_cast<std::uint32_t>(m.packs.size()));
    put_u32(h + 60, kFileRecordSize);
    put_u32(h + 64, kChunkRecordSize);
    put_u32(h + 68, kPackRecordSize);
    put_u64(h + 72, files_offset);
    put_u64(h + 80, chunks_offset);
    put_u64(h + 88, packs_offset);
    put_u64(h + 96, strings_offset);
    put_u64(h + 104, m.strings.size());
    put_u32(h + 112, payload_crc);
    put_u32(h + 116, 0);
    put_u64(h + 120, 0);
    put_u32(h + 116, crc(Bytes(h, kIndexHeaderSize)));
    return out;
}

namespace {

Status validate_manifest(const Manifest& m) {
    for (std::size_t id = 0; id < m.packs.size(); ++id) {
        const PackRecord& r = m.packs[id];
        std::string idn = std::to_string(id);
        if (static_cast<std::uint64_t>(r.name_offset) + r.name_length >= m.strings.size() ||
            m.strings[r.name_offset + r.name_length] != 0)
            return bad("pack name " + idn + " is out of bounds");
        if (!is_safe_relative(m.pack_name(id))) return bad("unsafe pack name " + idn);
        if (r.flags & ~kPackKnownFlags) return bad("unknown flags for pack " + idn);
        if (r.io_page_size < 4096 || r.io_page_size > (1u << 20) || !is_power_of_two(r.io_page_size))
            return bad("invalid I/O page size for pack " + idn);
        if (!(r.flags & kPackIoPageLayout)) return bad("pack " + idn + " does not declare page-aware layout");
        if (r.file_size < kDataHeaderSize || r.payload_bytes > r.file_size) return bad("invalid size for pack " + idn);
        std::uint64_t payload_offset = r.file_size - r.payload_bytes;
        if (payload_offset < kDataHeaderSize || payload_offset % r.io_page_size || r.file_size % r.io_page_size)
            return bad("invalid payload offset for pack " + idn);
    }
    for (std::size_t i = 0; i < m.files.size(); ++i) {
        const FileRecord& r = m.files[i];
        std::string idn = std::to_string(i + 1);
        if (static_cast<std::uint64_t>(r.path_offset) + r.path_length >= m.strings.size() ||
            m.strings[r.path_offset + r.path_length] != 0)
            return bad("path of file id " + idn + " is out of bounds");
        std::string path = m.file_path(i);
        auto canonical = canonical_asset_path(path);
        if (!canonical || *canonical != path || *canonical == "/app0") return bad("non-canonical path for file id " + idn);
        if (r.flags & ~kFileKnownFlags) return bad("unknown flags for file id " + idn);
        if (asset_path_hash(path) != r.path_hash) return bad("path hash mismatch for file id " + idn);
        if (!(r.flags & kFilePacked)) {
            if (r.first_chunk || r.chunk_count || r.block_shift || r.packing_class || r.flags)
                return bad("loose file id " + idn + " references chunks");
            continue;
        }
        if (r.block_shift < kMinBlockShift || r.block_shift > kMaxBlockShift)
            return bad("invalid block shift for file id " + idn);
        if ((r.flags & kFileStreaming) && (r.flags & kFileRandomAccess))
            return bad("file id " + idn + " cannot be both streaming and random-access");
        if (static_cast<std::uint64_t>(r.first_chunk) + r.chunk_count > m.chunks.size())
            return bad("chunk range is invalid for file id " + idn);
        std::uint64_t total = 0, block = 1ull << r.block_shift;
        for (std::uint32_t local = 0; local < r.chunk_count; ++local) {
            const ChunkRecord& c = m.chunks[r.first_chunk + local];
            if (c.raw_size == 0 || c.raw_size > block) return bad("invalid raw chunk size for file id " + idn);
            if (c.stored_size == 0 || c.stored_size > block) return bad("invalid stored chunk size for file id " + idn);
            if (local + 1 != r.chunk_count && c.raw_size != block) return bad("short non-final chunk for file id " + idn);
            if (c.codec != kCodecRaw && c.codec != kCodecLz4) return bad("unknown chunk codec for file id " + idn);
            if (c.codec == kCodecRaw && c.stored_size != c.raw_size) return bad("raw chunk size mismatch for file id " + idn);
            if (c.pack_id >= m.packs.size()) return bad("invalid pack id for file id " + idn);
            if (c.flags & ~kChunkKnownFlags) return bad("invalid chunk flags for file id " + idn);
            bool streaming = r.flags & kFileStreaming;
            if (streaming != bool(c.flags & kChunkStreaming)) return bad("streaming flag mismatch for file id " + idn);
            if ((r.flags & kFileStoreOnly) && c.codec != kCodecRaw)
                return bad("store-only file id " + idn + " contains a compressed chunk");
            const PackRecord& p = m.packs[c.pack_id];
            std::uint64_t payload_offset = p.file_size - p.payload_bytes;
            if (c.offset < payload_offset || c.offset + c.stored_size > p.file_size || c.offset % kPhysicalAlignment)
                return bad("chunk range/alignment is invalid for file id " + idn);
            std::uint64_t page = p.io_page_size;
            std::uint64_t page_begin = align_down(c.offset, page);
            std::uint64_t page_end = align_up(c.offset + c.stored_size, page);
            bool contained = c.flags & kChunkPageContained, aligned = c.flags & kChunkPageAligned;
            if (contained && (c.stored_size > page || page_begin != align_down(c.offset + c.stored_size - 1, page)))
                return bad("page-contained chunk crosses an I/O page for file id " + idn);
            if (aligned && c.offset % page) return bad("page-aligned chunk is not page aligned for file id " + idn);
            if (c.stored_size <= page && aligned && !contained)
                return bad("small page-aligned chunk lacks containment flag for file id " + idn);
            bool safe = contained || aligned;
            if ((r.flags & kFileRandomAccess) && !safe)
                return bad("random-access file uses a non-page-safe chunk for file id " + idn);
            if (!(r.flags & kFileStreaming) && !safe)
                return bad("non-streaming file uses a non-page-safe chunk for file id " + idn);
            if (page_begin < payload_offset || page_end > p.file_size)
                return bad("aligned I/O range is outside the pack for file id " + idn);
            total += c.raw_size;
        }
        if (total != r.logical_size) return bad("logical size mismatch for file id " + idn);
    }
    return {};
}

} // namespace

Result<Manifest> parse_manifest(Bytes data) {
    if (data.size() < kIndexHeaderSize) return bad("pack index is truncated");
    const std::uint8_t* h = data.data();
    if (std::memcmp(h, kIndexMagic, 8) != 0 || get_u32(h + 8) != kIndexVersion)
        return bad("unsupported AMPR pack index");
    if (get_u32(h + 12) != kIndexHeaderSize || get_u32(h + 20) != kEndianMarker || get_u32(h + 16) != 0 ||
        get_u64(h + 120) != 0)
        return bad("invalid AMPR pack header");
    if (get_u32(h + 60) != kFileRecordSize || get_u32(h + 64) != kChunkRecordSize || get_u32(h + 68) != kPackRecordSize)
        return bad("unsupported AMPR pack record size");
    std::uint8_t copy[kIndexHeaderSize];
    std::memcpy(copy, h, kIndexHeaderSize);
    put_u32(copy + 116, 0);
    if (crc(Bytes(copy, kIndexHeaderSize)) != get_u32(h + 116)) return bad("pack index header CRC mismatch");
    if (crc(data.subspan(kIndexHeaderSize)) != get_u32(h + 112)) return bad("pack index payload CRC mismatch");
    std::uint64_t file_count = get_u64(h + 40), chunk_count = get_u64(h + 48), pack_count = get_u32(h + 56);
    if (file_count > data.size() || chunk_count > data.size() || pack_count > data.size())
        return bad("invalid pack index section layout");
    std::uint64_t files_offset = kIndexHeaderSize;
    std::uint64_t chunks_offset = files_offset + file_count * kFileRecordSize;
    std::uint64_t packs_offset = chunks_offset + chunk_count * kChunkRecordSize;
    std::uint64_t strings_offset = packs_offset + pack_count * kPackRecordSize;
    if (get_u64(h + 72) != files_offset || get_u64(h + 80) != chunks_offset || get_u64(h + 88) != packs_offset ||
        get_u64(h + 96) != strings_offset || strings_offset + get_u64(h + 104) != data.size())
        return bad("invalid pack index section layout");

    Manifest m;
    std::memcpy(m.build_id.data(), h + 24, 16);
    m.files.resize(file_count);
    for (std::uint64_t i = 0; i < file_count; ++i) {
        const std::uint8_t* p = h + files_offset + i * kFileRecordSize;
        FileRecord& r = m.files[i];
        r.path_hash = get_u64(p);
        r.logical_size = get_u64(p + 8);
        r.mtime = static_cast<std::int64_t>(get_u64(p + 16));
        r.first_chunk = get_u32(p + 24);
        r.chunk_count = get_u32(p + 28);
        r.path_offset = get_u32(p + 32);
        r.path_length = get_u32(p + 36);
        r.flags = get_u32(p + 40);
        r.block_shift = p[44];
        r.packing_class = p[45];
        if (get_u16(p + 46) != 0) return bad("unknown flags for file id " + std::to_string(i + 1));
    }
    m.chunks.resize(chunk_count);
    for (std::uint64_t i = 0; i < chunk_count; ++i) {
        const std::uint8_t* p = h + chunks_offset + i * kChunkRecordSize;
        std::uint64_t location = get_u64(p);
        std::uint32_t descriptor = get_u32(p + 8);
        if (descriptor >> 30) return bad("chunk descriptor contains reserved bits");
        ChunkRecord& c = m.chunks[i];
        c.offset = location & ((1ull << 48) - 1);
        c.pack_id = static_cast<std::uint16_t>(location >> 48);
        c.stored_size = (descriptor & 0xFFFFF) + 1;
        c.codec = static_cast<std::uint8_t>((descriptor >> 20) & 3);
        c.flags = static_cast<std::uint8_t>((descriptor >> 22) & 0xFF);
    }
    for (FileRecord& f : m.files) {
        if (!(f.flags & kFilePacked)) continue;
        if (f.block_shift < kMinBlockShift || f.block_shift > kMaxBlockShift) return bad("invalid block shift");
        std::uint64_t block = 1ull << f.block_shift;
        for (std::uint32_t local = 0; local < f.chunk_count; ++local) {
            std::uint64_t index = static_cast<std::uint64_t>(f.first_chunk) + local;
            if (index >= m.chunks.size()) return bad("chunk range is invalid while decoding AMPRPAK4");
            std::uint64_t begin = local * block;
            if (begin >= f.logical_size) return bad("AMPRPAK4 file has too many chunks");
            m.chunks[index].raw_size = static_cast<std::uint32_t>(std::min(block, f.logical_size - begin));
        }
    }
    m.packs.resize(pack_count);
    for (std::uint64_t i = 0; i < pack_count; ++i) {
        const std::uint8_t* p = h + packs_offset + i * kPackRecordSize;
        PackRecord& r = m.packs[i];
        r.payload_bytes = get_u64(p);
        r.file_size = get_u64(p + 8);
        r.name_offset = get_u32(p + 16);
        r.name_length = get_u32(p + 20);
        r.flags = get_u32(p + 24);
        r.io_page_size = get_u32(p + 28);
    }
    m.strings.assign(h + strings_offset, h + data.size());
    ALEMBIC_TRY(validate_manifest(m));
    return m;
}

std::array<std::uint8_t, kDataHeaderSize> encode_data_header(std::uint16_t pack_id, const BuildId& build_id,
                                                             std::uint64_t payload_offset, std::uint64_t payload_bytes,
                                                             std::uint32_t flags) {
    std::array<std::uint8_t, kDataHeaderSize> h{};
    std::memcpy(h.data(), kDataMagic, 8);
    put_u32(h.data() + 8, kDataVersion);
    put_u32(h.data() + 12, kDataHeaderSize);
    put_u32(h.data() + 16, pack_id);
    put_u32(h.data() + 20, flags);
    std::memcpy(h.data() + 24, build_id.data(), 16);
    put_u64(h.data() + 40, payload_offset);
    put_u64(h.data() + 48, payload_bytes);
    put_u32(h.data() + 56, crc(Bytes(h.data(), h.size())));
    return h;
}

Result<DataHeader> parse_data_header(Bytes data) {
    if (data.size() != kDataHeaderSize) return bad("pack data header is truncated");
    const std::uint8_t* h = data.data();
    if (std::memcmp(h, kDataMagic, 8) != 0 || get_u32(h + 8) != kDataVersion || get_u32(h + 12) != kDataHeaderSize)
        return bad("unsupported pack data header");
    if (get_u32(h + 60) != 0) return bad("invalid pack data header");
    DataHeader out;
    out.pack_id = get_u32(h + 16);
    out.flags = get_u32(h + 20);
    if (out.flags & ~kPackKnownFlags) return bad("unknown pack data flags");
    std::uint8_t copy[kDataHeaderSize];
    std::memcpy(copy, h, kDataHeaderSize);
    put_u32(copy + 56, 0);
    if (crc(Bytes(copy, kDataHeaderSize)) != get_u32(h + 56)) return bad("pack data header CRC mismatch");
    std::memcpy(out.build_id.data(), h + 24, 16);
    out.payload_offset = get_u64(h + 40);
    out.payload_bytes = get_u64(h + 48);
    return out;
}

std::vector<std::uint8_t> encode_chunk_crcs(const BuildId& build_id, const std::vector<std::uint32_t>& crcs) {
    std::vector<std::uint8_t> out(kCrcHeaderSize + crcs.size() * 4);
    for (std::size_t i = 0; i < crcs.size(); ++i) put_u32(out.data() + kCrcHeaderSize + i * 4, crcs[i]);
    std::uint8_t* h = out.data();
    std::memcpy(h, kCrcMagic, 8);
    put_u32(h + 8, 1);
    put_u32(h + 12, kCrcHeaderSize);
    std::memcpy(h + 16, build_id.data(), 16);
    put_u64(h + 32, crcs.size());
    put_u32(h + 40, crc(Bytes(out.data() + kCrcHeaderSize, out.size() - kCrcHeaderSize)));
    put_u32(h + 44, 0);
    put_u32(h + 44, crc(Bytes(h, kCrcHeaderSize)));
    return out;
}

Result<std::vector<std::uint32_t>> parse_chunk_crcs(Bytes data, const BuildId& build_id, std::uint64_t chunk_count) {
    if (data.size() < kCrcHeaderSize) return bad("chunk CRC sidecar is truncated");
    const std::uint8_t* h = data.data();
    if (std::memcmp(h, kCrcMagic, 8) != 0 || get_u32(h + 8) != 1 || get_u32(h + 12) != kCrcHeaderSize)
        return bad("unsupported chunk CRC sidecar");
    if (std::memcmp(h + 16, build_id.data(), 16) != 0 || get_u64(h + 32) != chunk_count)
        return bad("chunk CRC sidecar build ID or chunk count mismatch");
    std::uint8_t copy[kCrcHeaderSize];
    std::memcpy(copy, h, kCrcHeaderSize);
    put_u32(copy + 44, 0);
    if (crc(Bytes(copy, kCrcHeaderSize)) != get_u32(h + 44)) return bad("chunk CRC sidecar header CRC mismatch");
    Bytes payload = data.subspan(kCrcHeaderSize);
    if (payload.size() != chunk_count * 4 || crc(payload) != get_u32(h + 40))
        return bad("chunk CRC sidecar payload CRC mismatch");
    std::vector<std::uint32_t> out(chunk_count);
    for (std::uint64_t i = 0; i < chunk_count; ++i) out[i] = get_u32(payload.data() + i * 4);
    return out;
}

Status validate(const RuntimeSettings& s) {
    if (s.workers < 1 || s.workers > 16 || s.latency_reserve_workers >= s.workers)
        return make_error(Errc::invalid_argument, "runtime workers must be 1..16 and the reserve must be smaller");
    if (s.decoded_cache_bytes % 16384 || s.physical_cache_bytes % 16384)
        return make_error(Errc::invalid_argument, "runtime cache sizes must be 16 KiB multiples");
    return {};
}

std::array<std::uint8_t, 64> encode_runtime(const RuntimeSettings& s, const BuildId& build_id) {
    std::array<std::uint8_t, 64> d{};
    std::memcpy(d.data(), kRuntimeMagic, 8);
    put_u32(d.data() + 8, 1);
    put_u32(d.data() + 12, 64);
    std::memcpy(d.data() + 16, build_id.data(), 16);
    put_u64(d.data() + 32, s.decoded_cache_bytes);
    put_u64(d.data() + 40, s.physical_cache_bytes);
    put_u32(d.data() + 48, s.workers);
    put_u32(d.data() + 52, s.latency_reserve_workers);
    put_u32(d.data() + 56, crc(Bytes(d.data(), d.size())));
    return d;
}

Result<RuntimeSettings> parse_runtime(Bytes data, const BuildId& build_id) {
    if (data.size() != 64) return bad("invalid runtime profile size");
    std::uint8_t copy[64];
    std::memcpy(copy, data.data(), 64);
    put_u32(copy + 56, 0);
    if (std::memcmp(copy, kRuntimeMagic, 8) != 0 || get_u32(copy + 8) != 1 || get_u32(copy + 12) != 64 ||
        std::memcmp(copy + 16, build_id.data(), 16) != 0 || get_u32(copy + 60) != 0 ||
        crc(Bytes(copy, 64)) != get_u32(data.data() + 56))
        return bad("invalid runtime profile header, build ID or CRC");
    RuntimeSettings s;
    s.decoded_cache_bytes = get_u64(copy + 32);
    s.physical_cache_bytes = get_u64(copy + 40);
    s.workers = get_u32(copy + 48);
    s.latency_reserve_workers = get_u32(copy + 52);
    ALEMBIC_TRY(validate(s));
    return s;
}

// ================================================================ globs

bool glob_match(std::string_view pattern, std::string_view path) {
    return match_items(translate(decode_utf8_lossy(normalize_glob_path(pattern))),
                       decode_utf8_lossy(normalize_glob_path(path)));
}

bool glob_match_any(const std::vector<std::string>& patterns, std::string_view path) {
    for (const std::string& p : patterns)
        if (glob_match(p, path)) return true;
    return false;
}

// ================================================================ config

Layout Rule::resolved_layout() const {
    if (layout != Layout::automatic) return layout;
    if (streaming) return Layout::streaming;
    if (hot) return Layout::random;
    return Layout::mixed;
}

Status validate(Config& c) {
    auto fail = [](const std::string& m) { return make_error(Errc::invalid_argument, m); };
    if (c.io_page_size < 4096 || c.io_page_size > (1u << 20) || !is_power_of_two(c.io_page_size))
        return fail("io_page_size must be a power of two between 4 KiB and 1 MiB");
    if (c.payload_alignment < kDataHeaderSize || c.payload_alignment > (1u << 20) || !is_power_of_two(c.payload_alignment))
        return fail("payload_alignment must be a power of two between 64 B and 1 MiB");
    if (c.chunk_alignment < kPhysicalAlignment || !is_power_of_two(c.chunk_alignment) ||
        c.chunk_alignment > c.io_page_size)
        return fail("chunk_alignment must be a power of two between 64 B and io_page_size");
    if (c.default_block_shift < kMinBlockShift || c.default_block_shift > kMaxBlockShift)
        return fail("block sizes must be powers of two from 16 KiB to 1 MiB");
    if (c.level < 1 || c.level > 12) return fail("compression level must be between 1 and 12");
    if (c.acceleration < 1) return fail("acceleration must be at least 1");
    if (!(c.min_savings_ratio >= 0 && c.min_savings_ratio < 1) ||
        !(c.io_neutral_min_savings_ratio >= 0 && c.io_neutral_min_savings_ratio < 1))
        return fail("savings ratios must be in [0, 1)");
    if (c.auto_loose_sample_blocks < 1 || c.auto_loose_sample_blocks > 4096 || c.auto_loose_sample_bytes < 1)
        return fail("auto-loose sampling settings are out of range");
    if (!(c.auto_loose_min_savings_ratio >= 0 && c.auto_loose_min_savings_ratio < 1) ||
        !(c.auto_loose_max_raw_ratio >= 0 && c.auto_loose_max_raw_ratio <= 1))
        return fail("auto-loose ratios are out of range");
    if (!is_safe_relative(c.index_name)) return fail("unsafe index_name");
    if (c.runtime) ALEMBIC_TRY(validate(*c.runtime));
    if (std::none_of(c.groups.begin(), c.groups.end(), [](const Group& g) { return g.name == "default"; })) {
        Group g;
        g.name = "default";
        c.groups.push_back(g);
    }
    if (c.groups.size() > 256) return fail("at most 256 packing groups");
    std::set<std::string> names;
    for (const Group& g : c.groups) {
        if (!names.insert(g.name).second) return fail("duplicate group " + g.name);
        if (g.pack_count < 1 || g.pack_count > 0xFFFF) return fail("groups." + g.name + ".pack_count must be 1..65535");
        std::uint32_t page = g.io_page_size ? g.io_page_size : c.io_page_size;
        if (page < 4096 || page > (1u << 20) || !is_power_of_two(page) || page % c.chunk_alignment)
            return fail("groups." + g.name + ".io_page_size is invalid");
        std::uint64_t min_volume = align_up(kDataHeaderSize, std::max(c.payload_alignment, page)) + page;
        if (g.max_pack_size && g.max_pack_size < min_volume)
            return fail("groups." + g.name + ".max_pack_size is smaller than one I/O page beyond the payload offset");
        if (g.stripe_group_blocks < 1) return fail("groups." + g.name + ".stripe_group_blocks must be at least 1");
    }
    for (std::size_t i = 0; i < c.rules.size(); ++i) {
        const Rule& r = c.rules[i];
        std::string idn = "rule " + std::to_string(i);
        if (!names.count(r.group)) return fail(idn + " references unknown group " + r.group);
        if (r.include.empty()) return fail(idn + " include list cannot be empty");
        if (r.block_shift < kMinBlockShift || r.block_shift > kMaxBlockShift)
            return fail(idn + " block_size must be a power of two from 16 KiB to 1 MiB");
        if (r.level < 1 || r.level > 12) return fail(idn + " compression_level must be between 1 and 12");
        if (r.acceleration < 1) return fail(idn + " acceleration must be at least 1");
        if (!(r.min_savings_ratio >= 0 && r.min_savings_ratio < 1) ||
            !(r.io_neutral_min_savings_ratio >= 0 && r.io_neutral_min_savings_ratio < 1))
            return fail(idn + " savings ratios must be in [0, 1)");
    }
    return {};
}

namespace {

// PS5-LZ4-Compression's k_safety_exclusions.
const char* const kSafetyExclusions[] = {
    "eboot.bin", "**/eboot.bin", "*.elf", "**/*.elf", "*.self", "**/*.self", "*.prx", "**/*.prx", "*.sprx",
    "**/*.sprx", "*.bak", "**/*.bak", "*.dat", "**/*.dat", "*.utoc", "**/*.utoc", "decrypted/**", "sce_module/**",
    "**/sce_module/**", "sce_sys/**", "**/sce_sys/**", "system/**", "**/system/**", "mods/**", "**/mods/**",
    "save/**", "**/save/**", "fakelib/**", "**/fakelib/**", "_DUPLEX_/**", "trophy2/**", "**/trophy2/**", "uds/**",
    "**/uds/**", "ampr_emu.index", "**/ampr_emu.index", "ampr_assets.index", "**/ampr_assets.index",
    "ampr_assets.index.crc", "**/ampr_assets.index.crc", "ampr_assets.index.runtime",
    "**/ampr_assets.index.runtime", "ampr_assets-*.pak", "**/*.pak", "*.json", "**/*.json", "*.ini", "**/*.ini",
    "*.cfg", "**/*.cfg", "*.xml", "**/*.xml", "*.txt", "**/*.txt", "*.bk2", "**/*.bk2", "*.mp4", "**/*.mp4",
    "*.ivf", "**/*.ivf", "*.usm", "**/*.usm", "*.bnk", "**/*.bnk", "*.wem", "**/*.wem", "*.at9", "**/*.at9",
    "*.pfs", "**/*.pfs", "*.img", "**/*.img", "*.png", "**/*.png", "*.jpg", "**/*.jpg", "*.jpeg", "**/*.jpeg",
    "*.webp", "**/*.webp", "*.gif", "**/*.gif", "*.bmp", "**/*.bmp", "*.ico", "**/*.ico", "*.tga", "**/*.tga",
    "*.tif", "**/*.tif", "*.tiff", "**/*.tiff", "*.exr", "**/*.exr", "*.hdr", "**/*.hdr", "*.psd", "**/*.psd",
    "*.dds", "**/*.dds", "*.ktx", "**/*.ktx", "*.ktx2", "**/*.ktx2", "*.astc", "**/*.astc", "*.basis",
    "**/*.basis", "*.gnf", "**/*.gnf", "*.gnfp", "**/*.gnfp", "*.jxm", "**/*.jxm", "*.vtf", "**/*.vtf", "*.zip",
    "**/*.zip", "*.7z", "**/*.7z", "*.rar", "**/*.rar", "*.gz", "**/*.gz", "*.xz", "**/*.xz", "*.bz2", "**/*.bz2",
    "*.zst", "**/*.zst", "*.lz4", "**/*.lz4", "*.mp3", "**/*.mp3", "*.ogg", "**/*.ogg", "*.flac", "**/*.flac",
    "*.aac", "**/*.aac", "*.opus", "**/*.opus", "*.m4a", "**/*.m4a", "*.avi", "**/*.avi", "*.mkv", "**/*.mkv",
    "*.mov", "**/*.mov", "*.wmv", "**/*.wmv", "*.flv", "**/*.flv", "*.webm", "**/*.webm", "*.m4v", "**/*.m4v",
};

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

bool contains_hint(const std::string& name, std::initializer_list<const char*> hints) {
    std::string lower = ascii_lower(name.substr(0, 255));
    for (const char* h : hints)
        if (lower.find(h) != std::string::npos) return true;
    return false;
}

// Path.suffix.lower()
std::string suffix_lower(const std::string& rel) {
    std::size_t slash = rel.rfind('/');
    std::string base = slash == std::string::npos ? rel : rel.substr(slash + 1);
    std::size_t dot = base.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == base.size()) return {};
    return ascii_lower(base.substr(dot));
}

bool alpha_us(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool digit(char c) { return c >= '0' && c <= '9'; }

// ^[a-zA-Z_]+\.\d+$ or ^[a-zA-Z_]+\d+\.[a-zA-Z]+$ (data.0, chunk0.bin).
bool root_numbered_name(const std::string& n) {
    std::size_t i = 0, len = n.size();
    while (i < len && alpha_us(n[i])) ++i;
    if (i == 0) return false;
    if (i < len && n[i] == '.') {
        std::size_t j = i + 1;
        while (j < len && digit(n[j])) ++j;
        if (j > i + 1 && j == len) return true;
    }
    std::size_t j = i;
    while (j < len && digit(n[j])) ++j;
    if (j == i || j >= len || n[j] != '.') return false;
    std::size_t k = j + 1;
    while (k < len && ((n[k] >= 'a' && n[k] <= 'z') || (n[k] >= 'A' && n[k] <= 'Z'))) ++k;
    return k > j + 1 && k == len;
}

void walk(const tree::Node& node, const std::string& prefix, std::vector<std::string>& dirs,
          std::vector<std::string>& files) {
    for (const tree::Node& child : node.children) {
        std::string rel = prefix.empty() ? child.name : prefix + "/" + child.name;
        if (child.is_directory) {
            dirs.push_back(rel);
            walk(child, rel, dirs, files);
        } else {
            files.push_back(rel);
        }
    }
}

Rule small_hot_rule(std::uint32_t, int level, bool hc) {
    Rule r;
    r.action = Action::compress;
    r.include.clear();
    r.block_shift = 14;
    r.group = "assets";
    r.layout = Layout::random;
    r.hot = true;
    r.min_savings_bytes = 16;
    r.min_savings_ratio = 0.0025;
    r.io_neutral_min_savings_bytes = 2048;
    r.io_neutral_min_savings_ratio = 0.125;
    r.level = level;
    r.hc = hc;
    return r;
}

} // namespace

Config automatic_profile(const tree::Node& root, int level, std::uint32_t block_shift) {
    level = std::clamp(level, 1, 12);
    Config c;
    c.hc = level > 4;
    c.level = level;
    c.acceleration = 1;
    c.default_block_shift = block_shift;
    c.pack_pattern = "ampr_assets-{group}-lane{lane:02d}-vol{volume:02d}-{id:03d}.pak";
    c.runtime = RuntimeSettings{0, 0, 8, 1};
    c.runtime_auto_budget = true;

    std::vector<std::string> dirs, files;
    walk(root, "", dirs, files);
    bool has_audio = false, has_video = false, has_soundbank_ext = false;
    for (const std::string& rel : dirs) {
        std::string top = rel.substr(0, rel.find('/'));
        if (contains_hint(top, {"video", "movie", "movies", "cutscene", "cutscenes", "fmv", "cinematic", "cinematics"}))
            has_video = true;
        if (contains_hint(top, {"sound", "audio", "sfx", "music", "voice", "vo", "dialog", "dialogue", "soundbank", "wem"}))
            has_audio = true;
    }
    static const char* const texture_exts[] = {".dds", ".ktx", ".ktx2", ".astc", ".basis", ".tga", ".gnf", ".gnfp",
                                               ".jxm", ".vtf", ".dat", ".res", ".uasset", ".ubulk", ".bin"};
    std::set<std::string> textures_found;
    std::vector<std::string> numbered;
    for (const std::string& rel : files) {
        std::string ext = suffix_lower(rel);
        if (!ext.empty()) {
            if (ext.find("soundbank") != std::string::npos) has_soundbank_ext = true;
            for (const char* t : texture_exts)
                if (ext == t) textures_found.insert(t);
        }
        if (rel.find('/') == std::string::npos && root_numbered_name(rel)) numbered.push_back(rel);
    }

    Group assets;
    assets.name = "assets";
    assets.pack_count = 4;
    assets.max_pack_size = 16ull << 30;
    c.groups.push_back(assets);

    auto base_rule = [&](std::vector<std::string> include, std::uint32_t shift) {
        Rule r;
        r.action = Action::compress;
        r.include = std::move(include);
        r.block_shift = shift;
        r.group = "assets";
        r.layout = Layout::mixed;
        r.level = level;
        r.hc = c.hc;
        return r;
    };
    if (!dirs.empty()) c.rules.push_back(base_rule({"*/**"}, block_shift));
    if (!numbered.empty()) {
        std::sort(numbered.begin(), numbered.end());
        c.rules.push_back(base_rule(numbered, block_shift));
    }
    if (!textures_found.empty()) {
        std::vector<std::string> pats;
        for (const std::string& t : textures_found) pats.push_back("**/*" + t); // std::set: strcmp order
        c.rules.push_back(base_rule(pats, std::max<std::uint32_t>(block_shift, 16)));
    }
    if (has_audio) {
        Rule r = small_hot_rule(block_shift, level, c.hc);
        r.include = {"**/sound/**", "**/audio/**", "**/sfx/**", "**/music/**", "**/voice/**", "**/vo/**",
                     "**/dialog/**", "**/dialogue/**"};
        c.rules.push_back(r);
    }
    if (has_video) {
        Rule r;
        r.action = Action::loose;
        r.include = {"**/video/**", "**/movie/**", "**/movies/**", "**/cutscene/**", "**/cutscenes/**", "**/fmv/**",
                     "**/cinematic/**", "**/cinematics/**"};
        r.level = level;
        r.hc = c.hc;
        c.rules.push_back(r);
    }
    {
        Rule r;
        r.action = Action::loose;
        r.include = {"**/movie", "**/movie_*", "**/movies", "**/movies_*", "**/soundbank", "**/soundbank_*",
                     "**/screenreaderwem", "**/screenreaderwem.*", "**/wem", "**/wem_*", "**/wem.*"};
        r.level = level;
        r.hc = c.hc;
        c.rules.push_back(r);
    }
    if (has_soundbank_ext) {
        Rule r = small_hot_rule(block_shift, level, c.hc);
        r.include = {"**/soundbank.*"};
        c.rules.push_back(r);
    }
    Rule safety;
    safety.action = Action::loose;
    safety.include.assign(std::begin(kSafetyExclusions), std::end(kSafetyExclusions));
    safety.level = level;
    safety.hc = c.hc;
    c.rules.push_back(safety);
    c.force_loose.assign(std::begin(kSafetyExclusions), std::end(kSafetyExclusions));
    return c;
}

std::string canonical_config_json(const Config& c) {
    JsonObject o;
    o.str("tool_version", "4.0");
    o.str("index_name", c.index_name);
    o.str("pack_pattern", c.pack_pattern);
    o.str("default_action", action_name(c.default_action));
    o.num("default_block_shift", c.default_block_shift);
    o.num("io_page_size", c.io_page_size);
    o.num("payload_alignment", c.payload_alignment);
    o.num("chunk_alignment", c.chunk_alignment);
    o.str("compression_mode", c.hc ? "hc" : "fast");
    o.num("compression_level", static_cast<std::uint64_t>(c.level));
    o.num("acceleration", static_cast<std::uint64_t>(c.acceleration));
    o.num("min_savings_bytes", c.min_savings_bytes);
    o.real("min_savings_ratio", c.min_savings_ratio);
    o.num("io_neutral_min_savings_bytes", c.io_neutral_min_savings_bytes);
    o.real("io_neutral_min_savings_ratio", c.io_neutral_min_savings_ratio);
    o.boolean("deduplicate", c.deduplicate);
    o.str("deduplicate_scope", c.deduplicate_group_scope ? "group" : "lane");
    o.boolean("deduplicate_streaming", c.deduplicate_streaming);
    o.boolean("auto_loose_large_files", c.auto_loose_large_files);
    o.boolean("auto_loose_hot_files", c.auto_loose_hot_files);
    o.num("auto_loose_min_file_size", c.auto_loose_min_file_size);
    o.num("auto_loose_sample_blocks", c.auto_loose_sample_blocks);
    o.num("auto_loose_sample_bytes", c.auto_loose_sample_bytes);
    o.real("auto_loose_min_savings_ratio", c.auto_loose_min_savings_ratio);
    o.real("auto_loose_max_raw_ratio", c.auto_loose_max_raw_ratio);
    std::vector<const Group*> groups;
    for (const Group& g : c.groups) groups.push_back(&g);
    std::sort(groups.begin(), groups.end(), [](const Group* a, const Group* b) { return a->name < b->name; });
    std::string gl = "[";
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const Group& g = *groups[i];
        JsonObject j;
        j.str("name", g.name);
        j.num("pack_count", g.pack_count);
        j.str("assignment", assignment_name(g.assignment));
        j.num("max_pack_size", g.max_pack_size);
        j.boolean("stripe_large_files", g.stripe_large_files);
        j.num("stripe_threshold", g.stripe_threshold);
        j.num("stripe_group_blocks", g.stripe_group_blocks);
        j.num("io_page_size", g.io_page_size);
        gl += (i ? "," : "") + j.text();
    }
    o.add("groups", gl + "]");
    std::string rl = "[";
    for (std::size_t i = 0; i < c.rules.size(); ++i) {
        const Rule& r = c.rules[i];
        JsonObject j;
        j.str("action", action_name(r.action));
        j.list("include", r.include);
        j.list("exclude", r.exclude);
        j.num("block_shift", r.block_shift);
        j.str("group", r.group);
        j.str("mode", r.hc ? "hc" : "fast");
        j.num("level", static_cast<std::uint64_t>(r.level));
        j.num("acceleration", static_cast<std::uint64_t>(r.acceleration));
        j.num("min_savings_bytes", r.min_savings_bytes);
        j.real("min_savings_ratio", r.min_savings_ratio);
        j.num("io_neutral_min_savings_bytes", r.io_neutral_min_savings_bytes);
        j.real("io_neutral_min_savings_ratio", r.io_neutral_min_savings_ratio);
        j.str("layout", layout_name(r.layout));
        j.boolean("streaming", r.streaming);
        j.boolean("hot", r.hot);
        j.boolean("force_pack", r.force_pack);
        rl += (i ? "," : "") + j.text();
    }
    o.add("rules", rl + "]");
    return o.text();
}

std::string to_toml(const Config& c) {
    std::string t = "# Written by Alembic: the configuration of one LZ4 pack build, as ampr_pack.py reads it.\n";
    auto line = [&](const std::string& k, const std::string& v) { t += k + " = " + v + "\n"; };
    if (c.runtime) {
        t += "[runtime]\n";
        line("decoded_cache_bytes", std::to_string(c.runtime->decoded_cache_bytes));
        line("physical_cache_bytes", std::to_string(c.runtime->physical_cache_bytes));
        line("workers", std::to_string(c.runtime->workers));
        line("latency_reserve_workers", std::to_string(c.runtime->latency_reserve_workers));
        t += "\n";
    }
    t += "[pack]\n";
    line("index_name", toml_string(c.index_name));
    line("pack_pattern", toml_string(c.pack_pattern));
    line("default_action", toml_string(action_name(c.default_action)));
    line("default_block_size", std::to_string(1u << c.default_block_shift));
    line("io_page_size", std::to_string(c.io_page_size));
    line("payload_alignment", std::to_string(c.payload_alignment));
    line("chunk_alignment", std::to_string(c.chunk_alignment));
    if (c.workers) line("workers", std::to_string(c.workers));
    line("compression_mode", toml_string(c.hc ? "hc" : "fast"));
    line("compression_level", std::to_string(c.level));
    line("acceleration", std::to_string(c.acceleration));
    line("min_savings_bytes", std::to_string(c.min_savings_bytes));
    line("min_savings_ratio", format_double(c.min_savings_ratio));
    line("io_neutral_min_savings_bytes", std::to_string(c.io_neutral_min_savings_bytes));
    line("io_neutral_min_savings_ratio", format_double(c.io_neutral_min_savings_ratio));
    line("deduplicate", c.deduplicate ? "true" : "false");
    line("deduplicate_scope", toml_string(c.deduplicate_group_scope ? "group" : "lane"));
    line("deduplicate_streaming", c.deduplicate_streaming ? "true" : "false");
    line("preserve_mtime", c.preserve_mtime ? "true" : "false");
    line("validate_index_metadata", c.validate_index_metadata ? "true" : "false");
    line("self_contained", c.self_contained ? "true" : "false");
    line("auto_loose_large_files", c.auto_loose_large_files ? "true" : "false");
    line("auto_loose_hot_files", c.auto_loose_hot_files ? "true" : "false");
    line("auto_loose_min_file_size", std::to_string(c.auto_loose_min_file_size));
    line("auto_loose_sample_blocks", std::to_string(c.auto_loose_sample_blocks));
    line("auto_loose_sample_bytes", std::to_string(c.auto_loose_sample_bytes));
    line("auto_loose_min_savings_ratio", format_double(c.auto_loose_min_savings_ratio));
    line("auto_loose_max_raw_ratio", format_double(c.auto_loose_max_raw_ratio));
    for (const Group& g : c.groups) {
        if (g.name == "default") continue; // ampr_pack.py adds it with these defaults
        t += "\n[groups." + toml_string(g.name) + "]\n";
        line("pack_count", std::to_string(g.pack_count));
        line("assignment", toml_string(assignment_name(g.assignment)));
        line("max_pack_size", std::to_string(g.max_pack_size));
        line("stripe_large_files", g.stripe_large_files ? "true" : "false");
        line("stripe_threshold", std::to_string(g.stripe_threshold));
        line("stripe_group_blocks", std::to_string(g.stripe_group_blocks));
        line("io_page_size", std::to_string(g.io_page_size));
    }
    for (const Rule& r : c.rules) {
        t += "\n[[rule]]\n";
        line("action", toml_string(action_name(r.action)));
        line("include", toml_list(r.include));
        line("exclude", toml_list(r.exclude));
        line("block_size", std::to_string(1u << r.block_shift));
        line("group", toml_string(r.group));
        line("compression_mode", toml_string(r.hc ? "hc" : "fast"));
        line("compression_level", std::to_string(r.level));
        line("acceleration", std::to_string(r.acceleration));
        line("min_savings_bytes", std::to_string(r.min_savings_bytes));
        line("min_savings_ratio", format_double(r.min_savings_ratio));
        line("io_neutral_min_savings_bytes", std::to_string(r.io_neutral_min_savings_bytes));
        line("io_neutral_min_savings_ratio", format_double(r.io_neutral_min_savings_ratio));
        line("layout", toml_string(layout_name(r.layout)));
        line("streaming", r.streaming ? "true" : "false");
        line("hot", r.hot ? "true" : "false");
        line("force_pack", r.force_pack ? "true" : "false");
    }
    return t;
}

} // namespace alembic::lz4pack
