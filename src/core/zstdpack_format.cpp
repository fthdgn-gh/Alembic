// SPDX-License-Identifier: GPL-3.0-or-later
// The zstd pack format (AMPRZPK1 / AMPRZDT1), its chunk codec and the rules
// for which files are packed. Ported from quer3q/ps5-dump-forge's
// ps5-dump-forge-zstd (lib.rs, index.rs, volume.rs, writer.rs's
// encode_chunk; GPL-3.0) and ps5-dump-forge-lz4's rules.rs (always_loose,
// select_everything); the layout is ampr-emu-ng's docs/zstd-format.md.
#include "zstdpack_internal.hpp"

#include "alembic/bytes.hpp"
#include "alembic/unicode.hpp"

#include "zstd.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <set>

namespace alembic::zstdpack {

namespace {

constexpr std::uint8_t kIndexMagic[8] = {'A', 'M', 'P', 'R', 'Z', 'P', 'K', '1'};
constexpr std::uint8_t kVolumeMagic[8] = {'A', 'M', 'P', 'R', 'Z', 'D', 'T', '1'};
constexpr std::size_t kFileRecordSize = 32;
constexpr std::size_t kChunkRecordSize = 8;
constexpr std::size_t kVolumeRecordSize = 8;
constexpr std::size_t kPayloadCrcAt = 112;
constexpr std::size_t kHeaderCrcAt = 116;
constexpr std::size_t kVolumeCrcAt = 60;

Error invalid(const std::string& what) { return make_error(Errc::invalid_data, "invalid zstd pack " + what); }

// CRC-32 of the first `len` bytes with [at, at + n) taken as zeros.
std::uint32_t crc_zeroed(const std::uint8_t* bytes, std::size_t len, std::size_t at, std::size_t n) {
    static const std::uint8_t zeros[8] = {};
    std::uint32_t c = detail::crc(Bytes(bytes, at));
    c = detail::crc(Bytes(zeros, n), c);
    return detail::crc(Bytes(bytes + at + n, len - at - n), c);
}

bool all_zero(const std::uint8_t* p, std::size_t n) {
    return std::all_of(p, p + n, [](std::uint8_t b) { return b == 0; });
}

bool is_zstd_error(std::size_t code) { return ZSTD_isError(code) != 0; }

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

bool key_less(const std::string& a, const std::string& b) {
    std::uint64_t ha = path_hash(a), hb = path_hash(b);
    if (ha != hb) return ha < hb;
    return a < b; // char_traits<char> compares like memcmp: unsigned bytes
}

} // namespace detail

// ---------------------------------------------------------------- basics

std::uint64_t path_hash(std::string_view path) {
    std::uint64_t h = 14695981039346656037ull;
    for (char c : path) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

bool valid_path(std::string_view path) {
    if (path.empty() || path.front() == '/') return false;
    if (path.find('\\') != std::string_view::npos || path.find('\0') != std::string_view::npos) return false;
    std::size_t at = 0;
    for (;;) {
        std::size_t slash = path.find('/', at);
        std::string_view part = path.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
        if (part.empty() || part == "." || part == "..") return false;
        if (slash == std::string_view::npos) return true;
        at = slash + 1;
    }
}

std::string volume_name(std::uint32_t id) {
    char digits[16];
    std::snprintf(digits, sizeof digits, "%03u", id);
    return std::string(kVolumePrefix) + digits + kVolumeSuffix;
}

Result<std::uint64_t> pack_chunk(const Chunk& c) {
    if (c.volume >= kMaxVolumes || c.stored_size == 0 || c.stored_size > kChunkSize || c.offset < kVolumeHeaderSize ||
        static_cast<std::uint64_t>(c.offset) + c.stored_size > kVolumeLimit || c.codec > Codec::zstd)
        return invalid("chunk descriptor");
    return static_cast<std::uint64_t>(c.offset) | (static_cast<std::uint64_t>(c.volume) << 32) |
           (static_cast<std::uint64_t>(c.stored_size - 1) << 44) | (static_cast<std::uint64_t>(c.codec) << 62);
}

Result<Chunk> unpack_chunk(std::uint64_t word) {
    if ((word >> 62) > 1) return invalid("chunk codec");
    Chunk c;
    c.offset = static_cast<std::uint32_t>(word);
    c.volume = static_cast<std::uint16_t>((word >> 32) & 0xFFF);
    c.stored_size = static_cast<std::uint32_t>(((word >> 44) & 0x3FFFF) + 1);
    c.codec = (word >> 62) ? Codec::zstd : Codec::raw;
    auto checked = pack_chunk(c);
    if (!checked.ok()) return checked.take_error();
    return c;
}

// ---------------------------------------------------------------- index

Status validate(const Index& index, std::uint64_t volume_limit) {
    if (index.level < kMinLevel || index.level > kMaxLevel || index.volume_sizes.size() > kMaxVolumes ||
        index.chunks.size() > 0xFFFFFFFFull)
        return invalid("index limits");
    std::uint64_t next = 0;
    const FileRecord* previous = nullptr;
    for (const FileRecord& f : index.files) {
        if (!valid_path(f.path) || f.path.size() > 0xFFFF || f.first_chunk != next) return invalid("file record");
        if (previous && !detail::key_less(previous->path, f.path)) return invalid("file order");
        previous = &f;
        std::uint64_t count = chunk_count(f.size);
        if (count > index.chunks.size() || next > index.chunks.size() - count) return invalid("file chunk range");
        for (std::uint64_t i = 0; i < count; ++i) {
            const Chunk& c = index.chunks[next + i];
            auto packed = pack_chunk(c);
            if (!packed.ok()) return packed.take_error();
            std::uint64_t logical = chunk_raw_size(f.size, i);
            if ((c.codec == Codec::raw && c.stored_size != logical) || (c.codec == Codec::zstd && c.stored_size >= logical) ||
                (f.store_only && c.codec != Codec::raw))
                return invalid("chunk size or file flags");
        }
        next += count;
    }
    if (next != index.chunks.size()) return invalid("unused chunks");
    // No file is also a folder of another (`a` beside `a/b`).
    std::set<std::string_view> paths;
    for (const FileRecord& f : index.files) paths.insert(f.path);
    for (const FileRecord& f : index.files)
        for (std::size_t at = f.path.find('/'); at != std::string::npos; at = f.path.find('/', at + 1))
            if (paths.count(std::string_view(f.path).substr(0, at))) return invalid("file is also a folder");
    if (index.chunks.empty()) {
        if (!index.volume_sizes.empty()) return invalid("unused volumes");
        return {};
    }
    std::size_t volume = 0;
    std::uint64_t cursor = kVolumeHeaderSize;
    for (const Chunk& c : index.chunks) {
        if (c.volume != volume) {
            if (c.volume != volume + 1 || cursor == kVolumeHeaderSize || volume >= index.volume_sizes.size() ||
                index.volume_sizes[volume] != cursor)
                return invalid("volume order or size");
            volume = c.volume;
            cursor = kVolumeHeaderSize;
        }
        if (c.offset != cursor) return invalid("chunk offset");
        cursor += c.stored_size;
        if (cursor > volume_limit) return invalid("volume limit");
    }
    if (volume + 1 != index.volume_sizes.size() || index.volume_sizes[volume] != cursor) return invalid("final volume size");
    return {};
}

Result<std::vector<std::uint8_t>> encode_index(const Index& index, std::uint64_t volume_limit) {
    ALEMBIC_TRY(validate(index, volume_limit));
    const std::uint64_t files_off = kIndexHeaderSize;
    const std::uint64_t chunks_off = files_off + index.files.size() * kFileRecordSize;
    const std::uint64_t volumes_off = chunks_off + index.chunks.size() * kChunkRecordSize;
    const std::uint64_t strings_off = volumes_off + index.volume_sizes.size() * kVolumeRecordSize;
    std::uint64_t strings_len = 0, total = 0;
    for (const FileRecord& f : index.files) strings_len += f.path.size(), total += f.size;
    if (strings_len > 0xFFFFFFFFull) return invalid("strings length");
    std::vector<std::uint8_t> out(static_cast<std::size_t>(strings_off + strings_len));
    std::uint8_t* h = out.data();
    std::memcpy(h, kIndexMagic, 8);
    put_u32(h + 8, kFormatVersion);
    put_u32(h + 12, kIndexHeaderSize);
    h[16] = static_cast<std::uint8_t>(kChunkShift);
    h[17] = index.level;
    put_u32(h + 20, static_cast<std::uint32_t>(index.volume_sizes.size()));
    std::memcpy(h + 24, index.archive_id.data(), 16);
    put_u64(h + 40, index.files.size());
    put_u64(h + 48, index.chunks.size());
    put_u64(h + 56, files_off);
    put_u64(h + 64, chunks_off);
    put_u64(h + 72, volumes_off);
    put_u64(h + 80, strings_off);
    put_u64(h + 88, strings_len);
    put_u64(h + 96, total);
    std::uint64_t path_off = 0;
    for (std::size_t i = 0; i < index.files.size(); ++i) {
        const FileRecord& f = index.files[i];
        std::uint8_t* r = h + files_off + i * kFileRecordSize;
        put_u64(r, path_hash(f.path));
        put_u64(r + 8, f.size);
        put_u32(r + 16, f.first_chunk);
        put_u32(r + 20, static_cast<std::uint32_t>(path_off));
        put_u16(r + 24, static_cast<std::uint16_t>(f.path.size()));
        put_u16(r + 26, f.store_only ? 1 : 0);
        if (!f.path.empty()) std::memcpy(h + strings_off + path_off, f.path.data(), f.path.size());
        path_off += f.path.size();
    }
    for (std::size_t i = 0; i < index.chunks.size(); ++i) {
        ALEMBIC_ASSIGN(std::uint64_t word, pack_chunk(index.chunks[i]));
        put_u64(h + chunks_off + i * kChunkRecordSize, word);
    }
    for (std::size_t i = 0; i < index.volume_sizes.size(); ++i)
        put_u64(h + volumes_off + i * kVolumeRecordSize, index.volume_sizes[i]);
    put_u32(h + kPayloadCrcAt, detail::crc(Bytes(h + kIndexHeaderSize, out.size() - kIndexHeaderSize)));
    put_u32(h + kHeaderCrcAt, crc_zeroed(h, kIndexHeaderSize, kPayloadCrcAt, 8));
    return out;
}

Result<Index> parse_index(Bytes data, std::uint64_t volume_limit) {
    const std::uint8_t* h = data.data();
    if (data.size() < kIndexHeaderSize || std::memcmp(h, kIndexMagic, 8) != 0 || get_u32(h + 8) != kFormatVersion ||
        get_u32(h + 12) != kIndexHeaderSize || h[16] != kChunkShift || !all_zero(h + 18, 2) || !all_zero(h + 104, 8) ||
        !all_zero(h + 120, 8) ||
        get_u32(h + kPayloadCrcAt) != detail::crc(Bytes(h + kIndexHeaderSize, data.size() - kIndexHeaderSize)) ||
        get_u32(h + kHeaderCrcAt) != crc_zeroed(h, kIndexHeaderSize, kPayloadCrcAt, 8))
        return invalid("index header or CRC");
    const std::uint64_t files = get_u64(h + 40), chunks = get_u64(h + 48);
    const std::uint32_t volumes = get_u32(h + 20);
    if (chunks > 0xFFFFFFFFull || volumes > kMaxVolumes || files > data.size() / kFileRecordSize)
        return invalid("index counts");
    const std::uint64_t files_off = kIndexHeaderSize;
    const std::uint64_t chunks_off = files_off + files * kFileRecordSize;
    const std::uint64_t volumes_off = chunks_off + chunks * kChunkRecordSize;
    const std::uint64_t strings_off = volumes_off + std::uint64_t{volumes} * kVolumeRecordSize;
    const std::uint64_t strings_len = get_u64(h + 88);
    if (get_u64(h + 56) != files_off || get_u64(h + 64) != chunks_off || get_u64(h + 72) != volumes_off ||
        get_u64(h + 80) != strings_off || strings_len > data.size() || strings_off + strings_len != data.size())
        return invalid("index table offsets");
    Index index;
    std::memcpy(index.archive_id.data(), h + 24, 16);
    index.level = h[17];
    index.files.reserve(static_cast<std::size_t>(files));
    std::uint64_t next_path = 0, total = 0;
    for (std::uint64_t i = 0; i < files; ++i) {
        const std::uint8_t* r = h + files_off + i * kFileRecordSize;
        std::uint32_t path_off = get_u32(r + 20);
        std::uint16_t path_len = get_u16(r + 24);
        std::uint16_t flags = get_u16(r + 26);
        if ((flags & ~1u) != 0 || get_u32(r + 28) != 0) return invalid("file flags");
        if (path_off != next_path) return invalid("string order");
        if (std::uint64_t{path_off} + path_len > strings_len) return invalid("path range");
        FileRecord f;
        f.path.assign(reinterpret_cast<const char*>(h + strings_off + path_off), path_len);
        if (!unicode::utf8_to_utf16(f.path).ok()) return invalid("path UTF-8");
        if (get_u64(r) != path_hash(f.path)) return invalid("path hash");
        f.size = get_u64(r + 8);
        f.first_chunk = get_u32(r + 16);
        f.store_only = flags == 1;
        if (total + f.size < total) return invalid("logical size");
        total += f.size;
        next_path = std::uint64_t{path_off} + path_len;
        index.files.push_back(std::move(f));
    }
    if (next_path != strings_len) return invalid("unused strings");
    index.chunks.reserve(static_cast<std::size_t>(chunks));
    for (std::uint64_t i = 0; i < chunks; ++i) {
        ALEMBIC_ASSIGN(Chunk c, unpack_chunk(get_u64(h + chunks_off + i * kChunkRecordSize)));
        index.chunks.push_back(c);
    }
    for (std::uint32_t i = 0; i < volumes; ++i) index.volume_sizes.push_back(get_u64(h + volumes_off + i * kVolumeRecordSize));
    if (total != get_u64(h + 96)) return invalid("logical total");
    ALEMBIC_TRY(validate(index, volume_limit));
    return index;
}

// ---------------------------------------------------------------- volume header

namespace {
Status validate_header(const VolumeHeader& v) {
    if (v.count == 0 || v.count > kMaxVolumes || v.id >= v.count || v.payload_bytes == 0 ||
        v.payload_bytes > kVolumeLimit - kVolumeHeaderSize)
        return invalid("volume header values");
    return {};
}
} // namespace

Result<std::array<std::uint8_t, kVolumeHeaderSize>> encode_volume_header(const VolumeHeader& v) {
    ALEMBIC_TRY(validate_header(v));
    std::array<std::uint8_t, kVolumeHeaderSize> out{};
    std::memcpy(out.data(), kVolumeMagic, 8);
    put_u32(out.data() + 8, kFormatVersion);
    put_u32(out.data() + 12, kVolumeHeaderSize);
    put_u32(out.data() + 16, v.id);
    put_u32(out.data() + 20, v.count);
    std::memcpy(out.data() + 24, v.archive_id.data(), 16);
    put_u64(out.data() + 40, v.payload_bytes);
    put_u32(out.data() + kVolumeCrcAt, crc_zeroed(out.data(), kVolumeHeaderSize, kVolumeCrcAt, 4));
    return out;
}

Result<VolumeHeader> parse_volume_header(Bytes data) {
    const std::uint8_t* h = data.data();
    if (data.size() < kVolumeHeaderSize || std::memcmp(h, kVolumeMagic, 8) != 0 || get_u32(h + 8) != kFormatVersion ||
        get_u32(h + 12) != kVolumeHeaderSize || !all_zero(h + 48, 12) ||
        get_u32(h + kVolumeCrcAt) != crc_zeroed(h, kVolumeHeaderSize, kVolumeCrcAt, 4))
        return invalid("volume header");
    VolumeHeader v;
    v.id = get_u32(h + 16);
    v.count = get_u32(h + 20);
    std::memcpy(v.archive_id.data(), h + 24, 16);
    v.payload_bytes = get_u64(h + 40);
    ALEMBIC_TRY(validate_header(v));
    return v;
}

// ---------------------------------------------------------------- codec

Encoder::~Encoder() {
    if (context_) ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(context_));
}

Result<bool> Encoder::compress(Bytes raw, int level, std::vector<std::uint8_t>& frame) {
    auto zstd_error = [](std::size_t code) {
        return make_error(Errc::invalid_data, std::string("zstd: ") + ZSTD_getErrorName(code));
    };
    if (raw.empty() || raw.size() > kChunkSize) return make_error(Errc::invalid_argument, "zstd chunk size");
    // A context per level (ps5-dump-forge's thread-local one is made anew
    // when the level changes), kept from chunk to chunk.
    if (context_ && level_ != level) {
        ZSTD_freeCCtx(static_cast<ZSTD_CCtx*>(context_));
        context_ = nullptr;
    }
    if (!context_) {
        ZSTD_CCtx* c = ZSTD_createCCtx();
        if (!c) return make_error(Errc::io, "zstd: no memory for a context");
        for (auto [param, value] : {std::pair{ZSTD_c_compressionLevel, level},
                                    std::pair{ZSTD_c_windowLog, static_cast<int>(kChunkShift)},
                                    std::pair{ZSTD_c_checksumFlag, 1}, std::pair{ZSTD_c_contentSizeFlag, 1}}) {
            std::size_t r = ZSTD_CCtx_setParameter(c, param, value);
            if (is_zstd_error(r)) {
                ZSTD_freeCCtx(c);
                return zstd_error(r);
            }
        }
        context_ = c;
        level_ = level;
    }
    ZSTD_CCtx* c = static_cast<ZSTD_CCtx*>(context_);
    std::size_t r = ZSTD_CCtx_reset(c, ZSTD_reset_session_only);
    if (is_zstd_error(r)) return zstd_error(r);
    r = ZSTD_CCtx_setPledgedSrcSize(c, raw.size());
    if (is_zstd_error(r)) return zstd_error(r);
    // The output holds as much as the chunk: a frame that fills it cannot be
    // kept. Fed with "continue", closed with "end", as ps5-dump-forge does
    // (a full chunk then ends in an empty last block).
    frame.resize(raw.size());
    ZSTD_outBuffer out{frame.data(), frame.size(), 0};
    ZSTD_inBuffer in{raw.data(), raw.size(), 0};
    ZSTD_EndDirective end = ZSTD_e_continue;
    for (;;) {
        std::size_t left = ZSTD_compressStream2(c, &out, &in, end);
        if (is_zstd_error(left)) return zstd_error(left);
        if (end == ZSTD_e_end && left == 0) {
            frame.resize(out.pos);
            return out.pos < raw.size();
        }
        if (out.pos == out.size) return false;
        if (in.pos == raw.size()) end = ZSTD_e_end;
    }
}

Decoder::~Decoder() {
    if (context_) ZSTD_freeDCtx(static_cast<ZSTD_DCtx*>(context_));
}

Status Decoder::decode(Codec codec, Bytes stored, MutableBytes raw) {
    if (raw.empty() || raw.size() > kChunkSize || stored.empty() || stored.size() > kChunkSize)
        return make_error(Errc::invalid_data, "zstd pack chunk size");
    if (codec == Codec::raw) {
        if (stored.size() != raw.size()) return make_error(Errc::invalid_data, "a raw zstd pack chunk has the wrong size");
        std::memcpy(raw.data(), stored.data(), raw.size());
        return {};
    }
    if (codec != Codec::zstd) return make_error(Errc::invalid_data, "unknown zstd pack chunk codec");
    if (stored.size() >= raw.size()) return make_error(Errc::invalid_data, "a zstd chunk is not smaller than its data");
    // ampr-emu-ng's decode_zstd(): the magic, a checksum, a content size, no
    // dictionary, one frame filling the chunk.
    const std::uint8_t magic[4] = {0x28, 0xB5, 0x2F, 0xFD};
    if (stored.size() < 6 || std::memcmp(stored.data(), magic, 4) != 0)
        return make_error(Errc::invalid_data, "not a zstd frame");
    std::uint8_t d = stored[4];
    if (!(d & 0x04) || (d & 0x03) || (d & 0x08) || (!(d & 0xC0) && !(d & 0x20)))
        return make_error(Errc::invalid_data, "a zstd frame without a checksum or content size, or with a dictionary");
    if (ZSTD_getFrameContentSize(stored.data(), stored.size()) != raw.size() ||
        ZSTD_findFrameCompressedSize(stored.data(), stored.size()) != stored.size())
        return make_error(Errc::invalid_data, "a zstd frame of the wrong size");
    if (!context_) {
        ZSTD_DCtx* c = ZSTD_createDCtx();
        if (!c) return make_error(Errc::io, "zstd: no memory for a context");
        std::size_t r = ZSTD_DCtx_setParameter(c, ZSTD_d_windowLogMax, static_cast<int>(kChunkShift));
        if (is_zstd_error(r)) {
            ZSTD_freeDCtx(c);
            return make_error(Errc::invalid_data, std::string("zstd: ") + ZSTD_getErrorName(r));
        }
        context_ = c;
    }
    std::size_t n = ZSTD_decompressDCtx(static_cast<ZSTD_DCtx*>(context_), raw.data(), raw.size(), stored.data(), stored.size());
    if (is_zstd_error(n)) return make_error(Errc::invalid_data, std::string("zstd chunk does not decode: ") + ZSTD_getErrorName(n));
    if (n != raw.size()) return make_error(Errc::invalid_data, "a zstd chunk decodes to the wrong size");
    return {};
}

// ---------------------------------------------------------------- rules

namespace {

bool ends_with_icase(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && unicode::ascii_iequals(s.substr(s.size() - suffix.size()), suffix);
}
bool any_suffix(std::string_view s, std::initializer_list<std::string_view> suffixes) {
    for (std::string_view x : suffixes)
        if (ends_with_icase(s, x)) return true;
    return false;
}

} // namespace

bool always_loose(std::string_view path) {
    std::size_t last = path.rfind('/');
    std::string_view file = last == std::string_view::npos ? path : path.substr(last + 1);
    std::string_view first = path.substr(0, path.find('/'));
    const bool in_dir = last != std::string_view::npos;
    // ps5-dump-forge-lz4's always_loose (with its artifact list), ASCII
    // case-insensitive ...
    if (unicode::ascii_iequals(file, "eboot.bin") || any_suffix(path, {".elf", ".self", ".prx", ".sprx"})) return true;
    if (in_dir && (unicode::ascii_iequals(first, "sce_sys") || unicode::ascii_iequals(first, "sce_module") ||
                   (first.size() >= 7 && unicode::ascii_iequals(first.substr(0, 7), "fakelib"))))
        return true;
    for (std::string_view a : {"ampr_assets.index", "ampr_assets.index.crc", "ampr_assets.index.runtime", "ampr_emu.index",
                               "fakelib/libSceAmpr.sprx", "ampr_commands.bin", "ampr_emu.log", "apr_emu.log"})
        if (unicode::ascii_iequals(path, a)) return true;
    // ... and zstd_pack.rs's loose(): the zstd pack files themselves.
    if (!in_dir) {
        std::string lower(path);
        for (char& c : lower)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (lower == kIndexName) return true;
        if (lower.size() >= 14 && lower.compare(0, 10, kVolumePrefix) == 0 && ends_with_icase(lower, kVolumeSuffix)) return true;
    }
    return false;
}

bool default_selected(std::string_view path, std::uint64_t size) { return size != 0 && !always_loose(path); }

} // namespace alembic::zstdpack
