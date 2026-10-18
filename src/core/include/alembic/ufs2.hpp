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
#include <optional>
#include <string>

namespace alembic::resume {
struct Session;
}

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

// ---------------------------------------------------------------- writing

// Builds a UFS2 (.ffpkg) image the way PS5PKGTool does (Ufs2Operations.
// CreateFromDirectoryAsync over SvenGDK/UFS2Tool's Ufs2ImageCreator), as a
// forward-only stream: byte-identical to it for the same time stamp, process
// ID and (case-insensitive) directory order. See ufs2_image.cpp.
struct BuildOptions {
    std::uint32_t block_size = 32768;
    std::uint32_t fragment_size = 4096;
    std::int64_t bytes_per_inode = 262144;
    int min_free_percent = 0;
    // fs_volname: printable ASCII, at most 31 characters (PS5PKGTool uses the
    // title ID for a dump).
    std::string volume_name;
    // Every time stamp in the image and fs_id[0]; default: now.
    std::optional<std::int64_t> timestamp;
    // Mixed into fs_id[1] as PS5PKGTool does with its process ID; default: ours.
    std::optional<std::int32_t> process_id;
};

class ImageBuilder {
public:
    // Plans the image of `root` (a folder node of `source`). Nothing is read or
    // written yet. `source` must outlive the builder.
    static Result<ImageBuilder> from_source(const tree::Source& source, const tree::Node& root,
                                            const BuildOptions& options = {});
    ImageBuilder(ImageBuilder&&) noexcept;
    ImageBuilder& operator=(ImageBuilder&&) noexcept;
    ~ImageBuilder();

    std::uint64_t image_length() const;
    std::uint32_t cylinder_groups() const;
    // Streams exactly image_length() bytes into `sink`.
    // With `start`, only the bytes from that offset on are sent (to continue
    // an interrupted write); file blocks wholly before it are not read.
    Status write(ByteSink& sink, const ProgressFn& progress = {}, std::uint64_t start = 0) const;

private:
    struct Impl;
    explicit ImageBuilder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// With a session, it writes into the session's partial file and can
// continue from its checkpoint, which keeps the time stamp and process ID.
Status write_image(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                   const BuildOptions& options, bool overwrite, const ProgressFn& progress = {},
                   resume::Session* session = nullptr);

} // namespace alembic::ufs2
