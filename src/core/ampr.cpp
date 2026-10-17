// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PS5PKGTool PS5PKGTool.Ffpfsc/AmprIndex.cs (GPL-3.0). The
// ampr_emu scheme follows drakmor/ampr_emu (GPL-3.0): the reader in
// src/ampr_emu_index.cpp and the reference builder tools/build_ampr_index.py.
#include "alembic/ampr.hpp"

#include "alembic/bytes.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <set>

namespace alembic::ampr {

namespace {

constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kRecordSize = 24;
constexpr std::size_t kSlotSize = 16;
// Not the standard FNV-1a offset basis (14695981039346656037): a digit is
// missing. ampr_emu's reader uses this value, so the index must too.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
// Limits enforced by the ampr_emu reader (SCE_KERNEL_PATH_MAX / NAME_MAX).
constexpr std::size_t kMaxPathBytes = 1023;
constexpr std::size_t kMaxNameBytes = 255;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

std::string replace_backslashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

Result<std::string> normalize_relative_path(const std::string& path) {
    std::string normalized = replace_backslashes(path);
    std::size_t first = normalized.find_first_not_of('/');
    normalized.erase(0, first == std::string::npos ? normalized.size() : first);
    bool bad = normalized.empty();
    std::size_t begin = 0;
    while (!bad && begin <= normalized.size()) {
        std::size_t end = normalized.find('/', begin);
        if (end == std::string::npos) end = normalized.size();
        std::string_view part(normalized.data() + begin, end - begin);
        if (part.empty() || part == "." || part == "..") bad = true;
        begin = end + 1;
    }
    if (bad) return invalid("the AMPR relative path is invalid: " + path);
    return normalized;
}

// The folded path. For ampr_emu it is the UTF-8 bytes with only A-Z lowered
// (compared as unsigned bytes, which std::string does). For ps5pkgtool it is
// the .NET ToLowerInvariant() UTF-16 string, compared by code unit.
struct Key {
    std::string bytes;
    std::u16string utf16;
    bool operator<(const Key& other) const { return bytes < other.bytes || (bytes == other.bytes && utf16 < other.utf16); }
};

Result<Key> make_key(const std::string& path, Scheme scheme) {
    Key key;
    std::string slashed = replace_backslashes(path);
    if (scheme == Scheme::ampr_emu) {
        key.bytes = slashed;
        for (char& c : key.bytes)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    } else {
        ALEMBIC_ASSIGN(std::u16string utf16, unicode::utf8_to_utf16(slashed));
        key.utf16 = unicode::to_lower_invariant(utf16);
    }
    return key;
}

std::uint64_t hash_key(const Key& key, Scheme scheme) {
    std::uint64_t hash = kFnvOffset;
    if (scheme == Scheme::ampr_emu) {
        for (char c : key.bytes) {
            hash ^= static_cast<unsigned char>(c);
            hash *= kFnvPrime;
        }
    } else {
        const std::u16string& s = key.utf16;
        for (std::size_t i = 0; i < s.size(); ++i) {
            char32_t cp = s[i];
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
                ++i;
            }
            hash ^= static_cast<std::uint32_t>(cp);
            hash *= kFnvPrime;
        }
    }
    return hash == 0 ? 1 : hash;
}

// Files the ampr_emu reference builder never indexes (root level only).
bool ampr_emu_excluded(const std::string& relative) {
    for (const char* name : {"ampr_emu.index.bak", "ampr_commands.bin", "apr_emu.log"})
        if (unicode::ascii_iequals(relative, name)) return true;
    return false;
}

Status check_ampr_emu_limits(const std::string& path) {
    if (path.find_first_of("\t\n\r") != std::string::npos)
        return invalid("ampr_emu cannot index a path containing tabs or line breaks: " + path);
    if (path.size() > kMaxPathBytes) return invalid("the path is too long for ampr_emu (1023 bytes max): " + path);
    std::size_t begin = 0;
    while (begin <= path.size()) {
        std::size_t end = path.find('/', begin);
        if (end == std::string::npos) end = path.size();
        if (end - begin > kMaxNameBytes) return invalid("a name is too long for ampr_emu (255 bytes max): " + path);
        begin = end + 1;
    }
    return {};
}

struct Row {
    std::string path; // "/app0/<relative>"
    Key key;
    std::uint64_t size;
    std::int64_t mtime;
};

} // namespace

Result<std::uint64_t> hash_path(const std::string& utf8_path, Scheme scheme) {
    ALEMBIC_ASSIGN(Key key, make_key(utf8_path, scheme));
    return hash_key(key, scheme);
}

Result<std::vector<std::uint8_t>> build(const std::vector<FileRecord>& files, Scheme scheme) {
    std::vector<Row> rows;
    std::set<Key> seen;
    for (const FileRecord& file : files) {
        ALEMBIC_ASSIGN(std::string relative, normalize_relative_path(file.relative_path));
        if (unicode::ascii_iequals(relative, "ampr_emu.index") || unicode::ascii_iequals(relative, "ampr_emu.index.tmp"))
            continue;
        if (scheme == Scheme::ampr_emu && ampr_emu_excluded(relative)) continue;
        std::string path = "/app0/" + relative;
        if (scheme == Scheme::ampr_emu) ALEMBIC_TRY(check_ampr_emu_limits(path));
        ALEMBIC_ASSIGN(Key key, make_key(path, scheme));
        if (!seen.insert(key).second) continue;
        rows.push_back(Row{std::move(path), std::move(key), file.size, file.unix_modified_time});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.key < b.key; });
    if (rows.empty()) return std::vector<std::uint8_t>{};

    std::uint64_t path_blob_length = 0;
    for (const Row& row : rows) {
        if (path_blob_length > UINT32_MAX) return make_error(Errc::too_large, "the AMPR path table exceeds 32 bits");
        path_blob_length += row.path.size() + 1;
    }

    std::uint64_t slot_count = 2;
    while (slot_count < rows.size() * 2) slot_count *= 2;
    std::uint64_t path_end = kHeaderSize + rows.size() * kRecordSize + path_blob_length;
    std::uint64_t hash_offset = align_up(path_end, kSlotSize);
    std::uint64_t total = hash_offset + slot_count * kSlotSize;
    if (total > INT32_MAX) return make_error(Errc::too_large, "the AMPR index is too large");

    std::vector<std::uint8_t> out(static_cast<std::size_t>(total), 0);
    std::uint8_t* p = out.data();
    std::memcpy(p, "AMPRIDX3", 8);
    put_u32(p + 8, 3);
    put_u32(p + 12, kRecordSize);
    put_u64(p + 16, rows.size());
    put_u64(p + 24, path_blob_length);
    put_u64(p + 32, hash_offset);
    put_u32(p + 40, kSlotSize);
    put_u32(p + 44, static_cast<std::uint32_t>(slot_count));

    std::size_t path_base = kHeaderSize + rows.size() * kRecordSize;
    std::uint32_t path_offset = 0;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::uint8_t* record = p + kHeaderSize + i * kRecordSize;
        put_u32(record + 0, path_offset);
        put_u32(record + 4, static_cast<std::uint32_t>(rows[i].path.size()));
        put_u64(record + 8, rows[i].size);
        put_u64(record + 16, static_cast<std::uint64_t>(rows[i].mtime));
        std::memcpy(p + path_base + path_offset, rows[i].path.data(), rows[i].path.size());
        path_offset += static_cast<std::uint32_t>(rows[i].path.size() + 1);
    }

    std::uint64_t mask = slot_count - 1;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        std::uint64_t hash = hash_key(rows[i].key, scheme);
        std::uint64_t position = hash & mask;
        bool duplicate = false;
        for (;;) {
            std::uint8_t* slot = p + hash_offset + position * kSlotSize;
            if (get_u32(slot + 8) == 0) {
                put_u64(slot, hash);
                put_u32(slot + 8, static_cast<std::uint32_t>(i + 1));
                // ampr_emu flags every slot of a duplicate-hash group;
                // PS5PKGTool flags only the earlier ones.
                if (duplicate && scheme == Scheme::ampr_emu) put_u32(slot + 12, 1);
                break;
            }
            if (get_u64(slot) == hash) {
                put_u32(slot + 12, 1);
                duplicate = true;
            }
            position = (position + 1) & mask;
        }
    }
    ALEMBIC_ASSIGN(IndexInfo checked, inspect(out));
    (void)checked;
    return out;
}

Result<IndexInfo> inspect(const std::vector<std::uint8_t>& data) {
    if (data.size() < kHeaderSize || std::memcmp(data.data(), "AMPRIDX3", 8) != 0)
        return invalid("AMPRIDX3 magic is missing");
    const std::uint8_t* p = data.data();
    std::uint32_t version = get_u32(p + 8);
    std::uint32_t record_size = get_u32(p + 12);
    std::uint64_t rows = get_u64(p + 16);
    std::uint64_t path_length = get_u64(p + 24);
    std::uint64_t hash_offset = get_u64(p + 32);
    std::uint32_t slot_size = get_u32(p + 40);
    std::uint32_t slots = get_u32(p + 44);
    if (version != 3 || record_size != kRecordSize || slot_size != kSlotSize || rows == 0 || slots == 0 ||
        (slots & (slots - 1)) != 0 || rows > INT32_MAX || slots > INT32_MAX)
        return invalid("the AMPRIDX3 header is invalid");
    if (path_length > data.size() || rows > data.size()) return invalid("the AMPRIDX3 ranges are invalid");
    std::uint64_t records_end = kHeaderSize + rows * record_size;
    std::uint64_t path_end = records_end + path_length;
    std::uint64_t expected_end = hash_offset + static_cast<std::uint64_t>(slots) * slot_size;
    if (hash_offset < path_end || hash_offset % slot_size != 0 || expected_end != data.size())
        return invalid("the AMPRIDX3 ranges are invalid");
    for (std::uint64_t i = 0; i < rows; ++i) {
        const std::uint8_t* record = p + kHeaderSize + i * kRecordSize;
        std::uint64_t offset = get_u32(record);
        std::uint64_t length = get_u32(record + 4);
        if (offset + length >= path_length || p[records_end + offset + length] != 0)
            return invalid("an AMPRIDX3 path record is invalid");
    }
    return IndexInfo{static_cast<std::uint32_t>(rows), slots, path_length};
}

} // namespace alembic::ampr
