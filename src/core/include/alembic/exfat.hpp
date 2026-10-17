// SPDX-License-Identifier: GPL-3.0-or-later
// Builds an exFAT image from a dump folder as a forward-only byte stream, so
// it can go straight into a file or into the PFSC encoder without a temporary
// copy. The layout is deterministic and mirrors MkPFS; it is ported from
// PS5PKGTool's ExfatImage.cs and must stay byte-identical to it (with
// ampr::Scheme::ps5pkgtool; see ampr.hpp).
#pragma once

#include "alembic/ampr.hpp"
#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace alembic::exfat {

struct BuildOptions {
    // Power of two from 4 KiB to 32 MiB. Default: 64 KiB when the average file
    // is at least 1 MiB, otherwise 32 KiB.
    std::optional<std::uint32_t> cluster_size;
    // Add ampr_emu.index when the dump has fakelib/libSceAmpr.sprx (an
    // LZ4-packed tree keeps the index it has).
    bool generate_ampr_index = true;
    // Hashing rules for that index. ampr_emu is what the console reads;
    // ps5pkgtool reproduces PS5PKGTool byte for byte (differs for non-ASCII names).
    ampr::Scheme ampr_scheme = ampr::Scheme::ampr_emu;
};

class ImageBuilder {
public:
    // Plans an image of `root` (a folder node of `source`). Nothing is read or
    // written yet. `source` must outlive the builder.
    static Result<ImageBuilder> from_source(const tree::Source& source, const tree::Node& root,
                                            const BuildOptions& options = {});
    // Same, for a folder on disk.
    static Result<ImageBuilder> scan(const std::string& source_directory, const BuildOptions& options = {});

    ImageBuilder(ImageBuilder&&) noexcept;
    ImageBuilder& operator=(ImageBuilder&&) noexcept;
    ~ImageBuilder();

    std::uint64_t image_length() const;
    std::uint32_t cluster_size() const;
    std::uint32_t file_count() const;
    std::uint32_t directory_count() const; // includes the root

    // Streams exactly image_length() bytes into `sink`. Source files are read
    // during this call and must not change in between.
    Status write(ByteSink& sink, const ProgressFn& progress = {}) const;

private:
    struct Impl;
    explicit ImageBuilder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Scans `source_directory` and writes the image to a new file at `output_path`.
Status write_directory_image(const std::string& source_directory, const std::string& output_path,
                             const BuildOptions& options = {}, const ProgressFn& progress = {});

// Writes an image of `root` to a new file.
Status write_image(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                   const BuildOptions& options = {}, bool overwrite = false, const ProgressFn& progress = {});

// Builds ampr_emu.index for a tree with the same rules as the image builder,
// whether or not it has fakelib/libSceAmpr.sprx.
Result<std::vector<std::uint8_t>> build_ampr_index(const tree::Source& source, const tree::Node& root,
                                                   ampr::Scheme scheme = ampr::Scheme::ampr_emu);
Result<std::vector<std::uint8_t>> build_ampr_index(const std::string& source_directory,
                                                   ampr::Scheme scheme = ampr::Scheme::ampr_emu);

} // namespace alembic::exfat
