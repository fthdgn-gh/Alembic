// SPDX-License-Identifier: GPL-3.0-or-later
// Between the PS4 package writer's layers (fpkg_ps4_pfs.cpp, fpkg_ps4_build.cpp).
#pragma once

#include "alembic/crypto.hpp"
#include "alembic/io.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alembic::fpkg::ps4::detail {

inline constexpr std::uint32_t kBlock = 0x10000;

using Opener = std::function<Result<std::shared_ptr<const RandomAccess>>()>;

// LibOrbisPkg's FSDir / FSFile tree: each folder keeps its sub-folders and
// files in two lists, in the order they were added.
struct FsTree {
    struct Dir {
        std::string name;
        int parent = -1; // -1 for the root (dirs[0])
        std::vector<int> dirs, files;
    };
    struct File {
        std::string name;
        int parent = 0;
        std::uint64_t size = 0;
        std::uint64_t compressed_size = 0; // the inode's second size
        bool compress = false;             // the inode's compressed flag
        Opener open;
    };
    std::vector<Dir> dirs{Dir{}};
    std::vector<File> files;

    int add_dir(int parent, const std::string& name);
    int add_file(int parent, File file);
};

struct PfsProperties {
    bool sign = false;
    bool encrypt = false;
    std::uint32_t min_blocks = 0;
    std::int64_t time = 0;
    crypto::Digest ekpfs{};
    std::array<std::uint8_t, 16> seed{};
};

// A planned PFS image (PfsBuilder up to CalculatePfsSize).
class PfsImage {
public:
    static Result<std::shared_ptr<PfsImage>> plan(FsTree tree, const PfsProperties& properties);
    ~PfsImage();
    std::uint64_t size() const;
    std::size_t file_count() const;
    // An unsigned, unencrypted image as a file; read from one thread at a time.
    Result<std::shared_ptr<const RandomAccess>> open() const;
    // A signed (and encrypted) image written at `offset` of `out`; `progress`
    // gets the bytes of file data done so far.
    Status write(File& out, std::uint64_t offset, unsigned threads, const std::function<bool(std::uint64_t)>& progress) const;

    struct Impl;

private:
    explicit PfsImage(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// LibOrbisPkg's PFSCWriter: a version-6 PFSC header whose block table points
// at the payload stored raw; the header length is a multiple of 64 KiB.
std::uint64_t pfsc_header_size(std::uint64_t payload);
std::vector<std::uint8_t> pfsc_header(std::uint64_t payload);

} // namespace alembic::fpkg::ps4::detail
