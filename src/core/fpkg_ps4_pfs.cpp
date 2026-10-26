// SPDX-License-Identifier: GPL-3.0-or-later
// PS4 PFS images as maxton/LibOrbisPkg (LGPL-3.0, `6434772`) builds them:
// ported from PFS/PFSBuilder.cs, PFS/PfsStructs.cs, PFS/FSTree.cs,
// PFS/FlatPathTable.cs and PFS/PFSCWriter.cs, quirks included (they are in
// the bytes): the empty unencrypted block after a signed image's flat path
// table, uroot's second size fixed at 64 KiB, folders and files numbered in
// .NET's string order.
//
// LibOrbisPkg writes the whole image into a mapped file, then signs and
// encrypts it in place. Here the layout is planned the same way, then an
// unsigned image is served as a virtual file (the game's files read on
// demand), and a signed one is written with its file blocks signed and
// encrypted on the way; the small metadata blocks are kept in memory and
// signed last, in LibOrbisPkg's order.
#include "fpkg_ps4_internal.hpp"

#include "alembic/bytes.hpp"
#include "alembic/fpkg_ps4.hpp"
#include "alembic/thread_pool.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>

namespace alembic::fpkg::ps4::detail {

namespace {

constexpr std::uint16_t kModeSigned = 0x1, kModeEncrypted = 0x4, kModeAlwaysSet = 0x8;
constexpr std::uint16_t kRxOnly = 0x16D, kTypeDir = 0x4000, kTypeFile = 0x8000;
constexpr std::uint32_t kFlagCompressed = 0x1, kFlagUnk2 = 0x4, kFlagUnk3 = 0x8, kFlagReadOnly = 0x10,
                        kFlagInternal = 0x20000;
constexpr std::uint32_t kDirentFile = 2, kDirentDir = 3, kDirentDot = 4, kDirentDotDot = 5;
constexpr std::uint64_t kD32Size = 0xA8, kS32Size = 0x2C8;
constexpr std::uint64_t kDirentMax = 280;
constexpr std::uint64_t kSector = 0x1000;

std::uint64_t ceil_div(std::uint64_t a, std::uint64_t b) { return a / b + (a % b ? 1 : 0); }

struct Dirent {
    std::uint32_t ino = 0;
    std::uint32_t type = 0;
    std::string name;
    std::uint64_t entsize() const {
        std::uint64_t n = name.size() + 17;
        return n % 8 ? n + 8 - n % 8 : n;
    }
    void write(std::vector<std::uint8_t>& out) const {
        std::size_t at = out.size();
        out.resize(at + entsize(), 0);
        put_u32(&out[at], ino);
        put_u32(&out[at + 4], type);
        put_u32(&out[at + 8], static_cast<std::uint32_t>(name.size()));
        put_u32(&out[at + 12], static_cast<std::uint32_t>(entsize()));
        std::memcpy(&out[at + 16], name.data(), name.size());
    }
};

struct Inode {
    std::uint16_t mode = kRxOnly;
    std::uint16_t nlink = 1;
    std::uint32_t flags = 0;
    std::int64_t size = 0, size_compressed = 0;
    std::uint32_t blocks = 0;
    std::int32_t db[12] = {};
    std::uint32_t number = 0;
};

struct Sig {
    std::uint64_t block;
    std::uint64_t offset; // where the 32-byte HMAC and the block number go
    std::uint64_t size;
};

// A node of LibOrbisPkg's allNodes list.
struct NodeRef {
    bool dir;
    int index;
};

} // namespace

int FsTree::add_dir(int parent, const std::string& name) {
    Dir d;
    d.name = name;
    d.parent = parent;
    dirs.push_back(d);
    int index = static_cast<int>(dirs.size() - 1);
    dirs[static_cast<std::size_t>(parent)].dirs.push_back(index);
    return index;
}

int FsTree::add_file(int parent, File file) {
    file.parent = parent;
    files.push_back(std::move(file));
    int index = static_cast<int>(files.size() - 1);
    dirs[static_cast<std::size_t>(parent)].files.push_back(index);
    return index;
}

std::uint64_t pfsc_header_size(std::uint64_t payload) {
    std::int64_t blocks = static_cast<std::int64_t>(ceil_div(payload, kBlock));
    std::int64_t table = 8 + blocks * 8;
    std::int64_t extra = ((table - 0xFC00) + 0xFFFF) / 0x10000; // C# long division
    return 0x10000 + (extra > 0 ? std::uint64_t(extra) * kBlock : 0);
}

std::vector<std::uint8_t> pfsc_header(std::uint64_t payload) {
    std::uint64_t blocks = ceil_div(payload, kBlock);
    std::uint64_t header = pfsc_header_size(payload);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(header), 0);
    std::memcpy(out.data(), "PFSC", 4);
    put_u32(&out[8], 6);
    put_u32(&out[0x0C], kBlock);
    put_u64(&out[0x10], kBlock);
    put_u64(&out[0x18], 0x400);
    put_u64(&out[0x20], header);
    put_u64(&out[0x28], blocks * kBlock);
    for (std::uint64_t i = 0; i <= blocks; ++i) put_u64(&out[static_cast<std::size_t>(0x400 + i * 8)], header + i * kBlock);
    return out;
}

struct PfsImage::Impl {
    FsTree tree;
    PfsProperties p;
    std::uint64_t bs = kBlock;

    std::vector<Inode> inodes;
    int super_root = 0, fpt = 1, cr = -1, uroot = -1;
    std::vector<int> dir_ino, file_ino; // inode index of each folder / file (-1: not in the image)
    std::vector<std::vector<Dirent>> dirents; // per folder
    std::vector<Dirent> super_root_dirents;
    std::vector<int> all_dirs, all_files; // LibOrbisPkg's allDirs (sorted) and allFiles
    std::vector<NodeRef> all_nodes;       // with the root first

    std::map<std::uint32_t, std::uint32_t> fpt_map;
    std::vector<std::vector<Dirent>> collisions;
    std::uint64_t fpt_size = 0, cr_size = 0;

    // Header.
    std::uint64_t dinode_count = 0, dinode_block_count = 0, ndblock = 0;
    std::uint32_t ibs_blocks = 0, ibs_flags = kFlagReadOnly;
    std::int64_t ibs_size = 0;
    std::int64_t ibs_db[12] = {};
    std::uint64_t empty_block = 4;

    std::vector<Sig> final_sigs, data_sigs;

    // Where each file's data starts (blocks), and its first data signature.
    std::vector<std::uint64_t> file_start;
    std::vector<std::size_t> file_first_sig;

    std::string dir_path(int d) const {
        const FsTree::Dir& dir = tree.dirs[static_cast<std::size_t>(d)];
        return dir.parent < 0 ? std::string() : dir_path(dir.parent) + "/" + dir.name;
    }
    std::string file_path(int f) const {
        const FsTree::File& file = tree.files[static_cast<std::size_t>(f)];
        return dir_path(file.parent) + "/" + file.name;
    }
    std::string path(NodeRef n) const { return n.dir ? dir_path(n.index) : file_path(n.index); }
    int ino_of(NodeRef n) const { return n.dir ? dir_ino[static_cast<std::size_t>(n.index)] : file_ino[static_cast<std::size_t>(n.index)]; }
    std::uint64_t node_size(NodeRef n) const {
        if (!n.dir) return tree.files[static_cast<std::size_t>(n.index)].size;
        std::uint64_t s = 0;
        for (const Dirent& d : dirents[static_cast<std::size_t>(n.index)]) s += d.entsize();
        return s;
    }
    std::uint64_t ino_offset(std::uint32_t number, int db = 0) const {
        return bs + kS32Size * number + 0x64 + 36 * std::uint64_t(db);
    }

    Inode make_inode(std::uint16_t mode, std::uint32_t blocks, std::int64_t size, std::int64_t size_compressed,
                     std::uint16_t nlink, std::uint32_t number, std::uint32_t flags) const {
        Inode i;
        i.mode = mode;
        i.blocks = blocks;
        i.size = size;
        i.size_compressed = size_compressed;
        i.nlink = nlink;
        i.number = number;
        i.flags = p.sign ? flags | kFlagUnk2 | kFlagUnk3 : flags;
        return i;
    }

    // GetAllChildrenDirs / GetAllChildrenFiles.
    void children_dirs(int d, std::vector<int>& out) const {
        const FsTree::Dir& dir = tree.dirs[static_cast<std::size_t>(d)];
        out.insert(out.end(), dir.dirs.begin(), dir.dirs.end());
        for (int c : dir.dirs) children_dirs(c, out);
    }

    static std::uint32_t path_hash(const std::string& path) {
        std::uint32_t hash = 0;
        for (char c : path) {
            unsigned char u = static_cast<unsigned char>(c);
            if (u >= 'a' && u <= 'z') u = static_cast<unsigned char>(u - 32);
            hash = u + 31 * hash;
        }
        return hash;
    }

    Status setup();
    void layout();
    // The metadata blocks (header, inodes, folders, path tables) by block index.
    std::map<std::uint64_t, std::vector<std::uint8_t>> metadata() const;
};

Status PfsImage::Impl::setup() {
    final_sigs.push_back({0, 0x380, 0x5A0});
    dir_ino.assign(tree.dirs.size(), -1);
    file_ino.assign(tree.files.size(), -1);
    dirents.assign(tree.dirs.size(), {});

    // allDirs / allFiles; sce_sys files with an entry name stay out of the image.
    children_dirs(0, all_dirs);
    std::vector<int> dirs_in_order = {0};
    dirs_in_order.insert(dirs_in_order.end(), all_dirs.begin(), all_dirs.end());
    for (int d : dirs_in_order)
        for (int f : tree.dirs[static_cast<std::size_t>(d)].files) {
            const FsTree::File& file = tree.files[static_cast<std::size_t>(f)];
            const FsTree::Dir& parent = tree.dirs[static_cast<std::size_t>(file.parent)];
            if (parent.parent >= 0 && parent.name == "sce_sys" && entry_id_for_name(file.name)) continue;
            all_files.push_back(f);
        }
    for (int d : all_dirs)
        for (char c : tree.dirs[static_cast<std::size_t>(d)].name)
            if (static_cast<unsigned char>(c) >= 0x80) return make_error(Errc::unsupported, "PFS names must be ASCII: " + dir_path(d));
    for (int f : all_files)
        for (char c : tree.files[static_cast<std::size_t>(f)].name)
            if (static_cast<unsigned char>(c) >= 0x80) return make_error(Errc::unsupported, "PFS names must be ASCII: " + file_path(f));

    auto by_path = [&](bool dir) {
        return [this, dir](int a, int b) {
            return unicode::dotnet_compare(dir ? dir_path(a) : file_path(a), dir ? dir_path(b) : file_path(b)) < 0;
        };
    };
    std::vector<int> sorted_dirs = all_dirs, sorted_files = all_files;
    std::stable_sort(sorted_dirs.begin(), sorted_dirs.end(), by_path(true));
    std::stable_sort(sorted_files.begin(), sorted_files.end(), by_path(false));
    for (int d : sorted_dirs) all_nodes.push_back({true, d});
    for (int f : all_files) all_nodes.push_back({false, f});

    bool collision = false;
    {
        std::map<std::uint32_t, int> seen;
        for (NodeRef n : all_nodes)
            if (!seen.emplace(path_hash(path(n)), 0).second) collision = true;
    }
    if (collision && p.sign) return make_error(Errc::unsupported, "a signed PFS with colliding path hashes");

    // SetupRootStructure.
    std::uint32_t number = 0;
    inodes.push_back(make_inode(kTypeDir | kRxOnly, 1, 65536, 65536, 1, number++, kFlagInternal | kFlagReadOnly));
    inodes.push_back(make_inode(kTypeFile | kRxOnly, 1, 0, 0, 1, number++, kFlagInternal | kFlagReadOnly));
    if (collision) {
        cr = static_cast<int>(inodes.size());
        inodes.push_back(make_inode(kTypeFile | kRxOnly, 1, 0, 0, 1, number++, kFlagInternal | kFlagReadOnly));
    }
    Inode uroot_inode = make_inode(kTypeDir | kRxOnly, 1, 65536, 65536, 3, number++, kFlagReadOnly);
    super_root_dirents.push_back({inodes[static_cast<std::size_t>(fpt)].number, kDirentFile, "flat_path_table"});
    if (collision) super_root_dirents.push_back({inodes[static_cast<std::size_t>(cr)].number, kDirentFile, "collision_resolver"});
    super_root_dirents.push_back({uroot_inode.number, kDirentDir, "uroot"});
    dirents[0] = {{uroot_inode.number, kDirentDot, "."}, {uroot_inode.number, kDirentDotDot, ".."}};
    if (p.sign) {
        inodes[static_cast<std::size_t>(super_root)].flags &= ~kFlagReadOnly;
        inodes[static_cast<std::size_t>(fpt)].flags &= ~kFlagReadOnly;
        uroot_inode.flags &= ~kFlagReadOnly;
    }

    // addDirInodes.
    uroot = static_cast<int>(inodes.size());
    inodes.push_back(uroot_inode);
    dir_ino[0] = uroot;
    for (int d : sorted_dirs) {
        std::uint32_t n = static_cast<std::uint32_t>(inodes.size());
        Inode ino = make_inode(kTypeDir | kRxOnly, 1, 65536, 0, 2, n, kFlagReadOnly);
        int parent = tree.dirs[static_cast<std::size_t>(d)].parent;
        dirents[static_cast<std::size_t>(d)].push_back({n, kDirentDot, "."});
        dirents[static_cast<std::size_t>(d)].push_back(
            {inodes[static_cast<std::size_t>(dir_ino[static_cast<std::size_t>(parent)])].number, kDirentDotDot, ".."});
        dirents[static_cast<std::size_t>(parent)].push_back({n, kDirentDir, tree.dirs[static_cast<std::size_t>(d)].name});
        inodes[static_cast<std::size_t>(dir_ino[static_cast<std::size_t>(parent)])].nlink++;
        dir_ino[static_cast<std::size_t>(d)] = static_cast<int>(n);
        inodes.push_back(ino);
    }
    // addFileInodes.
    for (int f : sorted_files) {
        const FsTree::File& file = tree.files[static_cast<std::size_t>(f)];
        std::uint32_t n = static_cast<std::uint32_t>(inodes.size());
        Inode ino = make_inode(kTypeFile | kRxOnly, static_cast<std::uint32_t>(ceil_div(file.size, bs)),
                               static_cast<std::int64_t>(file.size), static_cast<std::int64_t>(file.compressed_size), 1, n,
                               kFlagReadOnly | (file.compress ? kFlagCompressed : 0));
        if (p.sign) ino.flags &= ~kFlagReadOnly;
        file_ino[static_cast<std::size_t>(f)] = static_cast<int>(n);
        dirents[static_cast<std::size_t>(file.parent)].push_back({n, kDirentFile, file.name});
        inodes.push_back(ino);
    }

    // FlatPathTable.Create over allNodes (no root).
    std::map<std::uint32_t, std::vector<NodeRef>> by_hash;
    for (NodeRef n : all_nodes) {
        std::uint32_t h = path_hash(path(n));
        auto it = fpt_map.find(h);
        if (it != fpt_map.end()) {
            it->second = 0x80000000u;
        } else {
            fpt_map[h] = static_cast<std::uint32_t>(ino_of(n)) | (n.dir ? 0x20000000u : 0u);
        }
        by_hash[h].push_back(n);
    }
    if (collision) {
        std::uint32_t offset = 0;
        for (auto& [h, value] : fpt_map) {
            if (value != 0x80000000u) continue;
            value = 0x80000000u | offset;
            std::vector<Dirent> list;
            for (NodeRef n : by_hash[h]) {
                Dirent d{static_cast<std::uint32_t>(ino_of(n)), n.dir ? kDirentDir : kDirentFile, path(n)};
                offset += static_cast<std::uint32_t>(d.entsize());
                list.push_back(d);
            }
            offset += 0x18;
            collisions.push_back(list);
        }
        for (const auto& list : collisions) {
            for (const Dirent& d : list) cr_size += d.entsize();
            cr_size += 0x18;
        }
    }
    fpt_size = fpt_map.size() * 8;

    all_nodes.insert(all_nodes.begin(), NodeRef{true, 0});
    layout();
    return {};
}

void PfsImage::Impl::layout() {
    file_start.assign(tree.files.size(), 0);
    file_first_sig.assign(tree.files.size(), 0);
    Inode& sr = inodes[static_cast<std::size_t>(super_root)];
    Inode& fp = inodes[static_cast<std::size_t>(fpt)];
    if (p.sign) {
        ndblock = 1;
        std::uint64_t per_block = bs / kS32Size;
        dinode_count = inodes.size();
        dinode_block_count = ceil_div(dinode_count, per_block);
        ibs_blocks = static_cast<std::uint32_t>(dinode_block_count);
        ibs_size = static_cast<std::int64_t>(dinode_block_count * bs);
        ibs_flags = 0;
        for (std::uint64_t i = 0; i < dinode_block_count; ++i) {
            if (i < 12) ibs_db[i] = static_cast<std::int64_t>(1 + i);
            final_sigs.push_back({1 + i, 0xB8 + 36 * i, bs});
        }
        ndblock += dinode_block_count;
        sr.db[0] = static_cast<std::int32_t>(dinode_block_count + 1);
        final_sigs.push_back({std::uint64_t(sr.db[0]), ino_offset(sr.number), bs});
        ndblock += sr.blocks;

        fp.db[0] = sr.db[0] + 1;
        fp.size = fp.size_compressed = static_cast<std::int64_t>(fpt_size);
        fp.blocks = static_cast<std::uint32_t>(ceil_div(fpt_size, bs));
        final_sigs.push_back({std::uint64_t(fp.db[0]), ino_offset(fp.number), bs});
        for (std::uint32_t i = 1; i < fp.blocks && i < 12; ++i) {
            fp.db[i] = static_cast<std::int32_t>(ndblock++);
            final_sigs.push_back({std::uint64_t(fp.db[0]), ino_offset(fp.number, static_cast<int>(i)), bs});
        }
        ndblock++;           // an empty block follows the flat path table
        empty_block = ndblock; // and is not encrypted
        ndblock++;

        std::uint64_t ib_start = ndblock;
        std::uint64_t spb = bs / 36;
        auto indirect_blocks = [&](std::uint64_t size) {
            std::uint64_t blocks = ceil_div(size, bs), ib = 0;
            if (blocks > 12) {
                blocks -= 12;
                ib++;
            }
            if (blocks > spb) {
                blocks -= spb;
                ib += 1 + ceil_div(blocks, spb);
            }
            return ib;
        };
        for (NodeRef n : all_nodes) ndblock += indirect_blocks(node_size(n));

        for (NodeRef n : all_nodes) {
            Inode& ino = inodes[static_cast<std::size_t>(ino_of(n))];
            std::uint64_t size = node_size(n);
            std::uint64_t blocks = ceil_div(size, bs);
            ino.db[0] = static_cast<std::int32_t>(ndblock);
            if (!n.dir) {
                file_start[static_cast<std::size_t>(n.index)] = ndblock;
                file_first_sig[static_cast<std::size_t>(n.index)] = data_sigs.size();
            }
            ino.blocks = static_cast<std::uint32_t>(blocks);
            ino.size = static_cast<std::int64_t>(n.dir ? ceil_div(size, bs) * bs : size);
            if (ino.size_compressed == 0) ino.size_compressed = ino.size;
            for (std::uint64_t i = 0; i < blocks && i < 12; ++i)
                data_sigs.push_back({ndblock++, ino_offset(ino.number, static_cast<int>(i)), bs});
            if (blocks > 12) {
                final_sigs.push_back({ib_start, ino_offset(ino.number, 12), bs});
                for (std::uint64_t i = 12, pointer = 0; i < blocks && i < 12 + spb; ++i, pointer += 36)
                    data_sigs.push_back({ndblock++, ib_start * bs + pointer, bs});
                ib_start++;
            }
            if (blocks > 12 + spb) {
                std::uint64_t done = 12 + spb;
                final_sigs.push_back({ib_start, ino_offset(ino.number, 13), bs});
                std::uint64_t ib1 = ib_start;
                for (std::uint64_t i = 0; i < spb && done < blocks; ++i) {
                    final_sigs.push_back({++ib_start, ib1 * bs + i * 36, bs});
                    for (std::uint64_t j = 0; j < spb && done < blocks; ++j, ++done)
                        data_sigs.push_back({ndblock++, ib_start * bs + j * 36, bs});
                }
            }
        }
    } else {
        ndblock = 1;
        std::uint64_t per_block = bs / kD32Size;
        dinode_count = inodes.size();
        dinode_block_count = ceil_div(dinode_count, per_block);
        ibs_blocks = static_cast<std::uint32_t>(dinode_block_count);
        ibs_size = static_cast<std::int64_t>(dinode_block_count * bs);
        ibs_db[0] = static_cast<std::int64_t>(ndblock++);
        for (std::uint64_t i = 1; i < dinode_block_count; ++i) {
            if (i < 12) ibs_db[i] = -1;
            ndblock++;
        }
        sr.db[0] = static_cast<std::int32_t>(ndblock);
        ndblock += sr.blocks;

        fp.db[0] = static_cast<std::int32_t>(ndblock++);
        fp.size = fp.size_compressed = static_cast<std::int64_t>(fpt_size);
        fp.blocks = static_cast<std::uint32_t>(ceil_div(fpt_size, bs));
        for (std::uint32_t i = 1; i < fp.blocks && i < 12; ++i) fp.db[i] = static_cast<std::int32_t>(ndblock++);
        if (cr < 0) {
            ndblock++;
        } else {
            Inode& c = inodes[static_cast<std::size_t>(cr)];
            c.db[0] = static_cast<std::int32_t>(ndblock++);
            c.size = c.size_compressed = static_cast<std::int64_t>(cr_size);
            c.blocks = static_cast<std::uint32_t>(ceil_div(cr_size, bs));
            for (std::uint32_t i = 1; i < c.blocks && i < 12; ++i) c.db[i] = static_cast<std::int32_t>(ndblock++);
        }
        for (NodeRef n : all_nodes) {
            Inode& ino = inodes[static_cast<std::size_t>(ino_of(n))];
            std::uint64_t size = node_size(n);
            std::uint64_t blocks = ceil_div(size, bs);
            ino.db[0] = static_cast<std::int32_t>(ndblock);
            if (!n.dir) file_start[static_cast<std::size_t>(n.index)] = ndblock;
            ino.blocks = static_cast<std::uint32_t>(blocks);
            ino.size = static_cast<std::int64_t>(n.dir ? ceil_div(size, bs) * bs : size);
            if (ino.size_compressed == 0) ino.size_compressed = ino.size;
            for (std::uint64_t i = 1; i < blocks && i < 12; ++i) ino.db[i] = -1;
            ndblock += blocks;
        }
    }
    ndblock = std::max<std::uint64_t>(ndblock, p.min_blocks);
}

std::map<std::uint64_t, std::vector<std::uint8_t>> PfsImage::Impl::metadata() const {
    std::map<std::uint64_t, std::vector<std::uint8_t>> blocks;
    auto put = [&](std::uint64_t offset, const std::uint8_t* data, std::size_t n) {
        while (n) {
            std::uint64_t b = offset / bs, within = offset % bs;
            auto& block = blocks[b];
            if (block.empty()) block.assign(static_cast<std::size_t>(bs), 0);
            std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(n, bs - within));
            std::memcpy(block.data() + within, data, take);
            data += take;
            offset += take;
            n -= take;
        }
    };
    auto touch = [&](std::uint64_t b) {
        auto& block = blocks[b];
        if (block.empty()) block.assign(static_cast<std::size_t>(bs), 0);
    };

    // The superblock.
    {
        std::vector<std::uint8_t> h(0x5A0, 0);
        put_u64(&h[0x00], 1);
        put_u64(&h[0x08], 20130315);
        h[0x1A] = 1; // read-only
        put_u16(&h[0x1C], static_cast<std::uint16_t>((p.sign ? kModeSigned : 0) | (p.encrypt ? kModeEncrypted : 0) | kModeAlwaysSet));
        put_u32(&h[0x20], static_cast<std::uint32_t>(bs));
        put_u64(&h[0x28], 1);
        put_u64(&h[0x30], dinode_count);
        put_u64(&h[0x38], ndblock);
        put_u64(&h[0x40], dinode_block_count);
        std::uint8_t* s = &h[0x50]; // the inode blocks' signature inode (S64)
        put_u16(s + 0x00, 0);
        put_u16(s + 0x02, 1);
        put_u32(s + 0x04, ibs_flags);
        put_u64(s + 0x08, static_cast<std::uint64_t>(ibs_size));
        put_u64(s + 0x10, static_cast<std::uint64_t>(ibs_size));
        for (int t = 0; t < 4; ++t) put_u64(s + 0x18 + 8 * t, static_cast<std::uint64_t>(p.time));
        put_u32(s + 0x60, ibs_blocks);
        for (int i = 0; i < 12; ++i) put_u64(s + 0x68 + 40 * i + 32, static_cast<std::uint64_t>(ibs_db[i]));
        if (p.sign || p.encrypt) {
            put_u32(&h[0x36C], 1);
            std::memcpy(&h[0x370], p.seed.data(), 16);
        } else {
            put_u32(&h[0x368], 1);
        }
        put(0, h.data(), h.size());
    }

    // The inodes, from block 1; an inode never straddles two blocks.
    {
        std::uint64_t size = p.sign ? kS32Size : kD32Size;
        std::uint64_t at = bs;
        for (const Inode& i : inodes) {
            std::vector<std::uint8_t> e(static_cast<std::size_t>(size), 0);
            put_u16(&e[0], i.mode);
            put_u16(&e[2], i.nlink);
            put_u32(&e[4], i.flags);
            put_u64(&e[8], static_cast<std::uint64_t>(i.size));
            put_u64(&e[0x10], static_cast<std::uint64_t>(i.size_compressed));
            for (int t = 0; t < 4; ++t) put_u64(&e[static_cast<std::size_t>(0x18 + 8 * t)], static_cast<std::uint64_t>(p.time));
            put_u32(&e[0x60], i.blocks);
            for (int k = 0; k < 12; ++k) {
                std::size_t slot = p.sign ? 0x64 + 36 * k + 32 : 0x64 + 4 * k;
                put_u32(&e[slot], static_cast<std::uint32_t>(i.db[k]));
            }
            put(at, e.data(), e.size());
            at += size;
            if (at % bs > bs - size) at += bs - at % bs;
        }
        for (std::uint64_t b = 0; b < dinode_block_count; ++b) touch(1 + b);
    }

    // The super root's entries.
    {
        std::vector<std::uint8_t> d;
        for (const Dirent& e : super_root_dirents) e.write(d);
        put(bs * (dinode_block_count + 1), d.data(), d.size());
        touch(dinode_block_count + 1);
    }

    // The flat path table, the collision resolver, then every folder.
    {
        const Inode& fp = inodes[static_cast<std::size_t>(fpt)];
        std::vector<std::uint8_t> t;
        for (const auto& [h, v] : fpt_map) {
            std::uint8_t row[8];
            put_u32(row, h);
            put_u32(row + 4, v);
            t.insert(t.end(), row, row + 8);
        }
        put(std::uint64_t(fp.db[0]) * bs, t.data(), t.size());
        touch(static_cast<std::uint64_t>(fp.db[0]));
        if (cr >= 0) {
            const Inode& c = inodes[static_cast<std::size_t>(cr)];
            std::vector<std::uint8_t> r;
            for (const auto& list : collisions) {
                for (const Dirent& e : list) e.write(r);
                r.resize(r.size() + 0x18, 0);
            }
            put(std::uint64_t(c.db[0]) * bs, r.data(), r.size());
        }
    }
    for (NodeRef n : all_nodes) {
        if (!n.dir) continue;
        const Inode& ino = inodes[static_cast<std::size_t>(ino_of(n))];
        std::uint64_t block = static_cast<std::uint64_t>(ino.db[0]);
        std::uint64_t at = block * bs;
        touch(block);
        for (const Dirent& e : dirents[static_cast<std::size_t>(n.index)]) {
            std::vector<std::uint8_t> d;
            e.write(d);
            put(at, d.data(), d.size());
            at += d.size();
            if (at % bs > bs - kDirentMax) at = ++block * bs;
        }
    }
    return blocks;
}

namespace {

// An unsigned image as a file: metadata from memory, file data read on demand.
class ImageFile final : public RandomAccess {
public:
    ImageFile(const PfsImage::Impl& impl, std::map<std::uint64_t, std::vector<std::uint8_t>> meta)
        : impl_(impl), meta_(std::move(meta)) {
        for (std::size_t f = 0; f < impl.tree.files.size(); ++f)
            if (impl.file_ino[f] >= 0 && impl.tree.files[f].size)
                extents_.push_back({impl.file_start[f], static_cast<int>(f)});
        std::sort(extents_.begin(), extents_.end());
    }
    Result<std::uint64_t> size() const override { return impl_.ndblock * impl_.bs; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        const std::uint64_t bs = impl_.bs;
        if (offset > impl_.ndblock * bs || out.size() > impl_.ndblock * bs - offset)
            return make_error(Errc::invalid_argument, "read past the end of a PFS image");
        std::size_t done = 0;
        while (done < out.size()) {
            std::uint64_t at = offset + done;
            std::uint64_t block = at / bs;
            std::uint64_t within = at % bs;
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size() - done, bs - within));
            MutableBytes part = out.subspan(done, n);
            auto m = meta_.find(block);
            if (m != meta_.end()) {
                std::memcpy(part.data(), m->second.data() + within, n);
            } else {
                ALEMBIC_TRY(read_file_part(block, within, part));
            }
            done += n;
        }
        return {};
    }

private:
    Status read_file_part(std::uint64_t block, std::uint64_t within, MutableBytes part) const {
        std::memset(part.data(), 0, part.size());
        auto it = std::upper_bound(extents_.begin(), extents_.end(), std::make_pair(block, INT32_MAX));
        if (it == extents_.begin()) return {};
        --it;
        const FsTree::File& file = impl_.tree.files[static_cast<std::size_t>(it->second)];
        std::uint64_t pos = (block - it->first) * impl_.bs + within;
        if (pos >= file.size) return {};
        if (cached_ != it->second) {
            ALEMBIC_ASSIGN(handle_, file.open());
            cached_ = it->second;
        }
        std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(part.size(), file.size - pos));
        return handle_->read_at(pos, part.first(n));
    }

    const PfsImage::Impl& impl_;
    std::map<std::uint64_t, std::vector<std::uint8_t>> meta_;
    std::vector<std::pair<std::uint64_t, int>> extents_; // start block, file
    mutable int cached_ = -1;
    mutable std::shared_ptr<const RandomAccess> handle_;
};

// Waits for a batch of jobs.
class Latch {
public:
    explicit Latch(std::size_t n) : left_(n) {}
    void done() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (--left_ == 0) cv_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&] { return left_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t left_;
};

} // namespace

PfsImage::PfsImage(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PfsImage::~PfsImage() = default;

Result<std::shared_ptr<PfsImage>> PfsImage::plan(FsTree tree, const PfsProperties& properties) {
    auto impl = std::make_unique<Impl>();
    impl->tree = std::move(tree);
    impl->p = properties;
    ALEMBIC_TRY(impl->setup());
    return std::shared_ptr<PfsImage>(new PfsImage(std::move(impl)));
}

std::uint64_t PfsImage::size() const { return impl_->ndblock * impl_->bs; }

std::size_t PfsImage::file_count() const { return impl_->all_files.size(); }

Result<std::shared_ptr<const RandomAccess>> PfsImage::open() const {
    if (impl_->p.sign || impl_->p.encrypt) return make_error(Errc::invalid_argument, "a signed PFS is written, not read");
    return std::shared_ptr<const RandomAccess>(std::make_shared<ImageFile>(*impl_, impl_->metadata()));
}

Status PfsImage::write(File& out, std::uint64_t offset, unsigned threads,
                       const std::function<bool(std::uint64_t)>& progress) const {
    const Impl& m = *impl_;
    const std::uint64_t bs = m.bs;
    auto meta = m.metadata();
    // ib blocks are metadata too (they get the data blocks' signatures).
    for (const Sig& s : m.final_sigs) {
        auto& block = meta[s.block];
        if (block.empty()) block.assign(static_cast<std::size_t>(bs), 0);
    }
    std::uint8_t message[20] = {0};
    std::memcpy(message + 4, m.p.seed.data(), 16);
    message[0] = 2;
    crypto::Digest sign_key = crypto::hmac_sha256(Bytes(m.p.ekpfs.data(), 32), Bytes(message, 20));
    message[0] = 1;
    crypto::Digest enc = crypto::hmac_sha256(Bytes(m.p.ekpfs.data(), 32), Bytes(message, 20));
    crypto::Aes128 tweak_key(enc.data()), data_key(enc.data() + 16);

    auto put_sig = [&](const Sig& s, const crypto::Digest& d) {
        std::uint8_t entry[36];
        std::memcpy(entry, d.data(), 32);
        put_u32(entry + 32, static_cast<std::uint32_t>(s.block));
        std::uint64_t b = s.offset / bs, within = s.offset % bs;
        std::memcpy(meta[b].data() + within, entry, 36);
    };
    auto encrypt_block = [&](std::uint64_t block, std::uint8_t* data) {
        if (!m.p.encrypt || block == 0 || block == m.empty_block) return;
        for (std::uint64_t s = 0; s < bs / kSector; ++s)
            crypto::xts_encrypt(data_key, tweak_key, block * (bs / kSector) + s, MutableBytes(data + s * kSector, kSector));
    };

    // Data blocks: folders from memory, files read, signed and encrypted on workers.
    for (const Sig& s : m.data_sigs) {
        auto it = meta.find(s.block);
        if (it != meta.end()) put_sig(s, crypto::hmac_sha256(Bytes(sign_key.data(), 32), Bytes(it->second.data(), bs)));
    }
    Pool pool(threads);
    const std::size_t batch = std::max<std::size_t>(8, pool.size() * 4);
    std::vector<std::vector<std::uint8_t>> buffers(batch, std::vector<std::uint8_t>(static_cast<std::size_t>(bs)));
    std::vector<crypto::Digest> digests(batch);
    std::uint64_t done_bytes = 0;
    for (std::size_t f = 0; f < m.tree.files.size(); ++f) {
        if (m.file_ino[f] < 0) continue;
        const FsTree::File& file = m.tree.files[f];
        std::uint64_t blocks = ceil_div(file.size, bs);
        if (!blocks) continue;
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> source, file.open());
        for (std::uint64_t first = 0; first < blocks; first += batch) {
            std::size_t count = static_cast<std::size_t>(std::min<std::uint64_t>(batch, blocks - first));
            for (std::size_t k = 0; k < count; ++k) {
                std::uint64_t pos = (first + k) * bs;
                std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(bs, file.size - pos));
                std::fill(buffers[k].begin() + static_cast<std::ptrdiff_t>(n), buffers[k].end(), 0);
                ALEMBIC_TRY(source->read_at(pos, MutableBytes(buffers[k].data(), n)));
            }
            Latch latch(count);
            for (std::size_t k = 0; k < count; ++k) {
                std::uint64_t block = m.file_start[f] + first + k;
                pool.submit([&, k, block](unsigned) {
                    digests[k] = crypto::hmac_sha256(Bytes(sign_key.data(), 32), Bytes(buffers[k].data(), bs));
                    encrypt_block(block, buffers[k].data());
                    latch.done();
                });
            }
            latch.wait();
            for (std::size_t k = 0; k < count; ++k) {
                std::uint64_t block = m.file_start[f] + first + k;
                const Sig& sig = m.data_sigs[m.file_first_sig[f] + first + k];
                if (sig.block != block) return make_error(Errc::invalid_data, "a PFS data block has the wrong signature slot");
                put_sig(sig, digests[k]);
                ALEMBIC_TRY(out.write_at(offset + block * bs, Bytes(buffers[k].data(), bs)));
            }
            done_bytes += std::min<std::uint64_t>(file.size - first * bs, std::uint64_t(count) * bs);
            if (progress && !progress(done_bytes)) return make_error(Errc::cancelled, "cancelled");
        }
    }

    // The rest of the signatures, last pushed first (indirect blocks, then
    // the folders' and inodes' blocks, the superblock last).
    for (auto it = m.final_sigs.rbegin(); it != m.final_sigs.rend(); ++it) {
        const auto& block = meta[it->block];
        put_sig(*it, crypto::hmac_sha256(Bytes(sign_key.data(), 32), Bytes(block.data(), static_cast<std::size_t>(it->size))));
    }

    // Metadata blocks, and zero blocks nothing covers (encrypted like the rest).
    std::vector<bool> written(static_cast<std::size_t>(m.ndblock), false);
    for (std::size_t f = 0; f < m.tree.files.size(); ++f)
        if (m.file_ino[f] >= 0)
            for (std::uint64_t b = 0; b < ceil_div(m.tree.files[f].size, bs); ++b) written[static_cast<std::size_t>(m.file_start[f] + b)] = true;
    for (auto& [block, data] : meta) {
        if (block >= m.ndblock) return make_error(Errc::invalid_data, "PFS metadata lies past the image");
        encrypt_block(block, data.data());
        ALEMBIC_TRY(out.write_at(offset + block * bs, Bytes(data.data(), data.size())));
        written[static_cast<std::size_t>(block)] = true;
    }
    if (m.p.encrypt) {
        std::vector<std::uint8_t> zero(static_cast<std::size_t>(bs));
        for (std::uint64_t b = 1; b < m.ndblock; ++b) {
            if (written[static_cast<std::size_t>(b)] || b == m.empty_block) continue;
            std::fill(zero.begin(), zero.end(), 0);
            encrypt_block(b, zero.data());
            ALEMBIC_TRY(out.write_at(offset + b * bs, Bytes(zero.data(), zero.size())));
        }
    }
    return {};
}

} // namespace alembic::fpkg::ps4::detail
