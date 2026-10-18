// SPDX-License-Identifier: GPL-3.0-or-later
// UFS2 (.ffpkg) image writer, ported from SvenGDK/UFS2Tool's
// Ufs2ImageCreator (BSD-2-Clause) as PS5PKGTool uses it in
// Ufs2Operations.CreateFromDirectoryAsync: the same sizing (including the
// retry with larger images), parameters, allocation order and on-disk
// structures, so the output is byte-identical to PS5PKGTool's for the same
// time stamp, process ID and directory order.
//
// PS5PKGTool builds an empty file system, then writes files into it at
// random offsets and patches the metadata. Here the whole layout is planned
// first (inodes, every data and pointer block, the final bitmaps), then the
// image is streamed front to back, one cylinder group after another, so the
// source can be any tree (an FPKG, another image) without a temporary copy.
//
// Differences from PS5PKGTool, by design:
// - Directory order: PS5PKGTool uses whatever order the host lists a folder in
//   (NTFS: case-insensitive). Alembic sorts names case-insensitively (ASCII
//   letters folded, then ordinal), which is NTFS's order for ASCII names.
// - Names are written as UTF-8 with their byte length; PS5PKGTool writes
//   Encoding.ASCII bytes (non-ASCII becomes '?') with the UTF-16 length.
#include "alembic/ufs2.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unistd.h>

namespace alembic::ufs2 {

namespace {

// Ufs2Constants
constexpr std::int32_t kUfs2Magic = 0x19540119;
constexpr std::int32_t kCgMagic = 0x090255;
constexpr std::int64_t kSuperblockOffset = 65536;
constexpr std::int32_t kSuperblockSize = 8192;
constexpr std::int32_t kSector = 512; // DEV_BSIZE: the layout sector after newfs's normalisation
constexpr std::int32_t kInodeSize = 256;
constexpr std::int32_t kNDirect = 12;
constexpr std::int32_t kNIndirect = 3;
constexpr std::int32_t kMaxBSize = 65536;
constexpr std::int32_t kMaxContig = 16;
constexpr std::int32_t kCgHeaderBaseSize = 168;
constexpr std::int32_t kCsumSize = 16;
constexpr std::int32_t kDirBlock = 512;
constexpr std::int32_t kDefaultInodesPerGroup = 2048;
constexpr std::int32_t kAvgFileSize = 16384;
constexpr std::int32_t kAvgFilesPerDir = 64;
constexpr std::uint16_t kIfDir = 0x4000, kIfReg = 0x8000, kPerm = 0x16D; // 0555
constexpr std::uint8_t kDtDir = 4, kDtReg = 8;
constexpr std::uint32_t kRootInode = 2;

std::int64_t align_up(std::int64_t v, std::int64_t a) { return (v + a - 1) / a * a; }
int log2i(std::int64_t v) {
    int r = 0;
    while (v > 1) {
        v >>= 1;
        ++r;
    }
    return r;
}

void put16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put64(std::uint8_t* p, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

// .NET's seeded System.Random (the Knuth subtractive generator kept for
// seeded instances), used by Ufs2ImageCreator for di_gen.
class DotNetRandom {
public:
    explicit DotNetRandom(std::int32_t seed) {
        constexpr std::int32_t kMseed = 161803398;
        std::int32_t subtraction = seed == INT32_MIN ? INT32_MAX : (seed < 0 ? -seed : seed);
        std::int32_t mj = kMseed - subtraction;
        seed_[55] = mj;
        std::int32_t mk = 1;
        int ii = 0;
        for (int i = 1; i < 55; ++i) {
            if ((ii += 21) >= 55) ii -= 55;
            seed_[ii] = mk;
            mk = mj - mk;
            if (mk < 0) mk += INT32_MAX;
            mj = seed_[ii];
        }
        for (int k = 1; k < 5; ++k)
            for (int i = 1; i < 56; ++i) {
                int n = i + 30;
                if (n >= 55) n -= 55;
                seed_[i] -= seed_[1 + n];
                if (seed_[i] < 0) seed_[i] += INT32_MAX;
            }
        inext_ = 0;
        inextp_ = 21;
    }
    // Next(minValue, maxValue) with maxValue - minValue <= int.MaxValue.
    std::int32_t next(std::int32_t min_value, std::int32_t max_value) {
        std::int64_t range = static_cast<std::int64_t>(max_value) - min_value;
        double sample = internal_sample() * (1.0 / INT32_MAX);
        return static_cast<std::int32_t>(sample * static_cast<double>(range)) + min_value;
    }

private:
    std::int32_t internal_sample() {
        int a = inext_ + 1 >= 56 ? 1 : inext_ + 1;
        int b = inextp_ + 1 >= 56 ? 1 : inextp_ + 1;
        std::int32_t r = seed_[a] - seed_[b];
        if (r == INT32_MAX) --r;
        if (r < 0) r += INT32_MAX;
        seed_[a] = r;
        inext_ = a;
        inextp_ = b;
        return r;
    }
    std::int32_t seed_[56] = {};
    int inext_ = 0, inextp_ = 0;
};

Error too_small(const std::string& what) { return Error{Errc::too_large, "Image is too small: " + what}; }
bool is_too_small(const Error& e) { return e.code == Errc::too_large && e.message.rfind("Image is too small", 0) == 0; }

// FreeBSD DIRECTSIZ: header (8) + name + NUL, 4-byte aligned.
std::uint16_t record_length(std::size_t name_length) {
    return static_cast<std::uint16_t>((8 + name_length + 1 + 3) & ~std::size_t(3));
}

} // namespace

struct ImageBuilder::Impl {
    // ---- inputs
    const tree::Source* source = nullptr;
    BuildOptions options;
    std::int64_t now = 0;
    std::int32_t pid = 0;

    // ---- tree
    struct Item {
        const tree::Node* node = nullptr;
        bool dir = false;
        std::string name;          // UTF-8 bytes as written
        std::vector<std::uint32_t> children; // item indices, sorted
        std::uint32_t inode = 0;
        std::uint32_t depth = 0;
        // allocation
        std::vector<std::int64_t> blocks; // dirs: data blocks (absolute frags)
        std::int64_t direct[kNDirect] = {};
        std::int64_t indirect[kNIndirect] = {};
        std::int64_t metadata_blocks = 0;
        std::int64_t size = 0;
        std::uint16_t nlink = 1;
    };
    std::vector<Item> items; // [0] = root
    std::uint64_t total_entries = 0; // every file and folder below the root

    // ---- geometry (WriteFilesystem)
    std::int32_t bsize = 0, fsize = 0, fpb = 0, ipg = 0, fpg = 0, ncg = 0;
    std::int64_t total_frags = 0;
    std::int32_t sblkno = 0, cblkno = 0, iblkno = 0, dblkno = 0;
    std::int32_t cg_size = 0, cs_size = 0, cs_frags = 0, cs_frags_blk = 0;
    std::int32_t contig = 0, maxcontig = 0;
    std::int64_t image_length = 0;
    std::string volume;

    // ---- plan
    struct Extent {
        std::int64_t frag;        // absolute
        std::uint32_t blocks;
        enum Kind : std::uint8_t { file_data, dir_data, pointer } kind;
        std::uint32_t item;       // file/dir item, or pointer block index
        std::uint64_t index;      // first block index within the file/dir
    };
    std::vector<Extent> extents; // ascending frag
    // Pointer blocks: runs of consecutive absolute frags (step fpb).
    std::vector<std::vector<std::pair<std::int64_t, std::uint32_t>>> pointer_blocks;
    std::vector<std::int32_t> high_water;           // per CG, frag offset
    std::vector<std::pair<std::int64_t, std::int32_t>> tail_free; // absolute frag, count
    std::vector<std::uint32_t> inode_item;          // inode - 3 -> item
    std::vector<std::int32_t> dirs_per_cg;
    std::uint32_t next_inode = 3;
    std::int32_t dir_count = 0;
    std::int32_t next_frag_in_cg = 0, current_cg = 0;

    // final metadata
    std::vector<std::uint8_t> superblock;  // 8192
    std::vector<std::uint8_t> cs_summary;  // cs_frags_blk * fsize
    struct CgFinal {
        std::vector<std::uint8_t> header; // the cg block
    };

    // ------------------------------------------------------------ helpers
    std::int32_t inodes_per_group(std::int64_t total_bytes) const {
        std::int64_t total_inodes = total_bytes / options.bytes_per_inode;
        std::int32_t fpg0 = kDefaultInodesPerGroup * fpb;
        std::int64_t frags = total_bytes / fsize;
        std::int32_t groups = static_cast<std::int32_t>((frags + fpg0 - 1) / fpg0);
        if (groups < 1) groups = 1;
        std::int32_t v = static_cast<std::int32_t>(total_inodes / groups);
        std::int32_t inopb = bsize / kInodeSize;
        if (inopb > 0) v = (v + inopb - 1) / inopb * inopb;
        return std::max(v, inopb);
    }

    // CalculateImageSize
    std::int64_t image_size(std::int64_t disk) const {
        std::int64_t size = static_cast<std::int64_t>(static_cast<double>(disk) * (1.0 + 2.0 / 100.0)) + 2 * 1024 * 1024;
        if (size < static_cast<std::int64_t>(bsize) * 16) size = static_cast<std::int64_t>(bsize) * 16;
        return align_up(size, fsize);
    }

    // CalculateImageSizeWithCgOverhead
    std::int64_t image_size_with_cg(std::int64_t disk, std::int64_t entries) const {
        std::int32_t inopb = std::max(1, bsize / kInodeSize);
        std::int32_t sb = static_cast<std::int32_t>(align_up((kSuperblockOffset + kSuperblockSize + fsize - 1) / fsize, fpb));
        std::int32_t cb = sb + static_cast<std::int32_t>(align_up((kSuperblockSize + fsize - 1) / fsize, fpb));
        std::int32_t ib = cb + fpb;
        std::int64_t needed_inodes = entries + 3;
        std::int64_t data = (disk + fsize - 1) / fsize;
        std::int64_t size = std::max(image_size(disk), static_cast<std::int64_t>(bsize) * 16);
        for (int i = 0; i < 16; ++i) {
            std::int32_t g = inodes_per_group(size);
            std::int32_t frags_per_group = g * fpb;
            std::int32_t inodeblks = (g + inopb - 1) / inopb * fpb;
            std::int32_t db = ib + inodeblks;
            std::int32_t data_per_cg = std::max(fpb, frags_per_group - db);
            std::int64_t required = (data * frags_per_group + data_per_cg - 1) / data_per_cg;
            std::int64_t groups = std::max<std::int64_t>(1, (required + frags_per_group - 1) / frags_per_group);
            if (groups * g < needed_inodes) {
                std::int64_t rc = (needed_inodes + g - 1) / g;
                required = std::max(required, rc * frags_per_group);
            }
            std::int64_t req = align_up(required * fsize + bsize, fsize);
            req = align_up(req + static_cast<std::int64_t>(frags_per_group) * fsize, fsize);
            if (req <= size) return size;
            size = req;
        }
        return size;
    }

    // The first CalculateDirectorySizes overload (an estimate of 24 bytes per entry).
    std::int64_t estimate_disk_size() const {
        std::int64_t total = 0;
        std::int64_t ppb = bsize / 8;
        for (std::size_t i = 1; i < items.size(); ++i) {
            const Item& it = items[i];
            if (!it.dir) {
                std::int64_t len = static_cast<std::int64_t>(it.node->size);
                if (len <= 0) continue;
                std::int64_t blocks = (len + bsize - 1) / bsize;
                total += blocks * bsize;
                if (blocks > kNDirect) {
                    total += bsize;
                    std::int64_t rem = blocks - kNDirect;
                    if (rem > ppb) {
                        rem -= ppb;
                        std::int64_t dind = (rem + ppb - 1) / ppb;
                        total += (1 + dind) * bsize;
                        if (rem > ppb * ppb) {
                            rem -= ppb * ppb;
                            std::int64_t l2 = (rem + ppb - 1) / ppb;
                            std::int64_t l1 = (l2 + ppb - 1) / ppb;
                            total += (1 + l1 + l2) * bsize;
                        }
                    }
                }
            } else {
                std::int64_t bytes = static_cast<std::int64_t>(2 + it.children.size()) * 24;
                total += std::max<std::int64_t>(1, (bytes + bsize - 1) / bsize) * bsize;
            }
        }
        std::int64_t root_bytes = static_cast<std::int64_t>(2 + items[0].children.size()) * 24;
        total += std::max<std::int64_t>(1, (root_bytes + bsize - 1) / bsize) * bsize;
        return total;
    }

    // The second overload: FreeBSD ffs_size_dir (exact directory sizes).
    std::int64_t exact_disk_size() const {
        std::int64_t disk = 0;
        std::int64_t nindir = std::max(1, bsize / 8);
        auto indirect_blocks = [&](std::int64_t n) -> std::int64_t {
            if (n <= 0) return 0;
            std::int64_t b = 1;
            if (n > nindir) {
                std::int64_t k2 = n - nindir;
                b += 1 + (k2 + nindir - 1) / nindir;
                if (k2 > nindir * nindir) {
                    std::int64_t k3 = k2 - nindir * nindir;
                    b += 1 + (k3 + nindir * nindir - 1) / (nindir * nindir) + (k3 + nindir - 1) / nindir;
                }
            }
            return b;
        };
        auto add = [&](std::int64_t x) {
            std::int64_t blocks = (x + bsize - 1) / bsize;
            disk += blocks * bsize;
            if (blocks - kNDirect > 0) disk += indirect_blocks(blocks - kNDirect) * bsize;
        };
        auto dirent = [](std::int32_t cur, std::size_t namelen) {
            std::int32_t ent = static_cast<std::int32_t>((8 + namelen + 1 + 3) & ~std::size_t(3));
            std::int32_t up = (cur + kDirBlock - 1) / kDirBlock * kDirBlock;
            if (ent + cur > up) cur = up;
            return cur + ent;
        };
        auto dir_size = [&](const Item& d) {
            std::int32_t cur = dirent(0, 1);
            cur = dirent(cur, 2);
            for (std::uint32_t c : d.children) cur = dirent(cur, items[c].name.size());
            return cur;
        };
        add(dir_size(items[0]));
        for (std::size_t i = 1; i < items.size(); ++i) {
            if (items[i].dir) add(dir_size(items[i]));
            else if (items[i].node->size > 0) add(static_cast<std::int64_t>(items[i].node->size));
        }
        return disk;
    }

    // ------------------------------------------------------------ plan
    Status set_geometry(std::int64_t total_bytes) {
        if (total_bytes < static_cast<std::int64_t>(bsize) * 16) return make_error(Errc::invalid_argument, "image too small");
        if (total_bytes % fsize) return make_error(Errc::invalid_argument, "image size must be a multiple of the fragment size");
        total_frags = total_bytes / fsize;
        ipg = inodes_per_group(total_bytes);
        fpg = ipg * fpb;
        std::int64_t groups = (total_frags + fpg - 1) / fpg;
        if (groups > INT32_MAX) return make_error(Errc::too_large, "too many cylinder groups");
        ncg = std::max<std::int32_t>(1, static_cast<std::int32_t>(groups));
        sblkno = static_cast<std::int32_t>(align_up((kSuperblockOffset + kSuperblockSize + fsize - 1) / fsize, fpb));
        cblkno = sblkno + static_cast<std::int32_t>(align_up((kSuperblockSize + fsize - 1) / fsize, fpb));
        iblkno = cblkno + fpb;
        std::int32_t inopb = bsize / kInodeSize;
        std::int32_t inodeblks = (ipg + inopb - 1) / inopb * fpb;
        dblkno = iblkno + inodeblks;
        maxcontig = std::max(1, kMaxBSize / bsize);
        contig = std::min(maxcontig, kMaxContig);
        std::int32_t raw = kCgHeaderBaseSize + (ipg + 7) / 8 + (fpg + 7) / 8 + 4;
        if (contig > 0) raw += contig * 4 + (fpg / fpb + 7) / 8;
        cg_size = static_cast<std::int32_t>(align_up(raw, fsize));
        cs_size = static_cast<std::int32_t>(align_up(static_cast<std::int64_t>(ncg) * kCsumSize, fsize));
        cs_frags = (cs_size + fsize - 1) / fsize;
        cs_frags_blk = (cs_size + bsize - 1) / bsize * fpb;
        return {};
    }

    std::int32_t usable_in_cg(std::int32_t cg) const {
        return cg == ncg - 1 ? static_cast<std::int32_t>(total_frags - static_cast<std::int64_t>(cg) * fpg) : fpg;
    }

    Result<std::int64_t> allocate_block() {
        auto limit = [&](std::int32_t cg) { return cg == ncg - 1 ? std::min(fpg, usable_in_cg(cg)) : fpg; };
        if (next_frag_in_cg + fpb > limit(current_cg)) {
            ++current_cg;
            if (current_cg >= ncg) return too_small("no more cylinder groups available for data.");
            next_frag_in_cg = dblkno;
            if (next_frag_in_cg + fpb > limit(current_cg)) return too_small("a cylinder group has no room for a block.");
        }
        std::int64_t frag = static_cast<std::int64_t>(current_cg) * fpg + next_frag_in_cg;
        next_frag_in_cg += fpb;
        high_water[current_cg] = next_frag_in_cg;
        return frag;
    }

    void add_extent(std::int64_t frag, Extent::Kind kind, std::uint32_t item, std::uint64_t index) {
        if (!extents.empty()) {
            Extent& last = extents.back();
            if (last.kind == kind && last.item == item && kind != Extent::pointer &&
                last.frag + static_cast<std::int64_t>(last.blocks) * fpb == frag && last.index + last.blocks == index) {
                ++last.blocks;
                return;
            }
        }
        extents.push_back({frag, 1, kind, item, index});
    }

    std::uint32_t new_pointer_block() {
        pointer_blocks.emplace_back();
        return static_cast<std::uint32_t>(pointer_blocks.size() - 1);
    }
    static void add_pointer(std::vector<std::pair<std::int64_t, std::uint32_t>>& runs, std::int64_t frag, std::int32_t fpb) {
        if (!runs.empty() && runs.back().first + static_cast<std::int64_t>(runs.back().second) * fpb == frag)
            ++runs.back().second;
        else
            runs.push_back({frag, 1});
    }

    // AllocateAndWriteSingleIndirectBlock: `count` data blocks behind pointer block `pb`.
    Status single_indirect(std::uint32_t item, std::uint64_t& block_index, std::int64_t count, std::uint32_t pb,
                           std::int64_t& last) {
        for (std::int64_t i = 0; i < count; ++i) {
            ALEMBIC_ASSIGN(std::int64_t frag, allocate_block());
            last = frag;
            add_pointer(pointer_blocks[pb], frag, fpb);
            add_extent(frag, Extent::file_data, item, block_index++);
        }
        return {};
    }

    Status allocate_file(std::uint32_t idx) {
        Item& f = items[idx];
        f.size = static_cast<std::int64_t>(f.node->size);
        if (f.size == 0) return {};
        std::int64_t need = (f.size + bsize - 1) / bsize;
        std::int64_t ppb = bsize / 8;
        std::uint64_t bi = 0;
        std::int64_t last = 0;
        std::int64_t direct = std::min<std::int64_t>(need, kNDirect);
        for (std::int64_t i = 0; i < direct; ++i) {
            ALEMBIC_ASSIGN(std::int64_t frag, allocate_block());
            f.direct[i] = frag;
            last = frag;
            add_extent(frag, Extent::file_data, idx, bi++);
        }
        std::int64_t remaining = need - direct;
        if (remaining > 0) {
            std::int64_t count = std::min(remaining, ppb);
            ALEMBIC_ASSIGN(std::int64_t ind, allocate_block());
            f.indirect[0] = ind;
            ++f.metadata_blocks;
            std::uint32_t pb = new_pointer_block();
            extents.push_back({ind, 1, Extent::pointer, pb, 0});
            ALEMBIC_TRY(single_indirect(idx, bi, count, pb, last));
            remaining -= count;
        }
        if (remaining > 0) {
            ALEMBIC_ASSIGN(std::int64_t dind, allocate_block());
            f.indirect[1] = dind;
            ++f.metadata_blocks;
            std::uint32_t dpb = new_pointer_block();
            extents.push_back({dind, 1, Extent::pointer, dpb, 0});
            std::int64_t second = (remaining + ppb - 1) / ppb;
            for (std::int64_t i = 0; i < second; ++i) {
                std::int64_t count = std::min(remaining, ppb);
                ALEMBIC_ASSIGN(std::int64_t sind, allocate_block());
                ++f.metadata_blocks;
                add_pointer(pointer_blocks[dpb], sind, fpb);
                std::uint32_t pb = new_pointer_block();
                extents.push_back({sind, 1, Extent::pointer, pb, 0});
                ALEMBIC_TRY(single_indirect(idx, bi, count, pb, last));
                remaining -= count;
            }
        }
        if (remaining > 0) {
            ALEMBIC_ASSIGN(std::int64_t tind, allocate_block());
            f.indirect[2] = tind;
            ++f.metadata_blocks;
            std::uint32_t tpb = new_pointer_block();
            extents.push_back({tind, 1, Extent::pointer, tpb, 0});
            std::int64_t per_dind = ppb * ppb;
            std::int64_t third = std::min((remaining + per_dind - 1) / per_dind, ppb);
            for (std::int64_t t = 0; t < third; ++t) {
                ALEMBIC_ASSIGN(std::int64_t dind, allocate_block());
                ++f.metadata_blocks;
                add_pointer(pointer_blocks[tpb], dind, fpb);
                std::uint32_t dpb = new_pointer_block();
                extents.push_back({dind, 1, Extent::pointer, dpb, 0});
                std::int64_t second = std::min((remaining + ppb - 1) / ppb, ppb);
                for (std::int64_t d = 0; d < second; ++d) {
                    std::int64_t count = std::min(remaining, ppb);
                    ALEMBIC_ASSIGN(std::int64_t sind, allocate_block());
                    ++f.metadata_blocks;
                    add_pointer(pointer_blocks[dpb], sind, fpb);
                    std::uint32_t pb = new_pointer_block();
                    extents.push_back({sind, 1, Extent::pointer, pb, 0});
                    ALEMBIC_TRY(single_indirect(idx, bi, count, pb, last));
                    remaining -= count;
                }
            }
        }
        if (remaining > 0) return make_error(Errc::too_large, "a file is too large for UFS2: " + f.name);
        // Unused fragments of the last block of a file without indirect blocks are free.
        if (need <= kNDirect) {
            std::int64_t used = (f.size + fsize - 1) / fsize;
            std::int64_t allocated = need * fpb;
            if (allocated > used) {
                std::int64_t in_last = used % fpb;
                if (in_last == 0) in_last = fpb;
                tail_free.push_back({last + in_last, static_cast<std::int32_t>(fpb - in_last)});
            }
        }
        return {};
    }

    // CalculateDirBlocksNeeded
    std::int32_t dir_blocks_needed(const Item& d) const {
        struct E {
            std::int32_t rec, min;
        };
        std::vector<E> all{{record_length(1), 8 + 1 + 1}, {record_length(2), 8 + 2 + 1}};
        for (std::uint32_t c : d.children) {
            std::size_t n = items[c].name.size();
            all.push_back({record_length(n), static_cast<std::int32_t>(8 + n + 1)});
        }
        std::int32_t blocks = 0;
        std::size_t idx = 0;
        while (idx < all.size()) {
            ++blocks;
            std::int32_t pos = 0;
            while (idx < all.size()) {
                const E& e = all[idx];
                std::int32_t in_chunk = pos % kDirBlock;
                std::int32_t remaining = kDirBlock - in_chunk;
                if (e.min > remaining) {
                    pos += remaining;
                    if (pos >= bsize) break;
                }
                if (pos + e.min > bsize) break;
                bool last = idx == all.size() - 1;
                if (!last) {
                    std::int32_t after = pos + e.rec;
                    std::int32_t next_in = after % kDirBlock;
                    std::int32_t next_rem = kDirBlock - next_in;
                    last = (all[idx + 1].min > next_rem && next_in != 0) || (after + all[idx + 1].min > bsize);
                }
                if (last) pos += kDirBlock - pos % kDirBlock;
                else pos += e.rec;
                ++idx;
                if (last && pos >= bsize) break;
            }
        }
        return std::max(1, blocks);
    }

    // The directory blocks' bytes (WriteDirBlocks).
    std::vector<std::uint8_t> dir_bytes(std::uint32_t idx, std::uint32_t parent_inode) const {
        const Item& d = items[idx];
        struct E {
            std::uint32_t inode;
            std::uint8_t type;
            std::string_view name;
        };
        std::vector<E> all{{d.inode, kDtDir, "."}, {parent_inode, kDtDir, ".."}};
        for (std::uint32_t c : d.children) all.push_back({items[c].inode, items[c].dir ? kDtDir : kDtReg, items[c].name});
        std::size_t nblocks = d.blocks.size();
        std::vector<std::uint8_t> out(nblocks * static_cast<std::size_t>(bsize), 0);
        auto write_entry = [&](std::uint8_t* p, std::uint32_t inode, std::uint16_t rec, std::uint8_t type, std::string_view name) {
            put32(p, inode);
            put16(p + 4, rec);
            p[6] = type;
            p[7] = static_cast<std::uint8_t>(name.size());
            std::memcpy(p + 8, name.data(), std::min<std::size_t>(name.size(), rec - 8u));
        };
        std::size_t ei = 0;
        for (std::size_t b = 0; b < nblocks; ++b) {
            std::uint8_t* block = out.data() + b * static_cast<std::size_t>(bsize);
            std::int32_t pos = 0;
            while (ei < all.size()) {
                const E& e = all[ei];
                std::uint16_t rec = record_length(e.name.size());
                std::int32_t min = static_cast<std::int32_t>(8 + e.name.size() + 1);
                std::int32_t remaining = kDirBlock - pos % kDirBlock;
                if (min > remaining) {
                    if (pos + remaining >= bsize) break;
                    write_entry(block + pos, 0, static_cast<std::uint16_t>(remaining), 0, {});
                    pos += remaining;
                }
                if (pos + min > bsize) break;
                bool last = ei == all.size() - 1;
                if (!last) {
                    std::int32_t next_min = static_cast<std::int32_t>(8 + all[ei + 1].name.size() + 1);
                    std::int32_t after = pos + rec;
                    std::int32_t next_in = after % kDirBlock;
                    std::int32_t next_rem = kDirBlock - next_in;
                    last = (next_min > next_rem && next_in != 0) || (after + next_min > bsize);
                }
                if (last) rec = static_cast<std::uint16_t>(kDirBlock - pos % kDirBlock);
                write_entry(block + pos, e.inode, rec, e.type, e.name);
                pos += rec;
                ++ei;
                if (last && ei < all.size()) {
                    if (pos >= bsize) break;
                } else if (last) {
                    break;
                }
            }
            if (pos % kDirBlock != 0) {
                std::int32_t rem = kDirBlock - pos % kDirBlock;
                write_entry(block + pos, 0, static_cast<std::uint16_t>(rem), 0, {});
                pos += rem;
            }
            while (pos + kDirBlock <= bsize) {
                write_entry(block + pos, 0, static_cast<std::uint16_t>(kDirBlock), 0, {});
                pos += kDirBlock;
            }
        }
        return out;
    }

    // BuildDirectoryAllocation + the directory's inode fields.
    Status allocate_dir_blocks(std::uint32_t idx, std::int32_t count, bool first_already_allocated) {
        Item& d = items[idx];
        for (std::int32_t b = first_already_allocated ? 1 : 0; b < count; ++b) {
            ALEMBIC_ASSIGN(std::int64_t frag, allocate_block());
            d.blocks.push_back(frag);
            add_extent(frag, Extent::dir_data, idx, static_cast<std::uint64_t>(b));
        }
        for (std::size_t i = 0; i < d.blocks.size() && i < static_cast<std::size_t>(kNDirect); ++i) d.direct[i] = d.blocks[i];
        if (d.blocks.size() > static_cast<std::size_t>(kNDirect)) {
            ALEMBIC_ASSIGN(std::int64_t ind, allocate_block());
            d.indirect[0] = ind;
            d.metadata_blocks = 1;
            std::uint32_t pb = new_pointer_block();
            for (std::size_t i = kNDirect; i < d.blocks.size(); ++i) add_pointer(pointer_blocks[pb], d.blocks[i], fpb);
            extents.push_back({ind, 1, Extent::pointer, pb, 0});
        }
        d.size = static_cast<std::int64_t>(count) * bsize;
        std::uint16_t subdirs = 0;
        for (std::uint32_t c : d.children)
            if (items[c].dir) ++subdirs;
        d.nlink = static_cast<std::uint16_t>(2 + subdirs);
        return {};
    }

    // ProcessDirectoryContents
    Status process(std::uint32_t dir_idx, std::uint32_t depth) {
        std::uint64_t max_inodes = static_cast<std::uint64_t>(ncg) * ipg;
        for (std::uint32_t c : std::vector<std::uint32_t>(items[dir_idx].children)) {
            if (next_inode >= max_inodes) return too_small("no more inodes available.");
            Item& child = items[c];
            child.inode = next_inode++;
            inode_item.push_back(c);
            if (child.dir) {
                child.depth = depth + 1;
                ALEMBIC_TRY(process(c, depth + 1));
                ALEMBIC_TRY(allocate_dir_blocks(c, dir_blocks_needed(items[c]), false));
                ++dir_count;
                std::int32_t cg = static_cast<std::int32_t>(items[c].inode / static_cast<std::uint32_t>(ipg));
                if (cg < ncg) ++dirs_per_cg[cg];
            } else {
                ALEMBIC_TRY(allocate_file(c));
            }
        }
        return {};
    }

    Status plan(std::int64_t total_bytes) {
        ALEMBIC_TRY(set_geometry(total_bytes));
        extents.clear();
        pointer_blocks.clear();
        tail_free.clear();
        inode_item.clear();
        high_water.assign(ncg, 0);
        dirs_per_cg.assign(ncg, 0);
        next_inode = 3;
        dir_count = 0;
        current_cg = 0;
        for (Item& it : items) {
            it.blocks.clear();
            std::fill(std::begin(it.direct), std::end(it.direct), 0);
            std::fill(std::begin(it.indirect), std::end(it.indirect), 0);
            it.metadata_blocks = 0;
            it.size = 0;
        }
        // CG 0: the CG summary and the root's first block are taken.
        std::int64_t root_frag = dblkno + cs_frags_blk;
        if (root_frag + fpb > usable_in_cg(0)) return too_small("cylinder group 0 has no room for the root directory.");
        next_frag_in_cg = static_cast<std::int32_t>(root_frag) + fpb;
        high_water[0] = next_frag_in_cg;
        dirs_per_cg[kRootInode / static_cast<std::uint32_t>(ipg)] = 1;
        Item& root = items[0];
        root.inode = kRootInode;
        root.blocks.push_back(root_frag);
        extents.push_back({root_frag, 1, Extent::dir_data, 0, 0});
        ALEMBIC_TRY(process(0, 0));
        ALEMBIC_TRY(allocate_dir_blocks(0, dir_blocks_needed(items[0]), true));
        // Data never exceeds the image end; the file may still grow past it
        // when the last CG is smaller than its own metadata (see layout()).
        return {};
    }

    // ------------------------------------------------------------ metadata
    std::vector<std::uint8_t> make_superblock(std::int64_t free_blocks, std::int64_t free_frags, std::int64_t free_inodes,
                                              std::int64_t dirs) const {
        std::vector<std::uint8_t> b(kSuperblockSize, 0);
        std::uint8_t* p = b.data();
        std::int64_t dsize = total_frags - sblkno - static_cast<std::int64_t>(ncg) * (dblkno - sblkno) - cs_frags;
        std::int32_t bmask = ~(bsize - 1), fmask = ~(fsize - 1);
        std::int64_t maxfile = static_cast<std::int64_t>(bsize) * kNDirect - 1;
        std::int64_t sizepb = bsize;
        for (int i = 0; i < kNIndirect; ++i) {
            sizepb *= bsize / 8;
            maxfile += sizepb;
        }
        std::int32_t fs_id0 = static_cast<std::int32_t>(now & 0xFFFFFFFF);
        std::int32_t fs_id1 = static_cast<std::int32_t>((now >> 16) ^ pid);
        put32(p + 0x008, static_cast<std::uint32_t>(sblkno));
        put32(p + 0x00C, static_cast<std::uint32_t>(cblkno));
        put32(p + 0x010, static_cast<std::uint32_t>(iblkno));
        put32(p + 0x014, static_cast<std::uint32_t>(dblkno));
        put32(p + 0x020, static_cast<std::uint32_t>(now & 0xFFFFFFFF));
        put32(p + 0x024, static_cast<std::uint32_t>(total_frags & 0xFFFFFFFF));
        put32(p + 0x028, static_cast<std::uint32_t>(dsize & 0xFFFFFFFF));
        put32(p + 0x02C, static_cast<std::uint32_t>(ncg));
        put32(p + 0x030, static_cast<std::uint32_t>(bsize));
        put32(p + 0x034, static_cast<std::uint32_t>(fsize));
        put32(p + 0x038, static_cast<std::uint32_t>(fpb));
        put32(p + 0x03C, static_cast<std::uint32_t>(options.min_free_percent));
        put32(p + 0x048, static_cast<std::uint32_t>(bmask));
        put32(p + 0x04C, static_cast<std::uint32_t>(fmask));
        put32(p + 0x050, static_cast<std::uint32_t>(log2i(bsize)));
        put32(p + 0x054, static_cast<std::uint32_t>(log2i(fsize)));
        put32(p + 0x058, static_cast<std::uint32_t>(maxcontig));
        put32(p + 0x05C, static_cast<std::uint32_t>(ipg)); // fs_maxbpg
        put32(p + 0x060, static_cast<std::uint32_t>(log2i(fpb)));
        put32(p + 0x064, static_cast<std::uint32_t>(log2i(fsize / kSector)));
        put32(p + 0x068, kSuperblockSize);
        put32(p + 0x074, static_cast<std::uint32_t>(bsize / 8));
        put32(p + 0x078, static_cast<std::uint32_t>(bsize / kInodeSize));
        put32(p + 0x07C, static_cast<std::uint32_t>(fsize / kSector));
        put32(p + 0x080, 1); // FS_OPTSPACE
        put32(p + 0x090, static_cast<std::uint32_t>(fs_id0));
        put32(p + 0x094, static_cast<std::uint32_t>(fs_id1));
        put32(p + 0x098, static_cast<std::uint32_t>(dblkno)); // fs_old_csaddr
        put32(p + 0x09C, static_cast<std::uint32_t>(cs_size));
        put32(p + 0x0A0, static_cast<std::uint32_t>(cg_size));
        put32(p + 0x0B0, static_cast<std::uint32_t>(ncg));
        put32(p + 0x0B4, 1);
        put32(p + 0x0B8, static_cast<std::uint32_t>(ipg));
        put32(p + 0x0BC, static_cast<std::uint32_t>(fpg));
        put32(p + 0x0C0, static_cast<std::uint32_t>(dirs & 0xFFFFFFFF));
        put32(p + 0x0C4, static_cast<std::uint32_t>(free_blocks & 0xFFFFFFFF));
        put32(p + 0x0C8, static_cast<std::uint32_t>(free_inodes & 0xFFFFFFFF));
        put32(p + 0x0CC, static_cast<std::uint32_t>(free_frags & 0xFFFFFFFF));
        p[0x0D1] = 1;    // fs_clean
        p[0x0D3] = 0x80; // FS_FLAGS_UPDATED
        p[0x0D4] = '/';  // fs_fsmnt
        std::memcpy(p + 0x2A8, volume.data(), std::min<std::size_t>(volume.size(), 31));
        put32(p + 0x35C, static_cast<std::uint32_t>(kMaxBSize));
        put64(p + 0x368, static_cast<std::uint64_t>(total_frags)); // fs_providersize
        put64(p + 0x370, static_cast<std::uint64_t>(static_cast<std::int64_t>(fpg) * options.min_free_percent / 200 / fpb * fpb));
        put64(p + 0x3E0, static_cast<std::uint64_t>(kSuperblockOffset));
        put64(p + 0x3E8, static_cast<std::uint64_t>(kSuperblockOffset));
        put64(p + 0x3F0, static_cast<std::uint64_t>(dirs));
        put64(p + 0x3F8, static_cast<std::uint64_t>(free_blocks));
        put64(p + 0x400, static_cast<std::uint64_t>(free_inodes));
        put64(p + 0x408, static_cast<std::uint64_t>(free_frags));
        put64(p + 0x430, static_cast<std::uint64_t>(now));
        put64(p + 0x438, static_cast<std::uint64_t>(total_frags));
        put64(p + 0x440, static_cast<std::uint64_t>(dsize));
        put64(p + 0x448, static_cast<std::uint64_t>(dblkno));
        put32(p + 0x4AC, kAvgFileSize);
        put32(p + 0x4B0, kAvgFilesPerDir);
        put64(p + 0x4B8, static_cast<std::uint64_t>(now));
        put32(p + 0x524, static_cast<std::uint32_t>(contig));
        put32(p + 0x528, (kNDirect + kNIndirect) * 8);
        put32(p + 0x52C, 2); // FS_44INODEFMT
        put64(p + 0x530, static_cast<std::uint64_t>(maxfile));
        put64(p + 0x538, static_cast<std::uint64_t>(~static_cast<std::int64_t>(bmask)));
        put64(p + 0x540, static_cast<std::uint64_t>(~static_cast<std::int64_t>(fmask)));
        put32(p + 0x54C, 0xFFFFFFFFu); // FS_DYNAMICPOSTBLFMT
        put32(p + 0x550, 1);
        put32(p + 0x55C, static_cast<std::uint32_t>(kUfs2Magic));
        return b;
    }

    // The final free-fragment bitmap of a CG (PatchCgAndSuperblock).
    std::vector<std::uint8_t> frag_bitmap(std::int32_t cg) const {
        std::int32_t usable = usable_in_cg(cg);
        std::vector<std::uint8_t> bm(static_cast<std::size_t>((fpg + 7) / 8), 0);
        auto set = [&](std::int64_t f) {
            if (f >= 0 && f < static_cast<std::int64_t>(bm.size()) * 8) bm[static_cast<std::size_t>(f / 8)] |= static_cast<std::uint8_t>(1u << (f % 8));
        };
        if (cg > 0)
            for (std::int32_t f = 0; f < sblkno && f < usable; ++f) set(f);
        std::int32_t used = high_water[cg] > 0 ? high_water[cg] - dblkno : 0;
        for (std::int32_t f = dblkno + used; f < usable; ++f) set(f);
        if (cg == 0 && cs_frags_blk > cs_frags)
            for (std::int32_t f = dblkno + cs_frags; f < dblkno + cs_frags_blk && f < usable; ++f) set(f);
        std::int64_t start = static_cast<std::int64_t>(cg) * fpg;
        auto it = std::lower_bound(tail_free.begin(), tail_free.end(), std::make_pair(start, INT32_MIN));
        for (; it != tail_free.end() && it->first < start + usable; ++it)
            for (std::int32_t k = 0; k < it->second; ++k)
                if (it->first + k < start + usable) set(it->first + k - start);
        return bm;
    }

    struct CgCounts {
        std::int32_t free_blocks = 0, free_frags = 0, free_inodes = 0, dirs = 0;
    };

    // The final CG header block; fills `counts`.
    std::vector<std::uint8_t> cg_header(std::int32_t cg, CgCounts& counts) const {
        std::int32_t usable = usable_in_cg(cg);
        std::int32_t ibm_bytes = (ipg + 7) / 8, fbm_bytes = (fpg + 7) / 8;
        std::int32_t iused_off = kCgHeaderBaseSize;
        std::int32_t free_off = iused_off + ibm_bytes;
        std::int32_t next_free = free_off + fbm_bytes;
        std::int32_t nclusterblks = 0, clustersum_off = 0, cluster_off = 0;
        if (contig > 0) {
            nclusterblks = usable / fpb;
            clustersum_off = static_cast<std::int32_t>(align_up(free_off + (fpg + 7) / 8, 4)) - 4;
            cluster_off = clustersum_off + (contig + 1) * 4;
            next_free = cluster_off + (fpg / fpb + 7) / 8;
        }
        std::int32_t total = static_cast<std::int32_t>(align_up(next_free, fsize));
        total = std::max(total, static_cast<std::int32_t>(align_up(fsize, kSector)));
        std::vector<std::uint8_t> h(static_cast<std::size_t>(align_up(total, kSector)), 0);
        std::uint8_t* p = h.data();

        // Inodes in use here (they are numbered in order across CGs).
        std::int64_t first = static_cast<std::int64_t>(cg) * ipg;
        std::int32_t used_inodes = cg == 0 ? static_cast<std::int32_t>(std::min<std::int64_t>(next_inode, ipg))
                                   : next_inode > first ? static_cast<std::int32_t>(std::min<std::int64_t>(next_inode - first, ipg))
                                                        : 0;
        std::vector<std::uint8_t> fbm = frag_bitmap(cg);
        auto is_free = [&](std::int32_t f) {
            return f >= 0 && static_cast<std::size_t>(f / 8) < fbm.size() && (fbm[static_cast<std::size_t>(f / 8)] >> (f % 8) & 1);
        };
        std::int32_t blocks_in_cg = usable / fpb;
        std::int32_t free_blocks = 0, free_frags = 0;
        std::int32_t frsum[8] = {};
        for (std::int32_t b = 0; b < blocks_in_cg; ++b) {
            std::int32_t n = 0;
            for (std::int32_t k = 0; k < fpb; ++k) n += is_free(b * fpb + k);
            if (n == fpb) {
                ++free_blocks;
                continue;
            }
            free_frags += n;
            std::int32_t run = 0;
            for (std::int32_t k = 0; k < fpb; ++k) {
                if (is_free(b * fpb + k)) ++run;
                else {
                    if (run > 0 && run < 8) ++frsum[run];
                    run = 0;
                }
            }
            if (run > 0 && run < 8) ++frsum[run];
        }
        {
            std::int32_t run = 0;
            for (std::int32_t f = blocks_in_cg * fpb; f < usable; ++f) {
                if (is_free(f)) {
                    ++free_frags;
                    ++run;
                } else {
                    if (run > 0 && run < 8) ++frsum[run];
                    run = 0;
                }
            }
            if (run > 0 && run < 8) ++frsum[run];
        }
        counts.free_blocks = free_blocks;
        counts.free_frags = free_frags;
        counts.free_inodes = ipg - used_inodes;
        counts.dirs = cg < static_cast<std::int32_t>(dirs_per_cg.size()) ? dirs_per_cg[cg] : 0;

        std::int32_t inopb = bsize / kInodeSize;
        std::int32_t initediblk = std::min(ipg, 2 * inopb);
        if (used_inodes > 0) initediblk = std::min((used_inodes + inopb - 1) / inopb * inopb, ipg);

        put32(p + 0x04, kCgMagic);
        put32(p + 0x08, static_cast<std::uint32_t>(now & 0xFFFFFFFF));
        put32(p + 0x0C, static_cast<std::uint32_t>(cg));
        put32(p + 0x14, static_cast<std::uint32_t>(usable));
        put32(p + 0x18, static_cast<std::uint32_t>(counts.dirs));
        put32(p + 0x1C, static_cast<std::uint32_t>(free_blocks));
        put32(p + 0x20, static_cast<std::uint32_t>(counts.free_inodes));
        put32(p + 0x24, static_cast<std::uint32_t>(free_frags));
        for (int i = 0; i < 8; ++i) put32(p + 0x34 + 4 * i, static_cast<std::uint32_t>(frsum[i]));
        put32(p + 0x5C, static_cast<std::uint32_t>(iused_off));
        put32(p + 0x60, static_cast<std::uint32_t>(free_off));
        put32(p + 0x64, static_cast<std::uint32_t>(next_free));
        put32(p + 0x68, static_cast<std::uint32_t>(clustersum_off));
        put32(p + 0x6C, static_cast<std::uint32_t>(cluster_off));
        put32(p + 0x70, static_cast<std::uint32_t>(nclusterblks));
        put32(p + 0x74, static_cast<std::uint32_t>(ipg));
        put32(p + 0x78, static_cast<std::uint32_t>(initediblk));
        put64(p + 0x88, static_cast<std::uint64_t>(now));
        for (std::int32_t i = 0; i < used_inodes; ++i) p[iused_off + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        std::memcpy(p + free_off, fbm.data(), fbm.size());
        if (contig > 0 && nclusterblks > 0) {
            std::vector<std::uint8_t> cbm(static_cast<std::size_t>((nclusterblks + 7) / 8), 0);
            for (std::int32_t b = 0; b < nclusterblks; ++b) {
                bool all = true;
                for (std::int32_t k = 0; k < fpb && all; ++k) all = b * fpb + k < usable && is_free(b * fpb + k);
                if (all) cbm[static_cast<std::size_t>(b / 8)] |= static_cast<std::uint8_t>(1u << (b % 8));
            }
            std::memcpy(p + cluster_off, cbm.data(), cbm.size());
            std::vector<std::int32_t> sum(static_cast<std::size_t>(contig + 1), 0);
            std::int32_t run = 0;
            for (std::int32_t b = 0; b < nclusterblks; ++b) {
                if (cbm[static_cast<std::size_t>(b / 8)] >> (b % 8) & 1) ++run;
                else if (run) {
                    ++sum[static_cast<std::size_t>(std::min(run, contig))];
                    run = 0;
                }
            }
            if (run) ++sum[static_cast<std::size_t>(std::min(run, contig))];
            for (std::int32_t i = 1; i <= contig; ++i) put32(p + clustersum_off + 4 * i, static_cast<std::uint32_t>(sum[static_cast<std::size_t>(i)]));
        }
        return h;
    }

    void write_inode(std::uint8_t* p, const Item& it, bool root) const {
        std::memset(p, 0, kInodeSize);
        std::int64_t need = it.size > 0 ? (it.size + bsize - 1) / bsize : 0;
        std::int64_t data_frags;
        if (need > kNDirect) {
            data_frags = need * fpb;
        } else {
            data_frags = (it.size > 0 ? it.size / bsize : 0) * fpb;
            std::int64_t tail = it.size > 0 ? it.size % bsize : 0;
            if (tail > 0) data_frags += (tail + fsize - 1) / fsize;
        }
        std::int64_t blocks512 = (data_frags + it.metadata_blocks * fpb) * (fsize / kSector);
        put16(p + 0x00, static_cast<std::uint16_t>((it.dir ? kIfDir : kIfReg) | kPerm));
        put16(p + 0x02, it.dir ? it.nlink : 1);
        put32(p + 0x0C, static_cast<std::uint32_t>(bsize));
        put64(p + 0x10, static_cast<std::uint64_t>(it.size));
        put64(p + 0x18, static_cast<std::uint64_t>(blocks512));
        for (int i = 0; i < 4; ++i) put64(p + 0x20 + 8 * i, static_cast<std::uint64_t>(now));
        put32(p + 0x50, 1); // di_gen
        for (int i = 0; i < kNDirect; ++i) put64(p + 0x70 + 8 * i, static_cast<std::uint64_t>(it.direct[i]));
        for (int i = 0; i < kNIndirect; ++i) put64(p + 0xD0 + 8 * i, static_cast<std::uint64_t>(it.indirect[i]));
        put32(p + 0xF0, it.dir && !root ? it.depth : 0);
    }
};

ImageBuilder::ImageBuilder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ImageBuilder::ImageBuilder(ImageBuilder&&) noexcept = default;
ImageBuilder& ImageBuilder::operator=(ImageBuilder&&) noexcept = default;
ImageBuilder::~ImageBuilder() = default;

std::uint64_t ImageBuilder::image_length() const { return static_cast<std::uint64_t>(impl_->image_length); }
std::uint32_t ImageBuilder::cylinder_groups() const { return static_cast<std::uint32_t>(impl_->ncg); }

namespace {

// Case-insensitive order for ASCII letters, then ordinal (NTFS's order for ASCII names).
bool name_less(const std::string& a, const std::string& b) {
    std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        auto up = [](char c) { return static_cast<unsigned char>(c >= 'a' && c <= 'z' ? c - 32 : c); };
        unsigned char x = up(a[i]), y = up(b[i]);
        if (x != y) return x < y;
    }
    if (a.size() != b.size()) return a.size() < b.size();
    return a < b;
}

std::string sanitize_volume(const std::string& value) {
    std::string out;
    for (char c : value)
        if (c >= ' ' && c <= '~' && out.size() < 31) out += c;
    return out;
}

} // namespace

Result<ImageBuilder> ImageBuilder::from_source(const tree::Source& source, const tree::Node& root, const BuildOptions& options) {
    auto impl = std::make_unique<Impl>();
    Impl& m = *impl;
    m.source = &source;
    m.options = options;
    m.bsize = static_cast<std::int32_t>(options.block_size);
    m.fsize = static_cast<std::int32_t>(options.fragment_size);
    if (m.bsize < 4096 || m.bsize > 65536 || (m.bsize & (m.bsize - 1)) || m.fsize < 512 || (m.fsize & (m.fsize - 1)) ||
        m.fsize > m.bsize || m.bsize / m.fsize > 8)
        return make_error(Errc::invalid_argument, "UFS2 block size must be 4-64 KiB and 1, 2, 4 or 8 fragments");
    if (options.bytes_per_inode <= 0 || options.min_free_percent < 0 || options.min_free_percent > 99)
        return make_error(Errc::invalid_argument, "bad UFS2 inode density or minimum free space");
    m.fpb = m.bsize / m.fsize;
    m.now = options.timestamp ? *options.timestamp
                              : std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    m.pid = options.process_id ? *options.process_id : static_cast<std::int32_t>(::getpid());
    m.volume = sanitize_volume(options.volume_name);
    if (!root.is_directory) return make_error(Errc::invalid_argument, "the UFS2 root must be a folder");

    // The tree, children sorted.
    m.items.push_back({});
    m.items[0].node = &root;
    m.items[0].dir = true;
    std::vector<std::uint32_t> stack{0};
    while (!stack.empty()) {
        std::uint32_t idx = stack.back();
        stack.pop_back();
        const tree::Node* node = m.items[idx].node;
        std::vector<const tree::Node*> kids;
        for (const tree::Node& c : node->children) kids.push_back(&c);
        std::stable_sort(kids.begin(), kids.end(), [](const tree::Node* a, const tree::Node* b) { return name_less(a->name, b->name); });
        std::vector<std::uint32_t> ids;
        for (const tree::Node* c : kids) {
            if (c->name.empty() || c->name.size() > 255 || c->name.find('/') != std::string::npos || c->name == "." || c->name == "..")
                return make_error(Errc::invalid_argument, "a name cannot be stored in UFS2: " + c->name);
            Impl::Item item;
            item.node = c;
            item.dir = c->is_directory;
            item.name = c->name;
            m.items.push_back(std::move(item));
            ids.push_back(static_cast<std::uint32_t>(m.items.size() - 1));
            ++m.total_entries;
        }
        m.items[idx].children = ids;
        for (auto it = ids.rbegin(); it != ids.rend(); ++it)
            if (m.items[*it].dir) stack.push_back(*it);
    }

    // CreateImageFromDirectory's size, then Ufs2Operations' retries.
    std::int64_t disk = m.estimate_disk_size();
    std::int64_t size = std::max(m.image_size(disk), m.image_size_with_cg(disk, static_cast<std::int64_t>(m.total_entries)));
    {
        std::int64_t need = static_cast<std::int64_t>(m.total_entries) + 3;
        std::int32_t g = m.inodes_per_group(size);
        std::int64_t fpg = static_cast<std::int64_t>(g) * m.fpb;
        std::int64_t groups = std::max<std::int64_t>(1, (size / m.fsize + fpg - 1) / fpg);
        if (need > groups * g) {
            std::int64_t bytes = (need + g - 1) / g * fpg * m.fsize;
            if (bytes > size) size = align_up(bytes, m.fsize);
        }
    }
    Status planned = m.plan(size);
    if (!planned.ok() && is_too_small(planned.error())) {
        std::int64_t retry = m.image_size_with_cg(m.exact_disk_size(), static_cast<std::int64_t>(m.total_entries));
        for (int attempt = 0; attempt < 3; ++attempt) {
            retry = align_up(retry * 5 / 4, m.fsize);
            planned = m.plan(retry);
            if (planned.ok() || !is_too_small(planned.error())) break;
        }
    }
    ALEMBIC_TRY(planned);

    // The file grows past fs_size when the last CG is smaller than its own
    // metadata: PS5PKGTool still writes that CG's header and inode table.
    std::int64_t last_start = static_cast<std::int64_t>(m.ncg - 1) * m.fpg * m.fsize;
    std::int64_t table = align_up(static_cast<std::int64_t>(m.ipg) * kInodeSize, kSector);
    m.image_length = std::max(m.total_frags * m.fsize, last_start + static_cast<std::int64_t>(m.iblkno) * m.fsize + table);
    return ImageBuilder(std::move(impl));
}

Status ImageBuilder::write(ByteSink& sink, const ProgressFn& progress) const {
    const Impl& m = *impl_;
    const std::int64_t fsize = m.fsize, bsize = m.bsize;

    // Final counts and the CG headers.
    std::vector<std::vector<std::uint8_t>> headers(static_cast<std::size_t>(m.ncg));
    std::vector<std::uint8_t> cs_summary(static_cast<std::size_t>(m.cs_frags_blk) * static_cast<std::size_t>(fsize), 0);
    std::int64_t free_blocks = 0, free_frags = 0, free_inodes = 0;
    for (std::int32_t cg = 0; cg < m.ncg; ++cg) {
        Impl::CgCounts c;
        headers[static_cast<std::size_t>(cg)] = m.cg_header(cg, c);
        free_blocks += c.free_blocks;
        free_frags += c.free_frags;
        free_inodes += c.free_inodes;
        std::size_t off = static_cast<std::size_t>(cg) * kCsumSize;
        if (off + kCsumSize <= cs_summary.size()) {
            put32(cs_summary.data() + off, static_cast<std::uint32_t>(c.dirs));
            put32(cs_summary.data() + off + 4, static_cast<std::uint32_t>(c.free_blocks));
            put32(cs_summary.data() + off + 8, static_cast<std::uint32_t>(c.free_inodes));
            put32(cs_summary.data() + off + 12, static_cast<std::uint32_t>(c.free_frags));
        }
    }
    std::vector<std::uint8_t> sb = m.make_superblock(free_blocks, free_frags, free_inodes, 1 + m.dir_count);

    // Streams the image through a 1 MiB buffer.
    std::vector<std::uint8_t> buf;
    buf.reserve(1 << 20);
    std::uint64_t written = 0, total = static_cast<std::uint64_t>(m.image_length);
    auto flush = [&]() -> Status {
        if (buf.empty()) return {};
        ALEMBIC_TRY(sink.write(buf));
        written += buf.size();
        buf.clear();
        if (progress && !progress({"Writing UFS2 image", written, total})) return make_error(Errc::cancelled, "cancelled");
        return {};
    };
    std::int64_t pos = 0;
    auto emit = [&](const std::uint8_t* data, std::size_t n) -> Status {
        while (n) {
            std::size_t room = (1u << 20) - buf.size();
            std::size_t k = std::min(room, n);
            buf.insert(buf.end(), data, data + k);
            data += k;
            n -= k;
            pos += static_cast<std::int64_t>(k);
            if (buf.size() == (1u << 20)) ALEMBIC_TRY(flush());
        }
        return {};
    };
    auto zeros_to = [&](std::int64_t target) -> Status {
        static const std::uint8_t zero[65536] = {};
        while (pos < target) ALEMBIC_TRY(emit(zero, static_cast<std::size_t>(std::min<std::int64_t>(sizeof zero, target - pos))));
        return {};
    };
    auto emit_at = [&](std::int64_t at, const std::vector<std::uint8_t>& data) -> Status {
        if (pos > at) return make_error(Errc::invalid_data, "UFS2 layout error: overlapping structures");
        ALEMBIC_TRY(zeros_to(at));
        return emit(data.data(), data.size());
    };

    // Directory parents: each dir item's parent inode.
    std::vector<std::uint32_t> parent(m.items.size(), kRootInode);
    for (std::size_t i = 0; i < m.items.size(); ++i)
        for (std::uint32_t c : m.items[i].children) parent[c] = m.items[i].inode;

    std::size_t ext = 0;
    std::uint32_t cached_dir = UINT32_MAX;
    std::vector<std::uint8_t> dir_cache;
    std::uint32_t open_file = UINT32_MAX;
    std::shared_ptr<const RandomAccess> file;
    std::vector<std::uint8_t> block(static_cast<std::size_t>(bsize));
    std::vector<std::uint8_t> table(static_cast<std::size_t>(align_up(static_cast<std::int64_t>(m.ipg) * kInodeSize, kSector)));

    for (std::int32_t cg = 0; cg < m.ncg; ++cg) {
        std::int64_t start = static_cast<std::int64_t>(cg) * m.fpg * fsize;
        std::int32_t usable = m.usable_in_cg(cg);
        std::int64_t end = start + static_cast<std::int64_t>(usable) * fsize;
        if (cg == 0) {
            // Recovery block (struct fsrecovery) in the sector before the superblock.
            std::vector<std::uint8_t> rec(kSector, 0);
            std::uint8_t* r = rec.data() + kSector - 20;
            put32(r, static_cast<std::uint32_t>(kUfs2Magic));
            put32(r + 4, static_cast<std::uint32_t>(m.fpg));
            put32(r + 8, static_cast<std::uint32_t>(log2i(fsize / kSector)));
            put32(r + 12, static_cast<std::uint32_t>(m.sblkno));
            put32(r + 16, static_cast<std::uint32_t>(m.ncg));
            ALEMBIC_TRY(emit_at(kSuperblockOffset - kSector, rec));
            ALEMBIC_TRY(emit_at(kSuperblockOffset, sb));
        } else {
            std::int64_t backup = start + static_cast<std::int64_t>(m.sblkno) * fsize;
            if (backup + kSuperblockSize <= end) ALEMBIC_TRY(emit_at(backup, sb));
        }
        ALEMBIC_TRY(emit_at(start + static_cast<std::int64_t>(m.cblkno) * fsize, headers[static_cast<std::size_t>(cg)]));

        // Inode table: allocated inodes, otherwise the initialised ones with a
        // random di_gen (the root's slot excepted), as Ufs2ImageCreator does.
        std::fill(table.begin(), table.end(), 0);
        DotNetRandom rng(cg + 1);
        std::int32_t inited = std::min(m.ipg, 2 * static_cast<std::int32_t>(bsize / kInodeSize));
        for (std::int32_t i = 0; i < m.ipg; ++i) {
            std::uint8_t* p = table.data() + static_cast<std::size_t>(i) * kInodeSize;
            std::uint64_t ino = static_cast<std::uint64_t>(cg) * m.ipg + i;
            bool root_slot = cg == 0 && i == static_cast<std::int32_t>(kRootInode);
            if (!root_slot && i < inited) put32(p + 0x50, static_cast<std::uint32_t>(rng.next(1, INT32_MAX)));
            if (root_slot) m.write_inode(p, m.items[0], true);
            else if (ino >= 3 && ino < m.next_inode) m.write_inode(p, m.items[m.inode_item[static_cast<std::size_t>(ino - 3)]], false);
        }
        ALEMBIC_TRY(emit_at(start + static_cast<std::int64_t>(m.iblkno) * fsize, table));

        if (cg == 0) ALEMBIC_TRY(emit_at(static_cast<std::int64_t>(m.dblkno) * fsize, cs_summary));

        // Data blocks of this CG.
        for (; ext < m.extents.size() && m.extents[ext].frag * fsize < end; ++ext) {
            const Impl::Extent& e = m.extents[ext];
            if (pos > e.frag * fsize) return make_error(Errc::invalid_data, "UFS2 layout error: overlapping blocks");
            ALEMBIC_TRY(zeros_to(e.frag * fsize));
            if (e.kind == Impl::Extent::pointer) {
                std::fill(block.begin(), block.end(), 0);
                std::size_t k = 0;
                for (const auto& [first, count] : m.pointer_blocks[e.item])
                    for (std::uint32_t j = 0; j < count; ++j) put64(block.data() + 8 * k++, static_cast<std::uint64_t>(first + static_cast<std::int64_t>(j) * m.fpb));
                ALEMBIC_TRY(emit(block.data(), block.size()));
            } else if (e.kind == Impl::Extent::dir_data) {
                if (cached_dir != e.item) {
                    dir_cache = m.dir_bytes(e.item, e.item == 0 ? kRootInode : parent[e.item]);
                    cached_dir = e.item;
                }
                ALEMBIC_TRY(emit(dir_cache.data() + e.index * static_cast<std::uint64_t>(bsize), static_cast<std::size_t>(e.blocks) * static_cast<std::size_t>(bsize)));
            } else {
                const Impl::Item& f = m.items[e.item];
                if (open_file != e.item) {
                    ALEMBIC_ASSIGN(file, m.source->open(*f.node));
                    open_file = e.item;
                }
                for (std::uint32_t j = 0; j < e.blocks; ++j) {
                    std::uint64_t off = (e.index + j) * static_cast<std::uint64_t>(bsize);
                    std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(bsize), static_cast<std::uint64_t>(f.size) - off));
                    std::fill(block.begin() + static_cast<std::ptrdiff_t>(n), block.end(), 0);
                    ALEMBIC_TRY(file->read_at(off, MutableBytes(block.data(), n)));
                    ALEMBIC_TRY(emit(block.data(), block.size()));
                }
            }
        }
    }
    ALEMBIC_TRY(zeros_to(m.image_length));
    ALEMBIC_TRY(flush());
    return {};
}

Status write_image(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                   const BuildOptions& options, bool overwrite, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(ImageBuilder builder, ImageBuilder::from_source(source, root, options));
    ALEMBIC_ASSIGN(AtomicOutput output, AtomicOutput::create(output_path, overwrite));
    FileSink sink(output.file(), 0);
    ALEMBIC_TRY(builder.write(sink, progress));
    return output.commit();
}

} // namespace alembic::ufs2
