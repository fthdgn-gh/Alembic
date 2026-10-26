// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from pearlxcore/ProsperoPkgTool (MIT): Containers/ProsperoNapsLayout.cs,
// ProsperoPs5InnerImageReader.cs, NapsDialectRegistry.cs, NapsIndexedLogicalMap.cs,
// NapsSparseFidxDialect.cs, ProsperoInnerMountStream.cs, ProsperoInnerPfsReader.cs.
#include "alembic/fpkg_inner.hpp"
#include "alembic/thread_pool.hpp"

#include "alembic/bytes.hpp"
#include "alembic/kraken.hpp"
#include "alembic/pfs.hpp"
#include "alembic/pfsc.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <condition_variable>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>

namespace alembic::fpkg {

namespace {

constexpr std::uint32_t kChunk128K = 0x20000;
constexpr std::uint32_t kPage = 0x40000; // 256 KiB uncompressed block
constexpr std::uint32_t kMod = 0x3FFFF;
constexpr std::uint8_t kSparseMarker = 0x40;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

// ---------------------------------------------------------------- NAPS layout

NapsCounts decode_header(const std::uint8_t* h) {
    std::uint64_t w0 = get_u64(h), w1 = get_u64(h + 8);
    NapsCounts c;
    c.num_files = static_cast<int>(w0 & 0xFFFFFF) + 1;
    c.compression_type = static_cast<int>((w0 >> 24) & 0x3);
    c.num_keys = static_cast<int>((w0 >> 26) & 0x3) + 1;
    c.num_shuffle_patterns = static_cast<int>((w0 >> 28) & 0xF);
    c.num_ublocks = static_cast<int>((w0 >> 32) & 0xFFFFFF);
    c.num_outer_blocks = static_cast<int>(w1 & 0xFFFFFF);
    c.num_cblock_info = static_cast<int>((w1 >> 24) & 0xFFFFFF) + 2;
    return c;
}

std::uint64_t cblock_info_base(const NapsCounts& c) {
    std::uint64_t raw = std::uint64_t(c.num_shuffle_patterns + c.num_outer_blocks) * 8 +
                        10ull * ((std::uint64_t(c.num_ublocks) + 8) >> 3) + 6ull * std::uint64_t(c.num_files - 1) + 29;
    return raw & ~7ull;
}

CblockInfo decode_cblock(const std::uint8_t* e) {
    CblockInfo c;
    std::memcpy(c.raw, e, 9);
    std::uint64_t lo = get_u64(e);
    std::uint64_t hi = e[8];
    c.is_run = ((lo >> 18) & 1) != 0;
    std::uint32_t coff = static_cast<std::uint32_t>(lo & 0x3FFFF);
    if (!c.is_run) {
        c.coff_start_mod = coff;
        c.uoff_start = static_cast<std::uint32_t>((lo >> 20) & 0x3FFFF);
        c.clen_even_minus1 = static_cast<std::uint32_t>((lo >> 38) & 0x1FFFF);
        c.even = static_cast<std::uint8_t>((lo >> 55) & 0x7);
        c.odd = static_cast<std::uint8_t>((lo >> 58) & 0x7);
        c.kde = static_cast<std::uint8_t>(((hi & 0x7) << 3) | ((lo >> 61) & 0x7));
        c.shuffle = static_cast<std::uint8_t>((hi >> 4) & 0xF);
    } else {
        c.coff_end_mod = coff;
        c.tweak_start = static_cast<std::uint32_t>((lo >> 20) & 0xFFFFFFF);
        c.key_table = static_cast<std::uint8_t>((lo >> 48) & 0x3);
        c.coff_start_256k = static_cast<std::uint32_t>(((hi & 0xFF) << 14) | ((lo >> 50) & 0x3FFF));
    }
    return c;
}

std::optional<std::uint64_t> mount_end(const NapsLayout& n) {
    if (n.fidx.empty()) return std::nullopt;
    const auto& sentinel = n.fidx.back();
    if (sentinel.type == kSparseMarker) return sentinel.offset;
    return std::nullopt;
}

int derive_kraken_flags(std::uint8_t even, std::uint8_t odd, bool multi_chunk) {
    int flags = 0;
    if (even & 4) flags |= kraken::kChunk0NewLz;
    if (even & 2) flags |= kraken::kChunk0SubLiterals;
    if (multi_chunk) {
        if (odd & 4) flags |= kraken::kChunk1NewLz;
        if (odd & 2) flags |= kraken::kChunk1SubLiterals;
    }
    return flags;
}

struct Std {
    const CblockInfo* entry;
    const CblockInfo* run;
    int run_index;
    int entry_index;
    std::uint32_t end_coff_mod;
};

std::vector<Std> collect_stds(const NapsLayout& n) {
    std::vector<Std> stds;
    const CblockInfo* active = nullptr;
    int run_index = -1;
    const auto& cb = n.cblocks;
    for (std::size_t i = 0; i < cb.size(); ++i) {
        if (cb[i].is_run) {
            active = &cb[i];
            run_index = static_cast<int>(i);
            continue;
        }
        // A span ends at the next record, or at a following RUN's end offset.
        std::uint32_t end = i + 1 < cb.size() ? (cb[i + 1].is_run ? cb[i + 1].coff_end_mod : cb[i + 1].coff_start_mod)
                                              : cb[i].coff_start_mod;
        stds.push_back({&cb[i], active, run_index, static_cast<int>(i), end});
    }
    return stds;
}

// compatible-coffset-v1 (native_spans = false) and native-span-v1 (true).
Result<std::vector<InnerBlock>> walk_blocks(const NapsLayout& n, std::uint64_t image_length, bool native_spans) {
    const auto& cb = n.cblocks;
    if (native_spans) {
        bool bad = cb.size() < 3 || !cb.front().is_run || cb.back().is_run || cb.back().even != 0 || cb.back().odd != 0;
        for (const CblockInfo& e : cb)
            if (!e.is_run && ((e.raw[8] & 8) != 0 || e.kde != 0)) bad = true;
        for (std::size_t i = 0; i + 1 < cb.size(); ++i)
            if (cb[i].is_run && cb[i + 1].is_run) bad = true;
        if (bad) return invalid("native span layout requires unmarked KDE=0 records, an initial RUN and a terminator");
    }
    std::vector<Std> stds = collect_stds(n);
    if (stds.size() < 2) return invalid("NAPS CblockInfo has no STD block and terminator");

    bool any_marked = false, any_high_run = false;
    for (const Std& s : stds) {
        if (s.entry->raw[8] & 0x08) any_marked = true;
        if (s.run && s.run->coff_start_256k > 0) any_high_run = true;
    }
    bool legacy = !native_spans && n.counts.compression_type != 0 && !any_marked && !any_high_run;

    std::vector<InnerBlock> result;
    result.reserve(stds.size() - 1);
    std::uint64_t abs_uoff = 0;
    std::uint64_t cursor = 0;
    int last_run = -1;
    std::unordered_map<std::uint64_t, std::size_t> owners;
    for (std::size_t i = 0; i + 1 < stds.size(); ++i) {
        const CblockInfo& curr = *stds[i].entry;
        const CblockInfo& next = *stds[i + 1].entry;
        if (legacy) {
            cursor = curr.coff_start_mod;
        } else if (stds[i].run && stds[i].run_index != last_run) {
            cursor = (std::uint64_t(stds[i].run->coff_start_256k) << 18) | curr.coff_start_mod;
            last_run = stds[i].run_index;
        }
        std::uint32_t uncompressed = ((next.uoff_start - curr.uoff_start - 1) & kMod) + 1;
        std::uint32_t total = legacy ? ((next.coff_start_mod - curr.coff_start_mod) & kMod)
                                     : ((stds[i].end_coff_mod - curr.coff_start_mod) & kMod);
        std::uint32_t even_comp = curr.clen_even_minus1 + 1;
        bool zero_hole = (curr.even == 0 && curr.odd == 0 && curr.kde == 0) ||
                         (curr.even == 1 && curr.kde == 0 && curr.clen_even_minus1 == 7 &&
                          ((curr.odd == 1 && total == 16 && uncompressed > 16) ||
                           (curr.odd == 0 && total == 8 && uncompressed > 8)));
        bool stored = (curr.raw[8] & 0x08) != 0 && !zero_hole;
        if (native_spans && !zero_hole && curr.even == 1 && curr.odd == (uncompressed > kChunk128K ? 1 : 0) &&
            even_comp == std::min(uncompressed, kChunk128K) && total == (uncompressed & kMod))
            stored = true;
        if (stored) total = uncompressed;
        bool dedup = legacy ? !zero_hole && (total == 0 || (curr.kde == 0 && curr.even == 1 && curr.odd == 1 &&
                                                             total < uncompressed))
                            : !stored && !zero_hole && total == 0;
        bool is_kraken = !stored && !dedup && !zero_hole && total < uncompressed;
        if (native_spans) {
            if ((cursor & kMod) != curr.coff_start_mod) return invalid("NAPS source cursor disagrees with an STD record");
            if (!stds[i].run || (stds[i].entry_index > 0 && cb[stds[i].entry_index - 1].is_run &&
                                 cb[stds[i].entry_index - 1].key_table != 0))
                return make_error(Errc::unsupported, "unsupported NAPS RUN record");
            if (!dedup && (cursor > image_length || total > image_length - cursor))
                return invalid("NAPS source outside the inner image");
            if (!zero_hole && !dedup && !stored && !is_kraken) return invalid("invalid native stored span");
            if (is_kraken && (total == 0 || even_comp > total || (uncompressed > kChunk128K && even_comp == total)))
                return invalid("invalid compressed range");
        }
        std::int64_t owner = -1;
        if (dedup) {
            auto it = owners.find(cursor);
            if (it != owners.end()) owner = static_cast<std::int64_t>(it->second);
            if (!legacy && (owner < 0 || result[owner].compressed_offset != cursor))
                return invalid("NAPS dedup alias references a compressed offset with no source block");
            if (native_spans && result[owner].length < uncompressed)
                return invalid("NAPS alias exceeds its source block");
        }
        InnerBlock b;
        b.compressed_offset = cursor;
        b.compressed_length = total;
        b.even_length = even_comp;
        b.offset = abs_uoff;
        b.length = uncompressed;
        b.kraken = is_kraken;
        b.zero_hole = zero_hole;
        b.alias = dedup;
        b.source = owner;
        b.run = last_run;
        b.kraken_flags = is_kraken ? derive_kraken_flags(curr.even, curr.odd, uncompressed > kChunk128K) : 0;
        if (curr.shuffle != 0 && !zero_hole && !dedup)
            return make_error(Errc::unsupported, "shuffled NAPS blocks are not supported");
        result.push_back(b);
        if (!zero_hole && !dedup) owners[cursor] = result.size() - 1;
        abs_uoff += uncompressed;
        if ((!zero_hole || native_spans) && !dedup) cursor += total;
    }
    if (native_spans && (abs_uoff <= (std::uint64_t(n.counts.num_ublocks) - 1) * kPage ||
                         abs_uoff > std::uint64_t(n.counts.num_ublocks) * kPage))
        return invalid("native span coverage disagrees with the NAPS ublock count");
    return result;
}

// The sparse-fidx walk (Sony's own writer): unmarked full stored spans are
// recognised by their wrapped delta; native zero holes still occupy payload.
Result<std::vector<InnerBlock>> walk_unmarked_stored(const NapsLayout& n, std::uint64_t image_length) {
    std::vector<Std> stds = collect_stds(n);
    if (stds.size() < 2) return invalid("NAPS CblockInfo has no STD block and terminator");
    std::vector<InnerBlock> result;
    result.reserve(stds.size() - 1);
    std::uint64_t abs_uoff = 0, cursor = 0;
    int last_run = -1;
    std::unordered_map<std::uint64_t, std::size_t> owners;
    for (std::size_t i = 0; i + 1 < stds.size(); ++i) {
        const CblockInfo& curr = *stds[i].entry;
        const CblockInfo& next = *stds[i + 1].entry;
        if (stds[i].run && stds[i].run_index != last_run) {
            cursor = (std::uint64_t(stds[i].run->coff_start_256k) << 18) | curr.coff_start_mod;
            last_run = stds[i].run_index;
        }
        std::uint32_t uncompressed = ((next.uoff_start - curr.uoff_start - 1) & kMod) + 1;
        std::uint32_t total = (stds[i].end_coff_mod - curr.coff_start_mod) & kMod;
        std::uint32_t even_comp = curr.clen_even_minus1 + 1;
        bool zero_hole = (curr.even == 0 && curr.odd == 0 && curr.kde == 0) ||
                         (curr.even == 1 && curr.kde == 0 && curr.clen_even_minus1 == 7 &&
                          ((curr.odd == 1 && total == 16 && uncompressed > 16) ||
                           (curr.odd == 0 && total == 8 && uncompressed > 8)));
        bool stored = (curr.raw[8] & 0x08) != 0 && !zero_hole;
        if (!zero_hole && curr.even == 1 && curr.odd == (uncompressed > kChunk128K ? 1 : 0) &&
            even_comp == std::min(uncompressed, kChunk128K) && total == (uncompressed & kMod))
            stored = true;
        if (stored) total = uncompressed;
        bool dedup = !stored && !zero_hole && total == 0;
        bool is_kraken = !stored && !dedup && !zero_hole && total < uncompressed;
        if (!zero_hole && !dedup && (cursor > image_length || total > image_length - cursor))
            return invalid("NAPS source outside the inner image");
        if (!zero_hole && !dedup && !stored && !is_kraken) return invalid("invalid NAPS span");
        if (is_kraken && (total == 0 || even_comp > total || (uncompressed > kChunk128K && even_comp == total)))
            return invalid("invalid compressed range");
        std::int64_t owner = -1;
        if (dedup) {
            auto it = owners.find(cursor);
            if (it == owners.end() || result[it->second].compressed_offset != cursor)
                return invalid("NAPS dedup alias references a compressed offset with no source block");
            owner = static_cast<std::int64_t>(it->second);
        }
        if (curr.shuffle != 0 && !zero_hole && !dedup)
            return make_error(Errc::unsupported, "shuffled NAPS blocks are not supported");
        InnerBlock b;
        b.compressed_offset = cursor;
        b.compressed_length = total;
        b.even_length = even_comp;
        b.offset = abs_uoff;
        b.length = uncompressed;
        b.kraken = is_kraken;
        b.zero_hole = zero_hole;
        b.alias = dedup;
        b.source = owner;
        b.run = last_run;
        b.kraken_flags = is_kraken ? derive_kraken_flags(curr.even, curr.odd, uncompressed > kChunk128K) : 0;
        result.push_back(b);
        bool advances = (!zero_hole || (curr.even == 1 && curr.clen_even_minus1 == 7)) && !dedup;
        if (!zero_hole && !dedup) owners[cursor] = result.size() - 1;
        abs_uoff += uncompressed;
        if (advances) cursor += total;
    }
    return result;
}

// Inserts the whole 256 KiB sparse pages that the STD stream omits, using the
// u2c table's absolute page anchors, then checks every fidx boundary.
// `sparse_holes` selects the sparse-fidx rules (marked fidx entries must start holes).
Result<std::vector<InnerBlock>> restore_pages(const NapsLayout& n, const std::vector<InnerBlock>& blocks,
                                              bool sparse_holes) {
    if (blocks.empty()) return invalid("no NAPS blocks");
    std::uint64_t walked_end = blocks.back().offset + blocks.back().length;
    auto declared = mount_end(n);
    if (!declared || *declared <= walked_end || (*declared - walked_end) % kPage != 0)
        return invalid("the NAPS map needs an exact fidx mount end and missing whole pages");
    std::uint64_t declared_end = *declared;
    std::vector<int> std_indices;
    for (std::size_t i = 0; i < n.cblocks.size(); ++i)
        if (!n.cblocks[i].is_run) std_indices.push_back(static_cast<int>(i));
    if (std_indices.size() != blocks.size() + 1) return invalid("the NAPS map needs one STD per block and a terminator");
    if (n.counts.num_ublocks <= 0 || declared_end <= (std::uint64_t(n.counts.num_ublocks) - 1) * kPage ||
        declared_end > std::uint64_t(n.counts.num_ublocks) * kPage)
        return invalid("the NAPS mount end disagrees with the ublock count");

    std::unordered_map<int, std::uint64_t> anchors;
    std::vector<int> page_indices(static_cast<std::size_t>(n.counts.num_ublocks));
    int previous = -1;
    for (int page = 0; page < n.counts.num_ublocks; ++page) {
        if (static_cast<std::size_t>(page / 8) >= n.u2c.size()) return invalid("the NAPS u2c table is too short");
        const auto& group = n.u2c[page / 8];
        std::int64_t index = group.base + (page % 8 == 0 ? 0 : group.delta[page % 8 - 1]);
        if (index < previous || index >= static_cast<std::int64_t>(n.cblocks.size()) || n.cblocks[index].is_run)
            return invalid("invalid NAPS u2c anchor");
        previous = page_indices[page] = static_cast<int>(index);
        anchors[static_cast<int>(index)] = std::uint64_t(page) * kPage;
    }

    std::vector<InnerBlock> restored;
    restored.reserve(blocks.size());
    std::vector<std::uint64_t> starts(std_indices.size());
    std::vector<std::size_t> new_index(blocks.size());
    std::uint64_t shift = 0;
    for (std::size_t i = 0; i < std_indices.size(); ++i) {
        std::uint64_t logical = (i < blocks.size() ? blocks[i].offset : walked_end) + shift;
        auto anchor = anchors.find(std_indices[i]);
        if (anchor != anchors.end() && logical < anchor->second) {
            std::uint64_t gap = ((anchor->second - logical + kPage - 1) / kPage) * kPage;
            if (gap > declared_end - walked_end - shift) return invalid("NAPS anchors demand too many sparse pages");
            for (std::uint64_t added = 0; added < gap; added += kPage) {
                InnerBlock hole;
                hole.offset = logical + added;
                hole.length = kPage;
                hole.zero_hole = true;
                restored.push_back(hole);
            }
            shift += gap;
            logical += gap;
        }
        if ((logical & (kPage - 1)) != n.cblocks[std_indices[i]].uoff_start)
            return invalid("the NAPS logical offset disagrees with an STD record");
        starts[i] = logical;
        if (i == blocks.size()) break;
        InnerBlock b = blocks[i];
        if (b.alias && b.source >= 0) {
            if (static_cast<std::size_t>(b.source) >= i) return invalid("a NAPS alias must reference an earlier block");
            b.source = static_cast<std::int64_t>(new_index[b.source]);
        }
        new_index[i] = restored.size();
        b.offset = logical;
        restored.push_back(b);
    }
    if (starts.back() != declared_end) return invalid("the NAPS coverage does not reach the fidx mount end");

    std::size_t first = 0;
    for (int page = 0; page < n.counts.num_ublocks; ++page) {
        while (first + 1 < starts.size() && starts[first] < std::uint64_t(page) * kPage) ++first;
        if (std_indices[first] != page_indices[page]) return invalid("a NAPS u2c page disagrees with the block map");
    }

    std::set<std::uint64_t> payload_starts(starts.begin(), starts.end());
    std::set<std::uint64_t> hole_starts;
    for (const InnerBlock& b : restored)
        if (b.zero_hole) hole_starts.insert(b.offset);
    std::uint64_t prev_offset = 0;
    std::size_t limit = sparse_holes ? n.fidx.size() - 1 : n.fidx.size();
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& f = n.fidx[i];
        bool boundary = sparse_holes && f.type == kSparseMarker ? hole_starts.count(f.offset) > 0
                                                                : payload_starts.count(f.offset) > 0;
        if (f.offset < prev_offset || f.offset > declared_end || !boundary)
            return invalid("a NAPS fidx entry disagrees with the block boundaries");
        prev_offset = f.offset;
    }
    return restored;
}

Result<std::vector<InnerBlock>> decode_sparse_fidx(const NapsLayout& n, std::uint64_t image_length) {
    int markers = 0;
    for (std::size_t i = 0; i + 1 < n.fidx.size(); ++i)
        if (n.fidx[i].type == kSparseMarker) ++markers;
    if (markers == 0) return invalid("sparse-fidx needs a non-sentinel fidx entry with the 0x40 marker");
    if (!mount_end(n)) return invalid("sparse-fidx needs an exact fidx mount sentinel");
    ALEMBIC_ASSIGN(std::vector<InnerBlock> blocks, walk_unmarked_stored(n, image_length));
    return restore_pages(n, blocks, true);
}

std::uint64_t mount_size_of(const std::vector<InnerBlock>& blocks) {
    std::uint64_t end = 0;
    for (const InnerBlock& b : blocks) end = std::max<std::uint64_t>(end, b.offset + b.length);
    return align_up(end, 0x10000);
}

// What a read returns, ignoring RUN bookkeeping and zero-span encodings.
bool equivalent(const std::vector<InnerBlock>& a, const std::vector<InnerBlock>& b) {
    if (a.size() != b.size()) return false;
    auto norm = [](const std::vector<InnerBlock>& v, std::size_t i) {
        InnerBlock x = v[i];
        if (x.alias && (x.source < 0 || static_cast<std::size_t>(x.source) >= i || v[x.source].length < x.length))
            x.zero_hole = true;
        if (x.zero_hole) {
            InnerBlock z;
            z.offset = x.offset;
            z.length = x.length;
            z.zero_hole = true;
            return z;
        }
        x.run = -1;
        if (!x.kraken) x.even_length = 0;
        if (!x.kraken && !x.alias) x.compressed_length = x.length;
        return x;
    };
    for (std::size_t i = 0; i < a.size(); ++i) {
        InnerBlock x = norm(a, i), y = norm(b, i);
        if (x.compressed_offset != y.compressed_offset || x.compressed_length != y.compressed_length ||
            x.even_length != y.even_length || x.offset != y.offset || x.length != y.length || x.kraken != y.kraken ||
            x.zero_hole != y.zero_hole || x.alias != y.alias || x.source != y.source || x.kraken_flags != y.kraken_flags)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------- inner PFS

constexpr std::uint64_t kInnerBlock = 0x10000;
constexpr std::uint32_t kInnerInodeSize = 0xA8;
constexpr std::uint64_t kInodesPerBlock = kInnerBlock / kInnerInodeSize;
constexpr std::uint64_t kPfsMagic = 20130315;

struct InnerInode {
    bool is_directory;
    std::uint64_t size;
    std::uint64_t offset;
};

Result<bool> is_superblock(const RandomAccess& mount, std::uint64_t offset) {
    std::uint8_t probe[0x28];
    ALEMBIC_TRY(mount.read_at(offset, MutableBytes(probe, sizeof probe)));
    return get_u64(probe) == 2 && get_u64(probe + 8) == kPfsMagic && get_u32(probe + 0x20) == kInnerBlock;
}

Status walk_inner(const RandomAccess& mount, std::uint64_t length, const std::vector<InnerInode>& inodes,
                  std::uint32_t index, const std::string& prefix, std::vector<InnerEntry>& out,
                  std::set<std::uint32_t>& active) {
    if (index >= inodes.size()) return invalid("an inner PFS folder references an invalid inode");
    if (!active.insert(index).second) return invalid("the inner PFS folder graph has a cycle");
    const InnerInode& dir = inodes[index];
    if (!dir.is_directory) return invalid("an inner PFS folder entry points to a file inode");
    if (dir.offset > length || dir.size > length - dir.offset || dir.size > 64u * 1024 * 1024)
        return invalid("inner PFS folder data lies outside the mount");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(dir.size));
    ALEMBIC_TRY(mount.read_at(dir.offset, data));

    std::vector<std::pair<std::uint32_t, std::string>> pending;
    std::size_t offset = 0;
    while (offset + 16 <= data.size()) {
        std::uint32_t child = get_u32(&data[offset]);
        std::uint32_t type = get_u32(&data[offset + 4]);
        std::uint32_t name_length = get_u32(&data[offset + 8]);
        std::uint32_t record = get_u32(&data[offset + 12]);
        if (record == 0) break; // zero padding to the end of a 64 KiB folder block
        if (record < 16 || record > data.size() - offset || name_length > record - 16)
            return invalid("an inner PFS folder record is malformed");
        std::string name(reinterpret_cast<const char*>(&data[offset + 16]), name_length);
        offset += record;
        if (name == "." || name == "..") continue;
        if (name.empty() || name.find('/') != std::string::npos || name.find('\0') != std::string::npos ||
            child >= inodes.size())
            return invalid("an inner PFS folder has an invalid name or inode");
        std::string path = prefix + "/" + name;
        bool is_dir = type == 3;
        if (is_dir != inodes[child].is_directory) return invalid("an inner PFS entry type disagrees with its inode");
        if (is_dir) pending.emplace_back(child, path);
        else out.push_back({path, false, inodes[child].offset, inodes[child].size, child});
    }
    for (const auto& [child, path] : pending) {
        out.push_back({path, true, inodes[child].offset, inodes[child].size, child});
        ALEMBIC_TRY(walk_inner(mount, length, inodes, child, path, out, active));
    }
    active.erase(index);
    return {};
}

// ---------------------------------------------------------------- tree

class MemoryFile final : public RandomAccess {
public:
    explicit MemoryFile(std::vector<std::uint8_t> data) : data_(std::move(data)) {}
    Result<std::uint64_t> size() const override { return data_.size(); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > data_.size() || out.size() > data_.size() - offset) return invalid("read past the end");
        std::memcpy(out.data(), data_.data() + offset, out.size());
        return {};
    }

private:
    std::vector<std::uint8_t> data_;
};

// CNT entries that are sce_sys files even when the names table omits them.
const char* system_file_name(std::uint32_t id) {
    switch (id) {
    case 0x400: return "license.dat";
    case 0x401: return "license.info";
    case 0x402: return "nptitle.dat";
    case 0x403: return "npbind.dat";
    case 0x404: return "selfinfo.dat";
    case 0x406: return "imageinfo.dat";
    case 0x407: return "target-deltainfo.dat";
    case 0x408: return "origin-deltainfo.dat";
    case 0x409: return "psreserved.dat";
    default: return nullptr;
    }
}

bool is_internal_entry(std::uint32_t id) {
    return id < 0x400 || id == 0x40A; // digests, keys, metas, names, image digests
}

class PackageTree final : public tree::Source {
public:
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return label_; }

    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& node) const override {
        if (node.is_directory) return make_error(Errc::invalid_argument, "not a file");
        if (node.id & kEntryBit) {
            const CntEntry& e = opened_.package.entries[node.id & ~kEntryBit];
            ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, read_entry(*file_, opened_.package, e, passcode_));
            return std::shared_ptr<const RandomAccess>(std::make_shared<MemoryFile>(std::move(data)));
        }
        if (ps4_inner_) return ps4_inner_->open(node);
        const InnerEntry& e = opened_.entries[node.id];
        return std::shared_ptr<const RandomAccess>(std::make_shared<Slice>(opened_.mount, e.offset, e.size));
    }

    static constexpr std::uint64_t kEntryBit = 1ull << 63;
    std::shared_ptr<const RandomAccess> file_;
    std::string passcode_;
    OpenedPackage opened_;
    std::unique_ptr<tree::Source> ps4_inner_; // PS4: the inner PFS; its node ids are kept
    tree::Node root_;
    std::string label_;
};

tree::Node* child_dir(tree::Node& parent, const std::string& name) {
    for (tree::Node& c : parent.children)
        if (c.is_directory && c.name == name) return &c;
    tree::Node n;
    n.name = name;
    n.is_directory = true;
    parent.children.push_back(std::move(n));
    return &parent.children.back();
}

} // namespace

Result<NapsLayout> parse_naps(const std::vector<std::uint8_t>& blob) {
    if (blob.size() < 16) return invalid("naps_pkg_layout.dat is too small");
    NapsLayout n;
    n.counts = decode_header(blob.data());
    const NapsCounts& c = n.counts;
    std::uint64_t pos = 16 + std::uint64_t(c.num_outer_blocks) * 8 + std::uint64_t(c.num_shuffle_patterns) * 8;
    std::uint64_t fidx_offset = pos;
    std::uint64_t u2c_offset = fidx_offset + std::uint64_t(c.num_files) * 6;
    std::uint64_t cbi_offset = cblock_info_base(c);
    std::uint64_t cbi_end = cbi_offset + std::uint64_t(c.num_cblock_info) * 9;
    if (u2c_offset + std::uint64_t(c.num_u2c()) * 10 > blob.size() || cbi_end > blob.size() ||
        cbi_offset < u2c_offset + std::uint64_t(c.num_u2c()) * 10)
        return invalid("naps_pkg_layout.dat is shorter than its header declares");
    for (int i = 0; i < c.num_files; ++i) {
        const std::uint8_t* e = blob.data() + fidx_offset + i * 6;
        std::uint64_t offset = 0;
        for (int k = 0; k < 5; ++k) offset |= std::uint64_t(e[k]) << (8 * k);
        n.fidx.push_back({e[5], offset});
    }
    for (int i = 0; i < c.num_u2c(); ++i) {
        const std::uint8_t* e = blob.data() + u2c_offset + i * 10;
        NapsLayout::U2c u;
        u.base = std::uint32_t(e[0]) | (std::uint32_t(e[1]) << 8) | (std::uint32_t(e[2]) << 16);
        std::memcpy(u.delta, e + 3, 7);
        n.u2c.push_back(u);
    }
    for (int i = 0; i < c.num_cblock_info; ++i) n.cblocks.push_back(decode_cblock(blob.data() + cbi_offset + i * 9));
    return n;
}

Result<BlockPlan> plan_blocks(const std::vector<std::uint8_t>& naps, std::uint64_t image_length, const PlanProbe& probe) {
    ALEMBIC_ASSIGN(NapsLayout layout, parse_naps(naps));
    struct Candidate {
        std::string id;
        std::vector<InnerBlock> blocks;
    };
    std::vector<Candidate> candidates;
    std::vector<std::string> diagnostics;
    auto declared = mount_end(layout);
    auto consider = [&](const char* id, Result<std::vector<InnerBlock>> r) {
        if (!r.ok()) {
            diagnostics.push_back(std::string(id) + ": " + r.error().message);
            return;
        }
        std::uint64_t end = r->empty() ? 0 : r->back().offset + r->back().length;
        if (declared && end != *declared) {
            diagnostics.push_back(std::string(id) + ": coverage disagrees with the fidx mount end");
            return;
        }
        candidates.push_back({id, std::move(*r)});
    };
    consider("compatible-coffset-v1", walk_blocks(layout, image_length, false));
    consider("native-span-v1", walk_blocks(layout, image_length, true));
    {
        auto plain = walk_blocks(layout, image_length, false);
        consider("indexed-coffset-v1", plain.ok() ? restore_pages(layout, *plain, false)
                                                  : Result<std::vector<InnerBlock>>(plain.take_error()));
    }
    // Last resort, so packages that read through the dialects above keep their exact result.
    if (candidates.empty()) consider("sparse-fidx-v1", decode_sparse_fidx(layout, image_length));

    bool ambiguous = false;
    for (std::size_t i = 1; i < candidates.size(); ++i)
        if (!equivalent(candidates[0].blocks, candidates[i].blocks)) ambiguous = true;
    if (ambiguous) {
        if (!probe) return invalid("ambiguous NAPS interpretations need a file system probe");
        for (std::size_t i = candidates.size(); i-- > 0;) {
            Status s = probe(candidates[i].blocks);
            if (!s.ok()) {
                diagnostics.push_back(candidates[i].id + ": probe: " + s.error().message);
                candidates.erase(candidates.begin() + static_cast<long>(i));
            }
        }
        for (std::size_t i = 1; i < candidates.size(); ++i)
            if (!equivalent(candidates[0].blocks, candidates[i].blocks))
                return invalid("ambiguous NAPS interpretations both pass the file system probe");
    }
    if (candidates.empty()) {
        std::string message = "no supported NAPS interpretation";
        for (const std::string& d : diagnostics) message += " | " + d;
        return make_error(Errc::unsupported, message);
    }
    BlockPlan plan;
    plan.dialect = candidates[0].id;
    plan.blocks = std::move(candidates[0].blocks);
    plan.diagnostics = std::move(diagnostics);
    plan.mount_size = mount_size_of(plan.blocks);
    return plan;
}

struct InnerMount::Ahead {
    std::size_t index = 0;
    std::vector<std::uint8_t> compressed;
    std::vector<std::uint8_t> decoded;
    kraken::DecodeStatus status = kraken::DecodeStatus::success;
    bool done = false;
    std::mutex mutex;
    std::condition_variable cv;
};

InnerMount::InnerMount(std::shared_ptr<const RandomAccess> inner_image, std::vector<InnerBlock> blocks,
                       std::uint64_t mount_size)
    : image_(std::move(inner_image)), blocks_(std::move(blocks)), mount_size_(mount_size) {
    auto size = image_->size();
    image_size_ = size.ok() ? *size : 0;
}

// Jobs hold their Ahead; the pool finishes them before it goes.
InnerMount::~InnerMount() { pool_.reset(); }

namespace {
// Blocks decoded ahead per worker: enough to keep them busy, ~0.5 MiB each.
constexpr std::size_t kAheadPerWorker = 3;
} // namespace

Status InnerMount::fill_ahead(std::size_t after) const {
    if (!pool_) {
        unsigned n = worker_count(threads_);
        if (n <= 1) return {};
        pool_ = std::make_unique<Pool>(n);
    }
    const std::size_t window = pool_->size() * kAheadPerWorker;
    std::size_t next = std::max(queued_up_to_, after + 1);
    while (ahead_.size() < window && next < blocks_.size()) {
        const InnerBlock& b = blocks_[next];
        // Only Kraken blocks are worth a thread; the others are decoded when read.
        if (b.kraken && !b.alias && !b.zero_hole && b.length > 0 && b.compressed_length > 0 &&
            b.compressed_offset <= image_size_ && b.compressed_length <= image_size_ - b.compressed_offset) {
            auto a = std::make_shared<Ahead>();
            a->index = next;
            a->compressed.resize(b.compressed_length);
            ALEMBIC_TRY(image_->read_at(b.compressed_offset, a->compressed));
            a->decoded.resize(b.length);
            const int flags = b.kraken_flags;
            const std::size_t first = b.length > kChunk128K ? b.even_length : 0;
            pool_->submit([a, flags, first](unsigned) {
                kraken::DecodeStatus st = kraken::decode_block(a->compressed, flags, first, a->decoded);
                std::lock_guard<std::mutex> lock(a->mutex);
                a->status = st;
                a->done = true;
                a->cv.notify_all();
            });
            ahead_.push_back(std::move(a));
        }
        ++next;
    }
    queued_up_to_ = next;
    return {};
}

Status InnerMount::take_ahead(std::size_t index) const {
    while (!ahead_.empty() && ahead_.front()->index < index) ahead_.pop_front();
    if (ahead_.empty() || ahead_.front()->index != index) return make_error(Errc::invalid_argument, "not ahead");
    std::shared_ptr<Ahead> a = ahead_.front();
    ahead_.pop_front();
    {
        std::unique_lock<std::mutex> lock(a->mutex);
        a->cv.wait(lock, [&] { return a->done; });
    }
    const InnerBlock& b = blocks_[index];
    if (a->status != kraken::DecodeStatus::success)
        return invalid("Kraken decode failed for the inner block at mount offset " + std::to_string(b.offset) +
                       (a->status == kraken::DecodeStatus::unsupported_entropy ? " (unsupported entropy type)" : ""));
    cache_.resize(kPage);
    std::memcpy(cache_.data(), a->decoded.data(), b.length);
    return {};
}

Status InnerMount::decode_block(std::size_t index, MutableBytes out, int depth) const {
    if (index >= blocks_.size() || depth > 32) return invalid("invalid inner block reference");
    const InnerBlock& b = blocks_[index];
    MutableBytes dst = out.first(b.length);
    if (b.length == 0 || b.zero_hole) {
        std::memset(dst.data(), 0, dst.size());
        return {};
    }
    if (b.alias) {
        std::memset(dst.data(), 0, dst.size());
        if (b.source < 0 || static_cast<std::size_t>(b.source) >= blocks_.size()) return {};
        std::vector<std::uint8_t> source(blocks_[b.source].length);
        ALEMBIC_TRY(decode_block(static_cast<std::size_t>(b.source), source, depth + 1));
        std::memcpy(dst.data(), source.data(), std::min<std::size_t>(dst.size(), source.size()));
        return {};
    }
    if (!b.kraken) {
        if (b.compressed_length < b.length || b.compressed_offset > image_size_ ||
            b.length > image_size_ - b.compressed_offset)
            return invalid("a stored inner block reads outside pfs_image.dat");
        return image_->read_at(b.compressed_offset, dst);
    }
    if (b.compressed_length == 0 || b.compressed_offset > image_size_ ||
        b.compressed_length > image_size_ - b.compressed_offset)
        return invalid("a Kraken inner block has an invalid source range");
    std::vector<std::uint8_t> compressed(b.compressed_length);
    ALEMBIC_TRY(image_->read_at(b.compressed_offset, compressed));
    std::size_t first = b.length > kChunk128K ? b.even_length : 0;
    kraken::DecodeStatus status = kraken::decode_block(compressed, b.kraken_flags, first, dst);
    if (status != kraken::DecodeStatus::success)
        return invalid("Kraken decode failed for the inner block at mount offset " + std::to_string(b.offset) +
                       (status == kraken::DecodeStatus::unsupported_entropy ? " (unsupported entropy type)" : ""));
    return {};
}

Status InnerMount::read_at(std::uint64_t offset, MutableBytes out) const {
    if (offset > mount_size_ || out.size() > mount_size_ - offset) return invalid("read past the end of the inner mount");
    std::size_t done = 0;
    while (done < out.size()) {
        std::uint64_t at = offset + done;
        // Blocks are in mount order; find the one containing `at`.
        auto it = std::upper_bound(blocks_.begin(), blocks_.end(), at,
                                   [](std::uint64_t v, const InnerBlock& b) { return v < b.offset; });
        if (it == blocks_.begin()) return invalid("an inner mount offset maps to no block");
        std::size_t index = static_cast<std::size_t>(it - blocks_.begin() - 1);
        const InnerBlock& b = blocks_[index];
        if (at >= b.offset + b.length) {
            // Past the last block but inside the 64 KiB-aligned mount: zeros.
            std::size_t n = out.size() - done;
            if (index + 1 < blocks_.size()) n = std::min<std::size_t>(n, blocks_[index + 1].offset - at);
            std::memset(out.data() + done, 0, n);
            done += n;
            continue;
        }
        if (cached_ != index) {
            // Moving forward: the block may be decoded already, and the next
            // ones are queued. Anything else (a jump) drops the read-ahead.
            bool forward = last_ != SIZE_MAX && index > last_ && index <= last_ + 1 + ahead_.size() + 1;
            if (!forward) {
                ahead_.clear();
                queued_up_to_ = 0;
            }
            cached_ = SIZE_MAX;
            if (!(forward && take_ahead(index).ok())) {
                cache_.resize(kPage);
                ALEMBIC_TRY(decode_block(index, cache_));
            }
            cached_ = index;
            last_ = index;
            if (forward || index == 0) ALEMBIC_TRY(fill_ahead(index));
        }
        std::size_t within = static_cast<std::size_t>(at - b.offset);
        std::size_t n = std::min<std::size_t>(out.size() - done, b.length - within);
        std::memcpy(out.data() + done, cache_.data() + within, n);
        done += n;
    }
    return {};
}

Result<std::vector<InnerEntry>> list_inner_pfs(const RandomAccess& mount, std::int64_t superblock_hint) {
    ALEMBIC_ASSIGN(std::uint64_t length, mount.size());
    std::int64_t super = -1;
    if (superblock_hint >= 0 && std::uint64_t(superblock_hint) + kInnerBlock <= length) {
        ALEMBIC_ASSIGN(bool ok, is_superblock(mount, std::uint64_t(superblock_hint)));
        if (ok) super = superblock_hint;
    }
    for (std::uint64_t off = 0; super < 0 && off + kInnerBlock <= length; off += kInnerBlock) {
        ALEMBIC_ASSIGN(bool ok, is_superblock(mount, off));
        if (ok) super = static_cast<std::int64_t>(off);
    }
    if (super < 0) return invalid("no inner PFS superblock in the reconstructed mount");

    std::uint8_t count_bytes[8];
    ALEMBIC_TRY(mount.read_at(std::uint64_t(super) + 0x30, MutableBytes(count_bytes, 8)));
    std::uint64_t inode_count = get_u64(count_bytes);
    if (inode_count < 1 || inode_count > INT32_MAX) return invalid("the inner PFS inode count is invalid");
    std::uint64_t table = std::uint64_t(super) + kInnerBlock;
    std::uint64_t table_blocks = (inode_count + kInodesPerBlock - 1) / kInodesPerBlock;
    if (table + table_blocks * kInnerBlock > length) return invalid("the inner PFS inode table lies outside the mount");

    std::vector<InnerInode> inodes;
    inodes.reserve(static_cast<std::size_t>(inode_count));
    std::vector<std::uint8_t> block(kInnerBlock);
    for (std::uint64_t b = 0; b < table_blocks; ++b) {
        ALEMBIC_TRY(mount.read_at(table + b * kInnerBlock, block));
        for (std::uint64_t k = 0; k < kInodesPerBlock && inodes.size() < inode_count; ++k) {
            const std::uint8_t* e = block.data() + k * kInnerInodeSize;
            std::uint16_t mode = get_u16(e);
            inodes.push_back({mode == 0x416D || mode == 0x4168, get_u64(e + 8), get_u64(e + 0x60)});
        }
    }
    // Directory offsets below the metadata region mean a writer truncated them to 32 bits.
    std::uint64_t metadata_start = std::uint64_t(super) + (1 + table_blocks) * kInnerBlock;
    if (metadata_start > UINT32_MAX)
        for (const InnerInode& inode : inodes)
            if (inode.is_directory && inode.offset < metadata_start)
                return invalid("the package's inner PFS has 32-bit-truncated folder offsets (a known writer bug)");

    std::vector<InnerEntry> out;
    std::set<std::uint32_t> active;
    ALEMBIC_TRY(walk_inner(mount, length, inodes, 0, "", out, active));
    return out;
}

Result<OpenedPackage> open_package(std::shared_ptr<const RandomAccess> file, const std::string& passcode) {
    OpenedPackage o;
    ALEMBIC_ASSIGN(o.package, read_package(*file));
    if (o.package.kind != Kind::finalized_debug)
        return make_error(Errc::unsupported, std::string("cannot open the file system of a ") + name(o.package.kind));
    ALEMBIC_ASSIGN(o.outer, OuterPfs::open(file, o.package, passcode));
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> naps_file, o.outer->open_file("naps_pkg_layout.dat"));
    ALEMBIC_ASSIGN(std::uint64_t naps_size, naps_file->size());
    if (naps_size > 256u * 1024 * 1024) return invalid("naps_pkg_layout.dat is too large");
    std::vector<std::uint8_t> naps(static_cast<std::size_t>(naps_size));
    ALEMBIC_TRY(naps_file->read_at(0, naps));
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> image, o.outer->open_inner_image());
    ALEMBIC_ASSIGN(std::uint64_t image_size, image->size());

    std::int64_t hint = o.package.inner_meta_base > 0 ? static_cast<std::int64_t>(o.package.inner_meta_base * kInnerBlock) : -1;
    PlanProbe probe = [&](const std::vector<InnerBlock>& blocks) -> Status {
        InnerMount mount(image, blocks, mount_size_of(blocks));
        ALEMBIC_ASSIGN(std::vector<InnerEntry> entries, list_inner_pfs(mount, hint));
        for (const InnerEntry& e : entries)
            if (e.is_directory && e.path == "/uroot") return {};
        return make_error(Errc::unsupported, "the probe found no /uroot folder");
    };
    ALEMBIC_ASSIGN(o.plan, plan_blocks(naps, image_size, probe));
    o.mount = std::make_shared<InnerMount>(image, o.plan.blocks, o.plan.mount_size);
    ALEMBIC_ASSIGN(o.entries, list_inner_pfs(*o.mount, hint));
    return o;
}

Result<std::unique_ptr<tree::Source>> open_tree(std::shared_ptr<const RandomAccess> file, std::string label,
                                                const std::string& passcode) {
    auto source = std::make_unique<PackageTree>();
    source->file_ = file;
    source->passcode_ = passcode;
    source->label_ = std::move(label);
    ALEMBIC_ASSIGN(Package package, read_package(*file));
    if (package.ps4()) {
        // PS4: the outer PFS's pfs_image.dat is a PFSC-compressed PFS holding the game.
        ALEMBIC_ASSIGN(std::shared_ptr<OuterPfs> outer, OuterPfs::open(file, package, passcode));
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> image, outer->open_file("pfs_image.dat"));
        ALEMBIC_ASSIGN(std::shared_ptr<pfsc::Reader> inner, pfsc::Reader::open(image));
        ALEMBIC_ASSIGN(source->ps4_inner_, pfs::open_image(std::move(inner), source->label_));
        source->root_ = source->ps4_inner_->root();
        source->opened_.package = std::move(package);
        source->opened_.outer = std::move(outer);
    } else {
        ALEMBIC_ASSIGN(source->opened_, open_package(file, passcode));
    }
    const OpenedPackage& o = source->opened_;

    // Inner PFS: everything under /uroot.
    tree::Node& root = source->root_;
    root.is_directory = true;
    for (std::size_t i = 0; i < o.entries.size(); ++i) {
        const InnerEntry& e = o.entries[i];
        if (e.path.rfind("/uroot/", 0) != 0) continue;
        std::string rest = e.path.substr(7);
        tree::Node* parent = &root;
        std::size_t slash;
        while ((slash = rest.find('/')) != std::string::npos) {
            parent = child_dir(*parent, rest.substr(0, slash));
            rest = rest.substr(slash + 1);
        }
        if (e.is_directory) {
            child_dir(*parent, rest);
        } else {
            tree::Node n;
            n.name = rest;
            n.size = e.size;
            n.id = i;
            parent->children.push_back(std::move(n));
        }
    }

    // sce_sys files kept as CNT entries.
    for (std::size_t i = 0; i < o.package.entries.size(); ++i) {
        const CntEntry& e = o.package.entries[i];
        if (is_internal_entry(e.id)) continue;
        std::string path = e.name;
        if (const char* sys = system_file_name(e.id); sys && (path.empty() || path == sys)) path = sys;
        if (path.empty() || path.find("..") != std::string::npos || path[0] == '/') continue;
        tree::Node* parent = child_dir(root, "sce_sys");
        std::size_t slash;
        while ((slash = path.find('/')) != std::string::npos) {
            parent = child_dir(*parent, path.substr(0, slash));
            path = path.substr(slash + 1);
        }
        if (tree::find_child_icase(*parent, path)) continue; // the inner PFS already has it
        tree::Node n;
        n.name = path;
        n.size = e.data_size;
        n.id = PackageTree::kEntryBit | i;
        parent->children.push_back(std::move(n));
    }
    return std::unique_ptr<tree::Source>(std::move(source));
}

} // namespace alembic::fpkg
