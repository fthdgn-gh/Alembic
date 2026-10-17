// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PS5PKGTool PS5PKGTool.Ffpfsc/AmprIndex.cs (GPL-3.0).
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
// Not the standard FNV-1a offset basis (14695981039346656037): PS5PKGTool's
// constant is missing its last digit. Kept as-is so indexes match its output
// and whatever reads them on the console.
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

std::string replace_backslashes(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}

Result<std::string> normalize_relative_path(const std::string& path) {
    std::string normalized = replace_backslashes(path);
    normalized.erase(0, normalized.find_first_not_of('/') == std::string::npos ? normalized.size()
                                                                               : normalized.find_first_not_of('/'));
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

// Lower-cased UTF-16 key, as .NET's path.Replace('\\', '/').ToLowerInvariant().
Result<std::u16string> key(const std::string& path) {
    ALEMBIC_ASSIGN(std::u16string utf16, unicode::utf8_to_utf16(replace_backslashes(path)));
    return unicode::to_lower_invariant(utf16);
}

std::uint64_t hash_key(const std::u16string& key) {
    std::uint64_t hash = kFnvOffset;
    for (std::size_t i = 0; i < key.size(); ++i) {
        char32_t cp = key[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < key.size() && key[i + 1] >= 0xDC00 && key[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (key[i + 1] - 0xDC00);
            ++i;
        }
        hash ^= static_cast<std::uint32_t>(cp);
        hash *= kFnvPrime;
    }
    return hash == 0 ? 1 : hash;
}

struct Row {
    std::string path; // "/app0/<relative>"
    std::u16string key;
    std::uint64_t size;
    std::int64_t mtime;
};

} // namespace

Result<std::uint64_t> hash_path(const std::string& utf8_path) {
    ALEMBIC_ASSIGN(std::u16string k, key(utf8_path));
    return hash_key(k);
}

Result<std::vector<std::uint8_t>> build(const std::vector<FileRecord>& files) {
    std::vector<Row> rows;
    std::set<std::u16string> seen;
    for (const FileRecord& file : files) {
        ALEMBIC_ASSIGN(std::string relative, normalize_relative_path(file.relative_path));
        if (unicode::ascii_iequals(relative, "ampr_emu.index") || unicode::ascii_iequals(relative, "ampr_emu.index.tmp"))
            continue;
        std::string path = "/app0/" + relative;
        ALEMBIC_ASSIGN(std::u16string k, key(path));
        if (!seen.insert(k).second) continue;
        rows.push_back(Row{std::move(path), std::move(k), file.size, file.unix_modified_time});
    }
    // .NET StringComparer.Ordinal compares UTF-16 code units, as u16string does.
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
        std::uint64_t hash = hash_key(rows[i].key);
        std::uint64_t position = hash & mask;
        for (;;) {
            std::uint8_t* slot = p + hash_offset + position * kSlotSize;
            if (get_u32(slot + 8) == 0) {
                put_u64(slot, hash);
                put_u32(slot + 8, static_cast<std::uint32_t>(i + 1));
                break;
            }
            if (get_u64(slot) == hash) put_u32(slot + 12, 1);
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
