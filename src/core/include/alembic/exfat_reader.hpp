// SPDX-License-Identifier: GPL-3.0-or-later
// Read-only exFAT, enough for PS5 dump images. Ported from PS5PKGTool's
// ExfatVolume.cs (same validation rules and safety limits).
#pragma once

#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace alembic::exfat {

struct Geometry {
    std::uint32_t sector_size = 0;
    std::uint32_t cluster_size = 0;
    std::uint32_t cluster_count = 0;
    std::uint32_t root_cluster = 0;
    std::uint64_t fat_offset = 0;
    std::uint64_t heap_offset = 0;
};

// True if the boot sector says exFAT.
bool looks_like_exfat(const RandomAccess& image);

// Opens an image as a file tree. `label` names the tree (usually the image
// file name). The image must stay unchanged while the source is in use.
Result<std::unique_ptr<tree::Source>> open_image(std::shared_ptr<const RandomAccess> image, std::string label,
                                                 Geometry* geometry = nullptr);

} // namespace alembic::exfat
