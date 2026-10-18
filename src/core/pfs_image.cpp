// SPDX-License-Identifier: GPL-3.0-or-later
// PFS images (.ffpfs). The writer is ported from PSBrew/MkPFS (GPL-3.0)
// mkpfs/pfs.py build_pfs, unsigned and uncompressed path, with its helpers
// (scan_source_tree, make_fpt_and_collision_blob, _pack_pfs_header_block,
// build_inode_block_sig_s64, Inode.to_bytes, Dirent.to_bytes). MkPFS writes
// the image with seeks; here the layout is planned first and the image is
// streamed in block order, so any tree converts without a temporary copy.
#include "alembic/pfs.hpp"

#include "alembic/bytes.hpp"
#include "alembic/exfat.hpp"
#include "alembic/unicode.hpp"
#include "alembic/pfsc.hpp"
#include "alembic/resume.hpp"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace alembic::pfs {

namespace {

constexpr std::uint64_t kMagic = 20130315;
constexpr std::uint64_t kVersionPs5 = 2;
constexpr std::uint16_t kModeSigned = 0x1;
constexpr std::uint16_t kMode64BitInodes = 0x2;
constexpr std::uint16_t kModeEncrypted = 0x4;
constexpr std::uint16_t kModeCaseInsensitive = 0x8;

constexpr std::uint32_t kInodeSize = 0xA8; // D32: 12 direct + 5 indirect 32-bit pointers
constexpr std::uint16_t kRxOnly = 0x16D;
constexpr std::uint16_t kTypeDirectory = 0x4000;
constexpr std::uint16_t kTypeFile = 0x8000;
constexpr std::uint32_t kFlagCompressed = 0x1;
constexpr std::uint32_t kFlagReadOnly = 0x10;
constexpr std::uint32_t kFlagInternal = 0x20000;

constexpr std::uint32_t kDirentFile = 2;
constexpr std::uint32_t kDirentDirectory = 3;
constexpr std::uint32_t kDirentDot = 4;
constexpr std::uint32_t kDirentDotDot = 5;

constexpr std::uint32_t kFptDirectoryBit = 0x20000000;
constexpr std::uint32_t kFptCollisionBit = 0x80000000;
constexpr std::uint64_t kInt32Max = 0x7FFFFFFF;

Error invalid(const std::string& message) { return make_error(Errc::invalid_data, message); }

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

// Java-style hash over the path (ASCII upper-cased when case-insensitive), as
// MkPFS's fpt_hash.
std::uint32_t fpt_hash(const std::string& path, bool case_insensitive) {
    std::uint32_t h = 0;
    for (char c : path) {
        auto u = static_cast<std::uint32_t>(static_cast<unsigned char>(c));
        if (case_insensitive && c >= 'a' && c <= 'z') u -= 32;
        h = u + 31u * h;
    }
    return h;
}

void append_dirent(std::vector<std::uint8_t>& out, std::uint32_t inode, std::uint32_t type, const std::string& name) {
    std::size_t size = align_up(name.size() + 17, 8);
    std::size_t at = out.size();
    out.resize(at + size, 0);
    put_u32(&out[at], inode);
    put_u32(&out[at + 4], type);
    put_u32(&out[at + 8], static_cast<std::uint32_t>(name.size()));
    put_u32(&out[at + 12], static_cast<std::uint32_t>(size));
    std::memcpy(&out[at + 16], name.data(), name.size());
}

struct InodeRecord {
    std::uint16_t mode = 0;
    std::uint16_t nlink = 0;
    std::uint32_t flags = 0;
    std::uint64_t size = 0;
    std::uint64_t size_compressed = 0;
    std::uint32_t blocks = 0;
    std::int32_t first_block = 0;
    std::int32_t other_direct = -1; // db[1..11]; the super root keeps 0
};

void put_inode(std::uint8_t* d, const InodeRecord& r, std::int64_t time) {
    put_u16(d + 0x00, r.mode);
    put_u16(d + 0x02, r.nlink);
    put_u32(d + 0x04, r.flags);
    put_u64(d + 0x08, r.size);
    put_u64(d + 0x10, r.size_compressed);
    for (int i = 0; i < 4; ++i) put_u64(d + 0x18 + i * 8, static_cast<std::uint64_t>(time));
    // 0x38 nanoseconds, 0x48 uid, 0x4C gid, 0x50/0x58 unknown: all zero.
    put_u32(d + 0x60, r.blocks);
    put_u32(d + 0x64, static_cast<std::uint32_t>(r.first_block));
    for (int i = 1; i < 12; ++i) put_u32(d + 0x64 + i * 4, static_cast<std::uint32_t>(r.other_direct));
    // 0x94: five indirect pointers, zero.
}

} // namespace

// ---------------------------------------------------------------- writing

struct ImageBuilder::Impl {
    struct Dir {
        std::string rel; // "" for the root, else "a/b"
        std::string name;
        int parent = -1;
        std::vector<int> child_dirs, child_files;
        InodeRecord inode;
        std::uint32_t number = 0;
        std::vector<std::uint8_t> blob;
    };
    struct File {
        std::string rel;
        std::string name;
        const tree::Node* node = nullptr;     // null for a generated file
        std::vector<std::uint8_t> content;    // a generated file's bytes
        InodeRecord inode;
        std::uint32_t number = 0;
    };

    const tree::Source* source = nullptr;
    BuildOptions options;
    std::int64_t now = 0;
    std::vector<Dir> dirs; // [0] is the root
    std::vector<File> files;
    bool collision = false;
    std::vector<std::uint8_t> super_root_blob, fpt_blob, collision_blob;
    InodeRecord super_root, fpt, collision_resolver;
    std::uint32_t inode_count = 0;
    std::uint64_t inode_blocks = 0;
    std::uint64_t total_blocks = 0;
    std::uint64_t file_bytes = 0;

    std::uint32_t bs() const { return options.block_size; }
};

namespace {

using Impl = ImageBuilder::Impl;

Status collect(const tree::Node& node, const std::string& prefix, std::vector<Impl::File>& out, int depth) {
    if (depth > 256) return make_error(Errc::invalid_argument, "the folder tree is too deep");
    for (const tree::Node& child : node.children) {
        if (tree::is_os_junk(child.name)) continue;
        for (char c : child.name)
            if (static_cast<unsigned char>(c) > 0x7F || c == '\0')
                return make_error(Errc::invalid_argument,
                                  "PFS images only support ASCII file names: " + prefix + child.name);
        std::string rel = prefix + child.name;
        if (child.is_directory) ALEMBIC_TRY(collect(child, rel + "/", out, depth + 1));
        else {
            Impl::File f;
            f.rel = rel;
            f.name = child.name;
            f.node = &child;
            out.push_back(std::move(f));
        }
    }
    return {};
}

// MkPFS sorts by the lower-cased relative path; ties keep a fixed order here.
template <typename T>
void sort_by_path(std::vector<T>& items) {
    std::stable_sort(items.begin(), items.end(), [](const T& a, const T& b) {
        std::string la = lower_ascii(a.rel), lb = lower_ascii(b.rel);
        return la != lb ? la < lb : a.rel < b.rel;
    });
}

} // namespace

ImageBuilder::ImageBuilder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ImageBuilder::ImageBuilder(ImageBuilder&&) noexcept = default;
ImageBuilder& ImageBuilder::operator=(ImageBuilder&&) noexcept = default;
ImageBuilder::~ImageBuilder() = default;

std::uint64_t ImageBuilder::image_length() const { return impl_->total_blocks * impl_->bs(); }

Result<ImageBuilder> ImageBuilder::from_source(const tree::Source& source, const tree::Node& root,
                                               const BuildOptions& options) {
    std::uint32_t b = options.block_size;
    if (b < 0x1000 || b > 0x100000 || (b & (b - 1)) != 0)
        return make_error(Errc::invalid_argument, "the PFS block size must be a power of two from 4 KiB to 1 MiB");
    auto m = std::make_unique<Impl>();
    m->source = &source;
    m->options = options;
    m->now = options.timestamp.value_or(static_cast<std::int64_t>(std::time(nullptr)));
    const bool ci = options.case_insensitive;

    // Files, then the folders that hold them (MkPFS only scans files, so empty
    // folders are left out).
    ALEMBIC_TRY(collect(root, "", m->files, 0));
    if (options.generate_ampr_index) {
        auto has = [&](const char* rel) {
            return std::any_of(m->files.begin(), m->files.end(), [&](const Impl::File& f) { return unicode::ascii_iequals(f.rel, rel); });
        };
        if (has("fakelib/libSceAmpr.sprx") && !has("ampr_assets.index") && (options.rebuild_ampr_index || !has("ampr_emu.index"))) {
            std::erase_if(m->files, [](const Impl::File& f) {
                return unicode::ascii_iequals(f.rel, "ampr_emu.index") || unicode::ascii_iequals(f.rel, "ampr_emu.index.tmp");
            });
            ALEMBIC_ASSIGN(std::vector<std::uint8_t> index, exfat::build_ampr_index(source, root, options.ampr_scheme));
            if (!index.empty()) {
                Impl::File f;
                f.rel = f.name = "ampr_emu.index";
                f.content = std::move(index);
                m->files.push_back(std::move(f));
            }
        }
    }
    sort_by_path(m->files);
    std::vector<Impl::Dir> dirs;
    std::set<std::string> seen;
    for (const Impl::File& f : m->files) {
        std::size_t slash = 0;
        while ((slash = f.rel.find('/', slash)) != std::string::npos) {
            std::string rel = f.rel.substr(0, slash++);
            if (!seen.insert(rel).second) continue;
            Impl::Dir d;
            d.rel = rel;
            std::size_t last = rel.rfind('/');
            d.name = last == std::string::npos ? rel : rel.substr(last + 1);
            dirs.push_back(std::move(d));
        }
    }
    sort_by_path(dirs);
    m->dirs.push_back(Impl::Dir{});
    for (Impl::Dir& d : dirs) m->dirs.push_back(std::move(d));
    std::unordered_map<std::string, int> dir_index;
    for (std::size_t i = 0; i < m->dirs.size(); ++i) dir_index[m->dirs[i].rel] = static_cast<int>(i);
    auto parent_of = [&](const std::string& rel) {
        std::size_t last = rel.rfind('/');
        return dir_index.at(last == std::string::npos ? std::string() : rel.substr(0, last));
    };
    for (std::size_t i = 1; i < m->dirs.size(); ++i) {
        m->dirs[i].parent = parent_of(m->dirs[i].rel);
        m->dirs[static_cast<std::size_t>(m->dirs[i].parent)].child_dirs.push_back(static_cast<int>(i));
    }
    for (std::size_t i = 0; i < m->files.size(); ++i)
        m->dirs[static_cast<std::size_t>(parent_of(m->files[i].rel))].child_files.push_back(static_cast<int>(i));

    // Flat path table: one hash per folder and file path. When two paths share
    // a hash, a collision_resolver file lists them and takes inode 2.
    struct PathEntry {
        std::string path;
        std::size_t index;
        bool is_dir;
    };
    std::map<std::uint32_t, std::vector<PathEntry>> by_hash;
    for (std::size_t i = 1; i < m->dirs.size(); ++i) {
        std::string p = "/" + m->dirs[i].rel;
        by_hash[fpt_hash(p, ci)].push_back({p, i, true});
    }
    for (std::size_t i = 0; i < m->files.size(); ++i) {
        std::string p = "/" + m->files[i].rel;
        by_hash[fpt_hash(p, ci)].push_back({p, i, false});
    }
    for (const auto& [h, list] : by_hash)
        if (list.size() > 1) m->collision = true;

    // Inode numbers: super root, path table, [collision resolver], user root,
    // folders, files.
    const std::uint32_t shift = m->collision ? 1 : 0;
    m->dirs[0].number = 2 + shift;
    std::uint64_t next = 3 + shift;
    for (std::size_t i = 1; i < m->dirs.size(); ++i) m->dirs[i].number = static_cast<std::uint32_t>(next++);
    for (Impl::File& f : m->files) f.number = static_cast<std::uint32_t>(next++);
    if (next > UINT32_MAX) return make_error(Errc::too_large, "too many files for a PFS image");
    m->inode_count = static_cast<std::uint32_t>(next);

    std::map<std::uint32_t, std::uint32_t> collision_offsets;
    if (m->collision) {
        for (const auto& [h, list] : by_hash) {
            if (list.size() < 2) continue;
            collision_offsets[h] = static_cast<std::uint32_t>(m->collision_blob.size());
            for (const PathEntry& e : list)
                append_dirent(m->collision_blob, e.is_dir ? m->dirs[e.index].number : m->files[e.index].number,
                              e.is_dir ? kDirentDirectory : kDirentFile, e.path);
            m->collision_blob.resize(m->collision_blob.size() + 0x18, 0);
        }
    }
    for (const auto& [h, list] : by_hash) {
        std::uint32_t value;
        if (list.size() == 1) {
            const PathEntry& e = list[0];
            value = e.is_dir ? (m->dirs[e.index].number | kFptDirectoryBit) : m->files[e.index].number;
        } else {
            value = kFptCollisionBit | collision_offsets[h];
        }
        std::size_t at = m->fpt_blob.size();
        m->fpt_blob.resize(at + 8);
        put_u32(&m->fpt_blob[at], h);
        put_u32(&m->fpt_blob[at + 4], value);
    }

    // Folder contents: ".", "..", subfolders, then files, each in path order.
    for (std::size_t i = 0; i < m->dirs.size(); ++i) {
        Impl::Dir& d = m->dirs[i];
        append_dirent(d.blob, d.number, kDirentDot, ".");
        append_dirent(d.blob, i == 0 ? d.number : m->dirs[static_cast<std::size_t>(d.parent)].number, kDirentDotDot, "..");
        for (int c : d.child_dirs)
            append_dirent(d.blob, m->dirs[static_cast<std::size_t>(c)].number, kDirentDirectory, m->dirs[static_cast<std::size_t>(c)].name);
        for (int c : d.child_files)
            append_dirent(d.blob, m->files[static_cast<std::size_t>(c)].number, kDirentFile, m->files[static_cast<std::size_t>(c)].name);
    }
    append_dirent(m->super_root_blob, 1, kDirentFile, "flat_path_table");
    if (m->collision) append_dirent(m->super_root_blob, 2, kDirentFile, "collision_resolver");
    append_dirent(m->super_root_blob, m->dirs[0].number, kDirentDirectory, "uroot");

    // Block layout: header, inode table, super root, path table, the collision
    // resolver or one empty block, then the folders and the files, each one
    // contiguous run of blocks.
    const std::uint64_t bs = b;
    auto blocks_for = [&](std::uint64_t bytes) { return std::max<std::uint64_t>(1, div_round_up(bytes, bs)); };
    const std::uint64_t per_block = bs / kInodeSize;
    m->inode_blocks = div_round_up(m->inode_count, per_block);
    std::uint64_t nd = 1 + m->inode_blocks;
    auto place = [&](InodeRecord& r, std::uint64_t blocks) -> Status {
        if (nd + blocks > kInt32Max || blocks > UINT32_MAX)
            return make_error(Errc::too_large, "the image needs more blocks than 32-bit PFS pointers can address");
        r.first_block = static_cast<std::int32_t>(nd);
        r.blocks = static_cast<std::uint32_t>(blocks);
        nd += blocks;
        return {};
    };
    const std::uint32_t internal = kFlagInternal | kFlagReadOnly;
    m->super_root = {static_cast<std::uint16_t>(kTypeDirectory | kRxOnly), 1, internal, bs, bs, 0, 0, 0};
    ALEMBIC_TRY(place(m->super_root, 1));
    m->fpt = {static_cast<std::uint16_t>(kTypeFile | kRxOnly), 1, internal, m->fpt_blob.size(), m->fpt_blob.size(), 0, 0, -1};
    ALEMBIC_TRY(place(m->fpt, blocks_for(m->fpt_blob.size())));
    if (m->collision) {
        m->collision_resolver = {static_cast<std::uint16_t>(kTypeFile | kRxOnly), 1, internal, m->collision_blob.size(),
                                 m->collision_blob.size(), 0, 0, -1};
        ALEMBIC_TRY(place(m->collision_resolver, blocks_for(m->collision_blob.size())));
    } else {
        ++nd; // reserved, left empty
    }
    for (std::size_t i = 0; i < m->dirs.size(); ++i) {
        Impl::Dir& d = m->dirs[i];
        std::uint64_t blocks = blocks_for(d.blob.size());
        d.inode = {static_cast<std::uint16_t>(kTypeDirectory | kRxOnly),
                   static_cast<std::uint16_t>((i == 0 ? 3 : 2) + d.child_dirs.size()), kFlagReadOnly, blocks * bs,
                   blocks * bs, 0, 0, -1};
        if (d.child_dirs.size() > 0xFFF0) return make_error(Errc::too_large, "a folder has too many subfolders for PFS");
        ALEMBIC_TRY(place(d.inode, blocks));
    }
    for (Impl::File& f : m->files) {
        std::uint64_t size = f.node ? f.node->size : f.content.size();
        f.inode = {static_cast<std::uint16_t>(kTypeFile | kRxOnly), 1, kFlagReadOnly, size, size, 0, 0, -1};
        ALEMBIC_TRY(place(f.inode, blocks_for(size)));
        m->file_bytes += size;
    }
    m->total_blocks = nd;
    return ImageBuilder(std::move(m));
}

Status ImageBuilder::write(ByteSink& out, const ProgressFn& progress, std::uint64_t start) const {
    const Impl& m = *impl_;
    const std::uint64_t bs = m.bs();
    if (start > m.total_blocks * bs) return make_error(Errc::invalid_argument, "the start is past the PFS image");
    resume::SkipSink sink(out, start); // drops what an interrupted write already has
    std::uint64_t written_blocks = 0;
    std::vector<std::uint8_t> block(static_cast<std::size_t>(bs));
    auto emit_padded = [&](const std::uint8_t* data, std::size_t size, std::uint64_t blocks) -> Status {
        std::uint64_t total = blocks * bs;
        if (size > total) return make_error(Errc::invalid_data, "PFS layout error: data larger than its blocks");
        if (size) ALEMBIC_TRY(sink.write(Bytes(data, size)));
        std::fill(block.begin(), block.end(), 0);
        for (std::uint64_t left = total - size; left;) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, bs));
            ALEMBIC_TRY(sink.write(Bytes(block.data(), n)));
            left -= n;
        }
        written_blocks += blocks;
        return {};
    };
    auto expect = [&](const InodeRecord& r) -> Status {
        if (static_cast<std::uint64_t>(r.first_block) != written_blocks)
            return make_error(Errc::invalid_data, "PFS layout error: blocks out of order");
        return {};
    };

    // Header (block 0), as MkPFS's _pack_pfs_header_block.
    std::vector<std::uint8_t> h(static_cast<std::size_t>(bs), 0);
    put_u64(&h[0x00], kVersionPs5);
    put_u64(&h[0x08], kMagic);
    h[0x1A] = 1;
    put_u16(&h[0x1C], m.options.case_insensitive ? kModeCaseInsensitive : 0);
    put_u32(&h[0x20], static_cast<std::uint32_t>(bs));
    put_u64(&h[0x28], 1);
    put_u64(&h[0x30], m.inode_count);
    put_u64(&h[0x38], m.total_blocks);
    put_u64(&h[0x40], m.inode_blocks);
    std::uint8_t* sig = &h[0x50]; // the inode-table block signature (S64 layout), unsigned
    put_u16(sig + 0x02, 1);
    put_u32(sig + 0x04, kFlagReadOnly);
    put_u64(sig + 0x08, m.inode_blocks * bs);
    put_u64(sig + 0x10, m.inode_blocks * bs);
    for (int i = 0; i < 4; ++i) put_u64(sig + 0x18 + i * 8, static_cast<std::uint64_t>(m.now));
    put_u32(sig + 0x60, static_cast<std::uint32_t>(m.inode_blocks));
    put_u64(sig + 0x68 + 32, 1); // db[0]: the inode table starts at block 1
    put_u32(&h[0x368], 1);
    ALEMBIC_TRY(emit_padded(h.data(), h.size(), 1));

    // Inode table: whole inodes per block.
    std::vector<std::uint8_t> table(static_cast<std::size_t>(m.inode_blocks * bs), 0);
    const std::uint64_t per_block = bs / kInodeSize;
    auto put = [&](std::uint32_t n, const InodeRecord& r) {
        put_inode(&table[static_cast<std::size_t>((n / per_block) * bs + (n % per_block) * kInodeSize)], r, m.now);
    };
    put(0, m.super_root);
    put(1, m.fpt);
    if (m.collision) put(2, m.collision_resolver);
    for (const Impl::Dir& d : m.dirs) put(d.number, d.inode);
    for (const Impl::File& f : m.files) put(f.number, f.inode);
    ALEMBIC_TRY(emit_padded(table.data(), table.size(), m.inode_blocks));

    ALEMBIC_TRY(expect(m.super_root));
    ALEMBIC_TRY(emit_padded(m.super_root_blob.data(), m.super_root_blob.size(), m.super_root.blocks));
    ALEMBIC_TRY(expect(m.fpt));
    ALEMBIC_TRY(emit_padded(m.fpt_blob.data(), m.fpt_blob.size(), m.fpt.blocks));
    if (m.collision) {
        ALEMBIC_TRY(expect(m.collision_resolver));
        ALEMBIC_TRY(emit_padded(m.collision_blob.data(), m.collision_blob.size(), m.collision_resolver.blocks));
    } else {
        ALEMBIC_TRY(emit_padded(nullptr, 0, 1));
    }
    for (const Impl::Dir& d : m.dirs) {
        ALEMBIC_TRY(expect(d.inode));
        ALEMBIC_TRY(emit_padded(d.blob.data(), d.blob.size(), d.inode.blocks));
    }

    std::uint64_t done = 0;
    std::vector<std::uint8_t> buffer(1u << 20);
    for (const Impl::File& f : m.files) {
        ALEMBIC_TRY(expect(f.inode));
        if (!f.node) { // generated
            ALEMBIC_TRY(emit_padded(f.content.data(), f.content.size(), f.inode.blocks));
            done += f.content.size();
            continue;
        }
        const std::uint64_t span = static_cast<std::uint64_t>(f.inode.blocks) * bs;
        if (sink.skippable() >= span) { // written before: not read again
            sink.skip(span);
            written_blocks += f.inode.blocks;
            done += f.inode.size;
            continue;
        }
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, m.source->open(*f.node));
        ALEMBIC_ASSIGN(std::uint64_t actual, in->size());
        if (actual != f.inode.size) return make_error(Errc::invalid_data, "a file changed size while writing: " + f.rel);
        const std::uint64_t first = std::min(sink.skippable(), actual);
        sink.skip(first);
        done += first;
        for (std::uint64_t off = first; off < actual;) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), actual - off));
            ALEMBIC_TRY(in->read_at(off, MutableBytes(buffer.data(), n)));
            ALEMBIC_TRY(sink.write(Bytes(buffer.data(), n)));
            off += n;
            done += n;
            if (progress && !progress({"Writing PFS image", done, m.file_bytes}))
                return make_error(Errc::cancelled, "cancelled");
        }
        std::uint64_t pad = static_cast<std::uint64_t>(f.inode.blocks) * bs - actual;
        std::fill(block.begin(), block.end(), 0);
        for (std::uint64_t left = pad; left;) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, bs));
            ALEMBIC_TRY(sink.write(Bytes(block.data(), n)));
            left -= n;
        }
        written_blocks += f.inode.blocks;
    }
    if (written_blocks != m.total_blocks) return make_error(Errc::invalid_data, "PFS layout error: wrong image length");
    return {};
}

Status write_image(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                   const BuildOptions& options, bool overwrite, const ProgressFn& progress, resume::Session* session) {
    if (!session) {
        ALEMBIC_ASSIGN(ImageBuilder builder, ImageBuilder::from_source(source, root, options));
        ALEMBIC_ASSIGN(AtomicOutput output, AtomicOutput::create(output_path, overwrite));
        FileSink sink(output.file(), 0);
        ALEMBIC_TRY(builder.write(sink, progress));
        return output.commit();
    }
    // The time stamp goes into the image: a continued write takes it from the checkpoint.
    BuildOptions o = options;
    if (session->from) {
        std::uint64_t t = resume::get(*session->from, "timestamp");
        if (t == ~std::uint64_t{0}) return make_error(Errc::invalid_data, "the checkpoint does not match this conversion");
        o.timestamp = static_cast<std::int64_t>(t);
    } else if (!o.timestamp) {
        o.timestamp = static_cast<std::int64_t>(std::time(nullptr));
    }
    ALEMBIC_ASSIGN(ImageBuilder builder, ImageBuilder::from_source(source, root, o));
    const std::uint64_t length = builder.image_length();
    ALEMBIC_ASSIGN(std::uint64_t start, resume::sequential_start(session, length, resume::kPfsFormat));
    ALEMBIC_ASSIGN(AtomicOutput output,
                   AtomicOutput::create_partial(output_path, session->partial_path, session->from.has_value(), overwrite));
    ALEMBIC_TRY(output.file().truncate(start)); // drop what came after the checkpoint
    resume::CheckpointSink sink(output.file(), start, length, session, resume::kPfsFormat,
                                {{"timestamp", static_cast<std::uint64_t>(*o.timestamp)}});
    if (Status s = builder.write(sink, progress, start); !s.ok()) {
        (void)sink.save_now(); // a pause loses nothing already written
        return s;
    }
    return output.commit();
}

// ---------------------------------------------------------------- reading

namespace {

constexpr std::size_t kMaxEntries = 4'000'000;
constexpr std::uint64_t kMaxDirectoryBytes = 64ull << 20;
constexpr int kMaxDepth = 256;

struct Volume {
    std::shared_ptr<const RandomAccess> image;
    std::uint64_t block_size = 0;
    std::uint64_t block_count = 0;
    std::uint64_t inode_count = 0;
    std::uint64_t per_block = 0;

    Result<InodeRecord> inode(std::uint64_t n, std::int64_t* time = nullptr) const {
        if (n >= inode_count) return invalid("a PFS entry references an invalid inode");
        std::uint8_t d[kInodeSize];
        std::uint64_t at = (1 + n / per_block) * block_size + (n % per_block) * kInodeSize;
        ALEMBIC_TRY(image->read_at(at, MutableBytes(d, sizeof d)));
        InodeRecord r;
        r.mode = get_u16(d);
        r.nlink = get_u16(d + 2);
        r.flags = get_u32(d + 4);
        r.size = get_u64(d + 8);
        r.size_compressed = get_u64(d + 0x10);
        r.blocks = get_u32(d + 0x60);
        r.first_block = static_cast<std::int32_t>(get_u32(d + 0x64));
        if (time) *time = static_cast<std::int64_t>(get_u64(d + 0x18));
        // Only contiguous files: the other direct pointers are unused or follow on.
        for (std::uint32_t i = 1; i < 12 && i < r.blocks; ++i) {
            auto p = static_cast<std::int32_t>(get_u32(d + 0x64 + i * 4));
            if (p != -1 && p != 0 && p != r.first_block + static_cast<std::int32_t>(i))
                return make_error(Errc::unsupported, "this PFS image has fragmented files (not supported yet)");
        }
        if (r.blocks == 0 || r.first_block <= 0 || static_cast<std::uint64_t>(r.first_block) + r.blocks > block_count)
            return invalid("a PFS inode points outside the image");
        if (r.size > static_cast<std::uint64_t>(r.blocks) * block_size)
            return invalid("a PFS inode is larger than its blocks");
        return r;
    }

    std::uint64_t offset(const InodeRecord& r) const { return static_cast<std::uint64_t>(r.first_block) * block_size; }
};

struct Entry {
    std::uint32_t inode;
    std::uint32_t type;
    std::string name;
};

Result<std::vector<Entry>> read_dir(const Volume& v, const InodeRecord& r) {
    std::uint64_t length = std::min<std::uint64_t>(r.size, static_cast<std::uint64_t>(r.blocks) * v.block_size);
    if (length > kMaxDirectoryBytes) return invalid("a PFS folder is unreasonably large");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    ALEMBIC_TRY(v.image->read_at(v.offset(r), data));
    std::vector<Entry> out;
    std::size_t at = 0;
    while (at + 16 <= data.size()) {
        std::uint32_t ino = get_u32(&data[at]);
        std::uint32_t type = get_u32(&data[at + 4]);
        std::uint32_t name_length = get_u32(&data[at + 8]);
        std::uint32_t size = get_u32(&data[at + 12]);
        if (size == 0) break; // the zero padding after the last entry
        if (size < 16 || (size & 7) || size > data.size() - at || name_length > size - 16)
            return invalid("a PFS folder entry is malformed");
        out.push_back({ino, type, std::string(reinterpret_cast<const char*>(&data[at + 16]), name_length)});
        at += size;
    }
    return out;
}

class PfsSource final : public tree::Source {
public:
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return label_; }

    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory) return make_error(Errc::invalid_argument, "not a PFS file");
        ALEMBIC_ASSIGN(InodeRecord r, volume_->inode(file.id));
        if (r.flags & kFlagCompressed) {
            ALEMBIC_ASSIGN(std::shared_ptr<pfsc::Reader> reader,
                           pfsc::Reader::open(volume_->image, volume_->offset(r), r.size, r.size_compressed));
            return std::shared_ptr<const RandomAccess>(std::move(reader));
        }
        return std::shared_ptr<const RandomAccess>(std::make_shared<Slice>(volume_->image, volume_->offset(r), r.size));
    }

    Status load(tree::Node& node, std::uint32_t number, std::set<std::uint32_t>& active, int depth) {
        if (depth > kMaxDepth) return invalid("the PFS folder depth exceeds the safety limit");
        if (!active.insert(number).second) return invalid("the PFS folder graph contains a cycle");
        ALEMBIC_ASSIGN(InodeRecord dir, volume_->inode(number));
        if ((dir.mode & 0xF000) != kTypeDirectory) return invalid("a PFS folder entry points to a file inode");
        ALEMBIC_ASSIGN(std::vector<Entry> entries, read_dir(*volume_, dir));
        for (const Entry& e : entries) {
            if (e.type == kDirentDot || e.type == kDirentDotDot) continue;
            if (e.type != kDirentFile && e.type != kDirentDirectory) continue;
            if (e.name.empty() || e.name == "." || e.name == ".." || e.name.find('/') != std::string::npos ||
                e.name.find('\0') != std::string::npos)
                return invalid("a PFS folder contains an unsafe name");
            if (++entries_ > kMaxEntries) return invalid("the PFS entry count exceeds the safety limit");
            std::int64_t time = 0;
            ALEMBIC_ASSIGN(InodeRecord child, volume_->inode(e.inode, &time));
            bool is_dir = e.type == kDirentDirectory;
            if (is_dir != ((child.mode & 0xF000) == kTypeDirectory)) return invalid("a PFS entry type disagrees with its inode");
            tree::Node n;
            n.name = e.name;
            n.is_directory = is_dir;
            n.size = is_dir ? 0 : (child.flags & kFlagCompressed ? child.size_compressed : child.size);
            n.mtime = time;
            n.id = e.inode;
            if (is_dir) ALEMBIC_TRY(load(n, e.inode, active, depth + 1));
            node.children.push_back(std::move(n));
        }
        active.erase(number);
        return {};
    }

    std::shared_ptr<Volume> volume_;
    tree::Node root_;
    std::string label_;
    std::size_t entries_ = 0;
};

} // namespace

bool looks_like_pfs(const RandomAccess& image) {
    auto size = image.size();
    if (!size.ok() || *size < 0x1000) return false;
    std::uint8_t h[16];
    if (!image.read_at(0, MutableBytes(h, sizeof h)).ok()) return false;
    std::uint64_t version = get_u64(h);
    return (version == 1 || version == 2) && get_u64(h + 8) == kMagic;
}

Result<std::unique_ptr<tree::Source>> open_image(std::shared_ptr<const RandomAccess> image, std::string label) {
    ALEMBIC_ASSIGN(std::uint64_t file_size, image->size());
    if (!looks_like_pfs(*image)) return invalid("not a PFS image");
    std::uint8_t h[0x48];
    ALEMBIC_TRY(image->read_at(0, MutableBytes(h, sizeof h)));
    std::uint16_t mode = get_u16(h + 0x1C);
    if (mode & (kModeSigned | kModeEncrypted))
        return make_error(Errc::unsupported, "signed or encrypted PFS images are not supported");
    if (mode & kMode64BitInodes) return make_error(Errc::unsupported, "PFS images with 64-bit inodes are not supported yet");
    auto v = std::make_shared<Volume>();
    v->image = std::move(image);
    v->block_size = get_u32(h + 0x20);
    v->inode_count = get_u64(h + 0x30);
    v->block_count = get_u64(h + 0x38);
    std::uint64_t inode_blocks = get_u64(h + 0x40);
    if (v->block_size < 0x1000 || v->block_size > 0x100000 || (v->block_size & (v->block_size - 1)))
        return invalid("the PFS block size is invalid");
    v->per_block = v->block_size / kInodeSize;
    if (get_u64(h + 0x28) != 1 || v->inode_count < 3 || inode_blocks == 0 ||
        inode_blocks > v->block_count || div_round_up(v->inode_count, v->per_block) > inode_blocks)
        return invalid("the PFS inode table is invalid");
    if (v->block_count > file_size / v->block_size) return invalid("the PFS image is shorter than its block count");

    auto source = std::make_unique<PfsSource>();
    source->volume_ = v;
    source->label_ = std::move(label);
    ALEMBIC_ASSIGN(InodeRecord super_root, v->inode(0));
    ALEMBIC_ASSIGN(std::vector<Entry> top, read_dir(*v, super_root));
    const Entry* uroot = nullptr;
    for (const Entry& e : top)
        if (e.type == kDirentDirectory && e.name == "uroot") uroot = &e;
    if (!uroot) return invalid("the PFS image has no uroot folder");
    std::set<std::uint32_t> active;
    ALEMBIC_TRY(source->load(source->root_, uroot->inode, active, 0));
    source->root_.is_directory = true;
    return std::unique_ptr<tree::Source>(std::move(source));
}

} // namespace alembic::pfs
