// SPDX-License-Identifier: GPL-3.0-or-later
// UFS2 reader. Layout: FreeBSD sys/ufs/ffs/fs.h, ufs/dinode.h, ufs/dir.h.
// Offsets and validation follow SvenGDK/UFS2Tool (BSD-2-Clause) as shipped in
// PS5PKGTool's PS5PKGTool.Ufs2 (Ufs2Superblock.cs, Ufs2Image.cs, Ufs2Volume.cs).
#include "alembic/ufs2.hpp"

#include "alembic/bytes.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <set>
#include <vector>

namespace alembic::ufs2 {

namespace {

constexpr std::uint64_t kSuperblockOffset = 65536;
constexpr std::size_t kSuperblockSize = 8192;
constexpr std::uint32_t kUfs2Magic = 0x19540119;
constexpr std::uint32_t kUfs1Magic = 0x00011954;
constexpr std::uint32_t kInodeSize = 256;
constexpr std::uint32_t kRootInode = 2;
constexpr int kDirect = 12;
constexpr std::size_t kMaxEntries = 2'000'000;
constexpr int kMaxDepth = 256;
constexpr std::uint64_t kMaxDirectoryBytes = 512ull * 1024 * 1024;

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

struct Inode {
    std::uint16_t mode = 0;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::int64_t direct[kDirect] = {};
    std::int64_t indirect[3] = {};
    bool is_directory() const { return (mode & 0xF000) == 0x4000; }
    bool is_regular() const { return (mode & 0xF000) == 0x8000; }
};

struct Volume {
    std::shared_ptr<const RandomAccess> image;
    std::uint64_t image_size = 0;
    Geometry g;
    std::uint32_t iblkno = 0; // inode table offset within a cylinder group, in fragments

    Result<Inode> read_inode(std::uint32_t number) const {
        std::uint64_t total = std::uint64_t(g.cylinder_groups) * g.inodes_per_group;
        if (number >= total) return invalid("a UFS2 inode number is out of range");
        std::uint64_t cg = number / g.inodes_per_group;
        std::uint64_t index = number % g.inodes_per_group;
        std::uint64_t offset = (cg * g.fragments_per_group + iblkno) * g.fragment_size + index * kInodeSize;
        if (offset > image_size || kInodeSize > image_size - offset) return invalid("a UFS2 inode lies outside the image");
        std::uint8_t d[kInodeSize];
        ALEMBIC_TRY(image->read_at(offset, MutableBytes(d, sizeof d)));
        Inode inode;
        inode.mode = get_u16(d);
        inode.size = get_u64(d + 0x10);
        inode.mtime = static_cast<std::int64_t>(get_u64(d + 0x28));
        for (int i = 0; i < kDirect; ++i) inode.direct[i] = static_cast<std::int64_t>(get_u64(d + 0x70 + i * 8));
        for (int i = 0; i < 3; ++i) inode.indirect[i] = static_cast<std::int64_t>(get_u64(d + 0xD0 + i * 8));
        if (inode.size > INT64_MAX) return invalid("a UFS2 inode has an invalid size");
        return inode;
    }
};

// A file's data, resolving block pointers (direct, then single, double and
// triple indirect) on demand. Pointer 0 is a hole and reads as zeros.
class UfsFile final : public RandomAccess {
public:
    UfsFile(std::shared_ptr<const Volume> volume, Inode inode) : v_(std::move(volume)), inode_(inode) {}

    Result<std::uint64_t> size() const override { return inode_.size; }

    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > inode_.size || out.size() > inode_.size - offset) return invalid("read past the end of a UFS2 file");
        const std::uint32_t bsize = v_->g.block_size;
        std::size_t done = 0;
        while (done < out.size()) {
            std::uint64_t at = offset + done;
            std::uint64_t logical = at / bsize;
            std::size_t within = static_cast<std::size_t>(at % bsize);
            std::size_t n = std::min<std::size_t>(out.size() - done, bsize - within);
            ALEMBIC_ASSIGN(std::int64_t fragment, resolve(logical));
            if (fragment == 0) {
                std::memset(out.data() + done, 0, n);
            } else {
                std::uint64_t image_offset = std::uint64_t(fragment) * v_->g.fragment_size + within;
                if (fragment < 0 || image_offset > v_->image_size || n > v_->image_size - image_offset)
                    return invalid("a UFS2 data pointer leaves the image");
                ALEMBIC_TRY(v_->image->read_at(image_offset, out.subspan(done, n)));
            }
            done += n;
        }
        return {};
    }

private:
    Result<std::int64_t> pointer(std::int64_t fragment, std::uint64_t index) const {
        if (fragment == 0) return std::int64_t(0);
        const std::uint32_t bsize = v_->g.block_size;
        if (index >= bsize / 8) return invalid("a UFS2 indirect index is invalid");
        auto hit = std::find_if(cache_.begin(), cache_.end(), [&](const Cached& c) { return c.fragment == fragment; });
        if (hit == cache_.end()) {
            std::uint64_t offset = std::uint64_t(fragment) * v_->g.fragment_size;
            if (fragment < 0 || offset > v_->image_size || bsize > v_->image_size - offset)
                return invalid("a UFS2 indirect block leaves the image");
            if (cache_.size() >= 8) cache_.erase(cache_.begin());
            cache_.push_back({fragment, std::vector<std::uint8_t>(bsize)});
            ALEMBIC_TRY(v_->image->read_at(offset, cache_.back().data));
            hit = cache_.end() - 1;
        }
        return static_cast<std::int64_t>(get_u64(hit->data.data() + index * 8));
    }

    Result<std::int64_t> resolve(std::uint64_t logical) const {
        if (logical < kDirect) return inode_.direct[logical];
        std::uint64_t index = logical - kDirect;
        const std::uint64_t per = v_->g.block_size / 8;
        if (index < per) return pointer(inode_.indirect[0], index);
        index -= per;
        if (index < per * per) {
            ALEMBIC_ASSIGN(std::int64_t first, pointer(inode_.indirect[1], index / per));
            return pointer(first, index % per);
        }
        index -= per * per;
        if (index >= per * per * per) return invalid("a UFS2 file exceeds triple-indirect addressing");
        ALEMBIC_ASSIGN(std::int64_t first, pointer(inode_.indirect[2], index / (per * per)));
        std::uint64_t rest = index % (per * per);
        ALEMBIC_ASSIGN(std::int64_t second, pointer(first, rest / per));
        return pointer(second, rest % per);
    }

    struct Cached {
        std::int64_t fragment;
        std::vector<std::uint8_t> data;
    };
    std::shared_ptr<const Volume> v_;
    Inode inode_;
    mutable std::vector<Cached> cache_;
};

class Ufs2Source final : public tree::Source {
public:
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return label_; }

    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory || file.id > UINT32_MAX) return make_error(Errc::invalid_argument, "not a UFS2 file");
        ALEMBIC_ASSIGN(Inode inode, volume_->read_inode(static_cast<std::uint32_t>(file.id)));
        if (!inode.is_regular() || inode.size != file.size) return invalid("a UFS2 file changed while open");
        return std::shared_ptr<const RandomAccess>(std::make_shared<UfsFile>(volume_, inode));
    }

    Status load_directory(tree::Node& node, std::uint32_t number, std::set<std::uint32_t>& visited, int depth) {
        if (depth > kMaxDepth) return invalid("the UFS2 folder depth exceeds the safety limit");
        if (!visited.insert(number).second) return invalid("the UFS2 folder graph contains a cycle");
        ALEMBIC_ASSIGN(Inode dir, volume_->read_inode(number));
        if (!dir.is_directory()) return invalid("a UFS2 folder entry points to a non-folder inode");
        if (dir.size > kMaxDirectoryBytes) return invalid("a UFS2 folder is unreasonably large");
        std::vector<std::uint8_t> data(static_cast<std::size_t>(dir.size));
        UfsFile reader(volume_, dir);
        ALEMBIC_TRY(reader.read_at(0, data));

        std::size_t offset = 0;
        while (offset + 8 <= data.size()) {
            std::uint32_t ino = get_u32(&data[offset]);
            std::uint16_t record = get_u16(&data[offset + 4]);
            std::uint8_t name_length = data[offset + 7];
            if (record < 8 || record > data.size() - offset || std::size_t(8) + name_length > record)
                return invalid("a UFS2 folder record is malformed");
            std::string name(reinterpret_cast<const char*>(&data[offset + 8]), name_length);
            offset += record;
            if (ino == 0 || name == "." || name == "..") continue; // free slot
            if (name.empty() || name.find('/') != std::string::npos || name.find('\0') != std::string::npos)
                return invalid("a UFS2 folder contains an unsafe name");
            ALEMBIC_ASSIGN(Inode child, volume_->read_inode(ino));
            if (!child.is_directory() && !child.is_regular()) continue; // symlinks, devices, FIFOs
            if (++entries_ > kMaxEntries) return invalid("the UFS2 entry count exceeds the safety limit");
            tree::Node n;
            n.name = name;
            n.is_directory = child.is_directory();
            n.size = n.is_directory ? 0 : child.size;
            n.mtime = child.mtime;
            n.id = ino;
            if (n.is_directory) ALEMBIC_TRY(load_directory(n, ino, visited, depth + 1));
            node.children.push_back(std::move(n));
        }
        return {};
    }

    std::shared_ptr<Volume> volume_;
    tree::Node root_;
    std::string label_;
    std::size_t entries_ = 0;
};

} // namespace

bool looks_like_ufs2(const RandomAccess& image) {
    auto size = image.size();
    if (!size.ok() || *size < kSuperblockOffset + kSuperblockSize) return false;
    std::uint8_t magic[4];
    if (!image.read_at(kSuperblockOffset + 0x55C, MutableBytes(magic, 4)).ok()) return false;
    return get_u32(magic) == kUfs2Magic;
}

Result<std::unique_ptr<tree::Source>> open_image(std::shared_ptr<const RandomAccess> image, std::string label,
                                                 Geometry* geometry) {
    auto volume = std::make_shared<Volume>();
    volume->image = std::move(image);
    ALEMBIC_ASSIGN(volume->image_size, volume->image->size());
    if (volume->image_size < kSuperblockOffset + kSuperblockSize) return invalid("the image is too small for UFS2");
    std::vector<std::uint8_t> sb(kSuperblockSize);
    ALEMBIC_TRY(volume->image->read_at(kSuperblockOffset, sb));
    std::uint32_t magic = get_u32(&sb[0x55C]);
    if (magic == kUfs1Magic) return make_error(Errc::unsupported, "UFS1 images are not supported");
    if (magic != kUfs2Magic) return invalid("the image has no UFS2 superblock");

    Geometry& g = volume->g;
    volume->iblkno = get_u32(&sb[0x10]);
    g.cylinder_groups = get_u32(&sb[0x2C]);
    g.block_size = get_u32(&sb[0x30]);
    g.fragment_size = get_u32(&sb[0x34]);
    std::uint32_t frags_per_block = get_u32(&sb[0x38]);
    g.inodes_per_group = get_u32(&sb[0xB8]);
    g.fragments_per_group = get_u32(&sb[0xBC]);
    g.total_fragments = get_u64(&sb[0x438]);
    std::size_t name_length = 0;
    while (name_length < 32 && sb[0x2A8 + name_length] != 0) ++name_length;
    g.volume_name.assign(reinterpret_cast<const char*>(&sb[0x2A8]), name_length);

    auto power_of_two = [](std::uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };
    if (g.block_size < 4096 || g.block_size > 65536 || !power_of_two(g.block_size) || g.fragment_size < 512 ||
        g.fragment_size > g.block_size || !power_of_two(g.fragment_size) ||
        frags_per_block != g.block_size / g.fragment_size || g.cylinder_groups == 0 || g.inodes_per_group == 0 ||
        g.fragments_per_group == 0 || g.total_fragments == 0 ||
        g.total_fragments > volume->image_size / g.fragment_size || volume->iblkno >= g.fragments_per_group)
        return invalid("the UFS2 superblock geometry is invalid");

    auto source = std::make_unique<Ufs2Source>();
    source->volume_ = volume;
    source->label_ = std::move(label);
    source->root_.is_directory = true;
    std::set<std::uint32_t> visited;
    ALEMBIC_TRY(source->load_directory(source->root_, kRootInode, visited, 0));
    if (geometry) *geometry = g;
    return std::unique_ptr<tree::Source>(std::move(source));
}

} // namespace alembic::ufs2
