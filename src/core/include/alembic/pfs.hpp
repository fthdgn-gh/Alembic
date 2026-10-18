// SPDX-License-Identifier: GPL-3.0-or-later
// PFS images (.ffpfs): the PlayStation file system holding a game's files
// directly, as MkPFS writes with `pack folder --raw --no-compress` (PS5
// profile, 32-bit inodes, unsigned, unencrypted, case-insensitive). ShadowMount+
// mounts them by extension. An FFPFSC is the same file system holding a single
// compressed image instead (see ffpfsc.hpp).
#pragma once

#include "alembic/ampr.hpp"
#include "alembic/io.hpp"
#include "alembic/result.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace alembic::pfs {

// ----------------------------------------------------------------- reading

// A PFS header (version 1 or 2 and the magic) at offset 0.
bool looks_like_pfs(const RandomAccess& image);

// The game tree under /uroot. Unsigned, unencrypted images with 32-bit inodes
// and contiguous files (what MkPFS and Alembic write); files stored PFSC-
// compressed are decompressed on read.
Result<std::unique_ptr<tree::Source>> open_image(std::shared_ptr<const RandomAccess> image, std::string label);

// ----------------------------------------------------------------- writing

// Builds a PFS image the way MkPFS 1.1.0's build_pfs does for an unsigned,
// uncompressed image, as a forward-only stream: byte-identical to it for the
// same time stamp and block size. See pfs_image.cpp.
struct BuildOptions {
    std::uint32_t block_size = 65536; // a power of two, 4 KiB to 1 MiB
    bool case_insensitive = true;     // header mode bit and path-table hashing
    // Every time stamp in the image; default: now.
    std::optional<std::int64_t> timestamp;
    // Add ampr_emu.index at the root when the game has
    // fakelib/libSceAmpr.sprx and is not LZ4-packed, as MkPFS does by default
    // and as the exFAT writer does (see exfat.hpp).
    bool generate_ampr_index = true;
    ampr::Scheme ampr_scheme = ampr::Scheme::ampr_emu;
};

class ImageBuilder {
public:
    // Plans the image of `root` (a folder node of `source`). Nothing is read or
    // written yet. `source` must outlive the builder. Names must be ASCII.
    static Result<ImageBuilder> from_source(const tree::Source& source, const tree::Node& root,
                                            const BuildOptions& options = {});
    ImageBuilder(ImageBuilder&&) noexcept;
    ImageBuilder& operator=(ImageBuilder&&) noexcept;
    ~ImageBuilder();

    std::uint64_t image_length() const;
    // Streams exactly image_length() bytes into `sink`.
    Status write(ByteSink& sink, const ProgressFn& progress = {}) const;

    struct Impl; // the planned layout (pfs_image.cpp)

private:
    explicit ImageBuilder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

Status write_image(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                   const BuildOptions& options, bool overwrite, const ProgressFn& progress = {});

} // namespace alembic::pfs
