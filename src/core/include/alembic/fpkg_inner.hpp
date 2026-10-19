// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 debug packages, read side, second layer: the inner image.
//
// pfs_image.dat is a raw stream of stored or header-stripped Kraken blocks
// (up to 256 KiB each). Their geometry lives only in naps_pkg_layout.dat:
// a bit-packed table of CblockInfo records. Decoding it gives a list of
// blocks that rebuild the "mount", an uncompressed PFS image whose file tree
// is the game. Several writers lay the table out differently ("dialects");
// like ProsperoPkgTool, we try each and use the one that fits.
// Ported from pearlxcore/ProsperoPkgTool (MIT): ProsperoNapsLayout.cs,
// ProsperoPs5InnerImageReader.cs, NapsDialectRegistry.cs,
// NapsIndexedLogicalMap.cs, NapsSparseFidxDialect.cs,
// ProsperoInnerMountStream.cs, ProsperoInnerPfsReader.cs.
#pragma once

#include "alembic/fpkg.hpp"
#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alembic {
class Pool; // thread_pool.hpp
}

namespace alembic::fpkg {

struct NapsCounts {
    int num_files = 0; // fidx entries, including the trailing mount-size sentinel
    int compression_type = 0;
    int num_keys = 0;
    int num_shuffle_patterns = 0;
    int num_ublocks = 0;
    int num_outer_blocks = 0;
    int num_cblock_info = 0;
    int num_u2c() const { return (num_ublocks + 8) >> 3; }
};

struct CblockInfo {
    std::uint8_t raw[9] = {};
    bool is_run = false;
    // STD records
    std::uint32_t coff_start_mod = 0; // compressed offset, mod 256 KiB
    std::uint32_t uoff_start = 0;     // uncompressed offset, mod 256 KiB
    std::uint32_t clen_even_minus1 = 0;
    std::uint8_t even = 0, odd = 0, kde = 0, shuffle = 0;
    // RUN records
    std::uint32_t coff_end_mod = 0;
    std::uint32_t tweak_start = 0;
    std::uint8_t key_table = 0;
    std::uint32_t coff_start_256k = 0;
};

struct NapsLayout {
    NapsCounts counts;
    struct FileOffset {
        std::uint8_t type;
        std::uint64_t offset;
    };
    std::vector<FileOffset> fidx;
    struct U2c {
        std::uint32_t base;
        std::uint8_t delta[7];
    };
    std::vector<U2c> u2c;
    std::vector<CblockInfo> cblocks;
};

Result<NapsLayout> parse_naps(const std::vector<std::uint8_t>& blob);

struct InnerBlock {
    std::uint64_t compressed_offset = 0; // in pfs_image.dat
    std::uint32_t compressed_length = 0;
    std::uint32_t even_length = 0;       // first sub-chunk's compressed length
    std::uint64_t offset = 0;            // in the mount
    std::uint32_t length = 0;            // decoded length (<= 256 KiB)
    bool kraken = false;
    bool zero_hole = false;
    bool alias = false;   // repeats an earlier block (dedup)
    std::int64_t source = -1;
    int run = -1;
    int kraken_flags = 0;
};

struct BlockPlan {
    std::string dialect;
    std::vector<InnerBlock> blocks;
    std::vector<std::string> diagnostics;
    std::uint64_t mount_size = 0; // 64 KiB aligned
};

// Called with a candidate plan when dialects disagree; should fail if the
// plan does not produce a readable inner PFS.
using PlanProbe = std::function<Status(const std::vector<InnerBlock>&)>;

Result<BlockPlan> plan_blocks(const std::vector<std::uint8_t>& naps, std::uint64_t inner_image_length,
                              const PlanProbe& probe);

// Random access to the reconstructed mount, decoding blocks on demand. When
// reads move forward block by block, the next Kraken blocks are decoded ahead
// on worker threads (their compressed bytes are still read on the calling
// thread, so the image underneath need not be thread-safe). Like the other
// readers, one InnerMount is used from one thread at a time.
class InnerMount final : public RandomAccess {
public:
    InnerMount(std::shared_ptr<const RandomAccess> inner_image, std::vector<InnerBlock> blocks, std::uint64_t mount_size);
    ~InnerMount() override;
    // Decoding threads for the read-ahead; 0 = every core, 1 = none.
    void set_threads(unsigned threads) { threads_ = threads; }
    Result<std::uint64_t> size() const override { return mount_size_; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override;
    // Decodes one block into `out` (which must hold its length).
    Status decode_block(std::size_t index, MutableBytes out, int depth = 0) const;
    const std::vector<InnerBlock>& blocks() const { return blocks_; }

private:
    std::shared_ptr<const RandomAccess> image_;
    std::vector<InnerBlock> blocks_;
    std::uint64_t mount_size_;
    std::uint64_t image_size_ = 0;
    mutable std::size_t cached_ = SIZE_MAX;
    mutable std::vector<std::uint8_t> cache_;

    // Read-ahead: blocks being decoded, in index order.
    struct Ahead;
    Status take_ahead(std::size_t index) const; // into cache_; drops older ones
    Status fill_ahead(std::size_t after) const;  // queue Kraken blocks after `after`
    unsigned threads_ = 0;
    mutable std::size_t last_ = SIZE_MAX;       // the block read last
    mutable std::size_t queued_up_to_ = 0;      // blocks below this were considered
    mutable std::deque<std::shared_ptr<Ahead>> ahead_;
    mutable std::unique_ptr<alembic::Pool> pool_; // last: joined first
};

struct InnerEntry {
    std::string path; // "/uroot/..."
    bool is_directory = false;
    std::uint64_t offset = 0;
    std::uint64_t size = 0;
    std::uint32_t inode = 0;
};

// Lists the inner PFS (files first, then sub-folders, at each level, like the
// reference reader). `superblock_hint` is a mount offset, or -1 to scan.
Result<std::vector<InnerEntry>> list_inner_pfs(const RandomAccess& mount, std::int64_t superblock_hint = -1);

struct OpenedPackage {
    Package package;
    std::shared_ptr<OuterPfs> outer;
    BlockPlan plan;
    std::shared_ptr<InnerMount> mount;
    std::vector<InnerEntry> entries;
};

// Opens a finalized debug package down to its inner file list.
Result<OpenedPackage> open_package(std::shared_ptr<const RandomAccess> file, const std::string& passcode = kDefaultPasscode);

// The package as a file tree: the inner PFS's uroot, plus sce_sys files that
// the package keeps as CNT entries (param.json, images, trophies, ...).
Result<std::unique_ptr<tree::Source>> open_tree(std::shared_ptr<const RandomAccess> file, std::string label,
                                                const std::string& passcode = kDefaultPasscode);

} // namespace alembic::fpkg
