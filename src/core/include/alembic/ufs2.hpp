// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only UFS2 (FreeBSD FFS), the file system inside .ffpkg images.
// On-disk layout follows FreeBSD's sys/ufs/ffs/fs.h and ufs/dinode.h; the
// field offsets and safety checks follow SvenGDK/UFS2Tool as used in
// PS5PKGTool (BSD-2-Clause, see THIRD_PARTY_NOTICES.md).
#pragma once

#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace alembic::ufs2 {

struct Geometry {
    std::uint32_t block_size = 0;    // fs_bsize
    std::uint32_t fragment_size = 0; // fs_fsize
    std::uint32_t cylinder_groups = 0;
    std::uint32_t inodes_per_group = 0;
    std::uint32_t fragments_per_group = 0;
    std::uint64_t total_fragments = 0;
    std::string volume_name;
};

// True if the image has a UFS2 superblock at 64 KiB.
bool looks_like_ufs2(const RandomAccess& image);

// Opens a UFS2 image as a file tree (regular files and folders; symbolic
// links and device nodes are skipped). File times come from the inodes.
Result<std::unique_ptr<tree::Source>> open_image(std::shared_ptr<const RandomAccess> image, std::string label,
                                                 Geometry* geometry = nullptr);

} // namespace alembic::ufs2
