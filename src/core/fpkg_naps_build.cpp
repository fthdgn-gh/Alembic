// SPDX-License-Identifier: GPL-3.0-or-later
// naps_pkg_layout.dat for a built inner image. Ported from
// pearlxcore/ProsperoPkgTool (MIT) Containers/ProsperoNapsPlanGenerator.cs,
// Containers/ProsperoNapsLayoutBuilder.cs and the serializer half of
// Containers/ProsperoNapsLayout.cs. (fpkg_inner.cpp has the reading side.)
#include "alembic/fpkg_build.hpp"

#include "alembic/bytes.hpp"

#include <algorithm>
#include <cstring>

namespace alembic::fpkg::build {

namespace {

constexpr std::uint64_t kUBlock = 0x40000;
constexpr std::uint64_t kMod256K = 0x3FFFF;

struct PlanEntry {
    bool start_run = false;
    bool stored_raw = false;
    bool terminator = false;
    std::uint64_t on_disk = 0;
    std::uint64_t logical = 0;
    std::uint64_t even_length = 0;
    std::uint64_t stream = 0;
    std::uint8_t even = 0, odd = 0, kde = 0, shuffle = 0;
};

// Opens a RUN (re-bases the compressed cursor) where a block does not follow
// the previous one on disk.
class Emitter {
public:
    explicit Emitter(std::vector<PlanEntry>& out) : out_(out) {}

    void emit(PlanEntry e) {
        out_.push_back(e);
        if (!e.terminator) prev_end_ = static_cast<std::int64_t>(e.on_disk + e.stream);
    }
    bool needs_run(std::uint64_t on_disk) {
        bool needed = first_ || static_cast<std::int64_t>(on_disk) != prev_end_;
        first_ = false;
        return needed;
    }
    void stored_full(std::uint64_t on_disk, std::uint64_t logical) {
        PlanEntry e;
        e.start_run = needs_run(on_disk);
        e.on_disk = on_disk;
        e.logical = logical;
        e.even_length = 0x20000;
        e.stream = kUBlock;
        e.even = 1;
        e.odd = 1;
        e.stored_raw = true;
        emit(e);
    }
    Status stored_tail(std::uint64_t on_disk, std::uint64_t logical, std::uint64_t size, std::uint64_t stream) {
        if (size == 0 || size > kUBlock) return make_error(Errc::invalid_data, "NAPS: a stored tail must fit one 256 KiB block");
        PlanEntry e;
        e.start_run = needs_run(on_disk);
        e.on_disk = on_disk;
        e.logical = logical;
        e.even_length = std::min<std::uint64_t>(size, 0x20000);
        e.stream = stream;
        e.even = 1;
        e.odd = size > 0x20000 ? 1 : 0;
        e.stored_raw = true;
        emit(e);
        return {};
    }
    void hole(std::uint64_t on_disk, std::uint64_t logical, std::uint64_t stream, bool full, bool start_run) {
        PlanEntry e;
        e.start_run = start_run || needs_run(on_disk); // short-circuit, as the reference does
        e.on_disk = on_disk;
        e.logical = logical;
        e.even_length = 8;
        e.stream = stream;
        e.even = 1;
        e.odd = full ? 1 : 0;
        emit(e);
    }

private:
    std::vector<PlanEntry>& out_;
    std::int64_t prev_end_ = -1;
    bool first_ = true;
};

struct Cblock {
    bool run = false;
    std::uint8_t raw2 = 0, raw8 = 0; // the two unprinted bits the reference keeps
    std::uint32_t coff = 0;          // STD start mod 256K, or RUN end mod 256K
    std::uint32_t uoff = 0, clen_even_minus1 = 0;
    std::uint8_t even = 0, odd = 0, kde = 0, shuffle = 0;
    std::uint32_t tweak = 0, c256k = 0;
    std::uint8_t key = 0;
};

void encode_cblock(const Cblock& c, std::uint8_t* out) {
    std::uint64_t lo = c.coff & 0x3FFFF;
    std::uint64_t hi;
    if (!c.run) {
        lo |= static_cast<std::uint64_t>(c.uoff & 0x3FFFF) << 20;
        lo |= static_cast<std::uint64_t>(c.clen_even_minus1 & 0x1FFFF) << 38;
        lo |= static_cast<std::uint64_t>(c.even & 7) << 55;
        lo |= static_cast<std::uint64_t>(c.odd & 7) << 58;
        lo |= static_cast<std::uint64_t>(c.kde & 7) << 61;
        hi = ((c.kde >> 3) & 7) | static_cast<std::uint64_t>(c.shuffle & 0xF) << 4;
    } else {
        lo |= 1ULL << 18;
        lo |= static_cast<std::uint64_t>(c.tweak & 0xFFFFFFF) << 20;
        lo |= static_cast<std::uint64_t>(c.key & 3) << 48;
        lo |= static_cast<std::uint64_t>(c.c256k & 0x3FFF) << 50;
        hi = (c.c256k >> 14) & 0xFF;
    }
    lo |= static_cast<std::uint64_t>(c.raw2 & 0x08) << 16;
    if (!c.run) hi |= c.raw8 & 0x08;
    for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(lo >> (8 * i));
    out[8] = static_cast<std::uint8_t>(hi);
}

} // namespace

Result<Naps> build_naps(const InnerImage& inner, Compression compression) {
    std::vector<PlanEntry> blocks;
    Emitter emitter(blocks);

    // 1. Data: the encoded payload blocks, in physical order.
    for (const EncodedBlock& b : inner.encoded_blocks) {
        if (b.uncompressed_length == 0) continue;
        if (b.uncompressed_length > kUBlock) return make_error(Errc::invalid_data, "NAPS: an encoded block exceeds 256 KiB");
        if (!b.is_stored) return make_error(Errc::unsupported, "NAPS: Kraken blocks are not supported yet");
        if (b.uncompressed_length == kUBlock && b.stored_length == kUBlock)
            emitter.stored_full(b.physical_offset, b.logical_offset);
        else
            ALEMBIC_TRY(emitter.stored_tail(b.physical_offset, b.logical_offset, b.uncompressed_length, b.stored_length));
    }

    // 2. The gap [data end, metadata base): 256 KiB zero blocks deduplicated
    //    onto the block-info table.
    if (inner.meta_base_logical > inner.data_end_logical) {
        std::uint64_t padding = inner.meta_base_logical - inner.data_end_logical;
        std::uint64_t holes = div_round_up(padding, kUBlock);
        std::uint64_t base = inner.block_info_on_disk_offset, cursor = base;
        for (std::uint64_t k = 0; k < holes; ++k) {
            bool last = k == holes - 1;
            std::uint64_t on_disk = last ? cursor : base;
            bool full = std::min<std::uint64_t>(kUBlock, padding - k * kUBlock) > 0x20000;
            std::uint64_t stream = full ? 16 : 8;
            emitter.hole(on_disk, inner.data_end_logical + k * kUBlock, stream, full, !last);
            cursor = on_disk + stream;
        }
    }

    // 3. The metadata: the PFSv3 container's blocks at their place in its data section.
    std::uint64_t meta_logical = inner.meta_base_logical;
    std::uint64_t meta_on_disk = inner.metadata_on_disk_offset + (inner.container.size() >= 0xAE ? get_u32(&inner.container[0xAA]) : 0);
    for (const ContainerBlock& b : inner.metadata_blocks) {
        if (b.flags != 0 && b.compressed_size < b.uncompressed_size)
            return make_error(Errc::unsupported, "NAPS: Kraken metadata is not supported yet");
        if (b.compressed_size >= kUBlock) emitter.stored_full(meta_on_disk, meta_logical);
        else ALEMBIC_TRY(emitter.stored_tail(meta_on_disk, meta_logical, b.uncompressed_size, b.compressed_size));
        meta_logical += b.uncompressed_size;
        meta_on_disk += b.compressed_size;
    }

    // 4. The terminator at the mount end.
    const std::uint64_t mount_size = inner.ndblock * kInnerBlock;
    {
        PlanEntry t;
        t.start_run = true;
        t.on_disk = meta_on_disk;
        t.logical = mount_size;
        t.terminator = true;
        blocks.push_back(t);
    }

    // 5. fidx: non-empty afid offsets, then data end, metadata base and mount size.
    std::vector<std::uint64_t> fidx;
    for (const Placement& p : inner.placements)
        if (p.uncompressed_size > 0) fidx.push_back(p.logical_offset);
    fidx.push_back(inner.data_end_logical);
    fidx.push_back(inner.meta_base_logical);
    fidx.push_back(mount_size);

    // Walk the plan with the compressed-offset cursor.
    std::vector<Cblock> entries;
    std::vector<std::pair<std::size_t, std::uint64_t>> std_logical;
    std::uint64_t cursor = 0;
    for (const PlanEntry& b : blocks) {
        if (b.start_run) {
            Cblock r;
            r.run = true;
            r.coff = static_cast<std::uint32_t>(cursor & kMod256K);
            r.tweak = b.terminator ? 0 : static_cast<std::uint32_t>(b.on_disk >> 16);
            r.c256k = static_cast<std::uint32_t>(b.on_disk >> 18);
            entries.push_back(r);
            cursor = b.on_disk;
        }
        Cblock s;
        s.coff = static_cast<std::uint32_t>(cursor & kMod256K);
        s.uoff = static_cast<std::uint32_t>(b.logical & kMod256K);
        if (b.terminator) {
            s.clen_even_minus1 = 0;
            s.raw2 = 0x08; // the last-block marker (bit 19)
        } else {
            s.clen_even_minus1 = static_cast<std::uint32_t>(std::min<std::uint64_t>(b.even_length - 1, 0x1FFFF));
            if (b.stored_raw) s.raw8 = 0x08; // stored-only marker (bit 67)
        }
        s.even = b.even;
        s.odd = b.odd;
        s.kde = b.kde;
        s.shuffle = b.shuffle;
        std_logical.push_back({entries.size(), b.logical});
        entries.push_back(s);
        cursor += b.stream;
    }

    const std::uint64_t num_files = fidx.size();
    const std::uint64_t num_ublocks = div_round_up(mount_size, kUBlock);
    const std::uint64_t num_outer = div_round_up(inner.image_length, kInnerBlock);
    const std::uint64_t num_cblock = entries.size();
    if (num_files - 1 > 0xFFFFFF || num_ublocks > 0xFFFFFF || num_outer > 0xFFFFFF || num_cblock - 2 > 0xFFFFFF)
        return make_error(Errc::too_large, "the package is too large for its NAPS table");

    // u2c: per group of 8 ublocks, the first STD entry whose logical start is in each.
    std::stable_sort(std_logical.begin(), std_logical.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
    std::vector<std::uint64_t> first(num_ublocks);
    {
        std::size_t p = 0;
        for (std::uint64_t u = 0; u < num_ublocks; ++u) {
            while (p < std_logical.size() && std_logical[p].second < u * kUBlock) ++p;
            first[u] = p < std_logical.size() ? std_logical[p].first : num_cblock - 1;
        }
    }
    const std::uint64_t groups = (num_ublocks + 8) / 8;

    // Sections: header, outer-block digests (zero), shuffle patterns (none),
    // fidx, u2c, then the cblock records at an 8-aligned offset.
    const std::uint64_t outer_off = 16, fidx_off = outer_off + num_outer * 8, u2c_off = fidx_off + num_files * 6;
    const std::uint64_t cbi_off = (num_outer * 8 + 10 * groups + 6 * (num_files - 1) + 29) & ~7ULL;
    const std::uint64_t content = cbi_off + num_cblock * 9;
    Naps result;
    result.file_count = num_files;
    std::vector<std::uint8_t>& out = result.blob;
    out.assign(static_cast<std::size_t>(align_up(content, 16)), 0);
    std::uint64_t word0 = ((num_files - 1) & 0xFFFFFF) | (static_cast<std::uint64_t>(compression == Compression::stored ? 0 : 2) << 24) |
                          (0ULL << 26) /* one key */ | (0ULL << 28) /* no shuffle patterns */ | ((num_ublocks & 0xFFFFFF) << 32);
    std::uint64_t word1 = (num_outer & 0xFFFFFF) | (((num_cblock - 2) & 0xFFFFFF) << 24);
    put_u64(&out[0], word0);
    put_u64(&out[8], word1);
    for (std::uint64_t i = 0; i < num_files; ++i) {
        std::uint8_t* e = &out[fidx_off + i * 6];
        for (int b = 0; b < 5; ++b) e[b] = static_cast<std::uint8_t>(fidx[i] >> (8 * b));
        e[5] = i == num_files - 1 ? 0x40 : 0;
    }
    for (std::uint64_t g = 0; g < groups; ++g) {
        std::uint64_t base_u = 8 * g;
        std::uint64_t base = base_u < num_ublocks ? first[base_u] : num_cblock - 1;
        std::uint8_t* e = &out[u2c_off + g * 10];
        e[0] = static_cast<std::uint8_t>(base);
        e[1] = static_cast<std::uint8_t>(base >> 8);
        e[2] = static_cast<std::uint8_t>(base >> 16);
        for (int i = 0; i < 7; ++i) {
            std::uint64_t u = base_u + 1 + i;
            std::uint64_t start = u < num_ublocks ? first[u] : num_cblock - 1;
            if (start - base > 255) return make_error(Errc::too_large, "the NAPS u2c table overflows (layout too large)");
            e[3 + i] = static_cast<std::uint8_t>(start - base);
        }
    }
    for (std::size_t i = 0; i < entries.size(); ++i) encode_cblock(entries[i], &out[cbi_off + i * 9]);
    return result;
}

} // namespace alembic::fpkg::build
