// SPDX-License-Identifier: GPL-3.0-or-later
// The inner image (pfs_image.dat) of a debug package. Ported from
// pearlxcore/ProsperoPkgTool (MIT) Containers/ProsperoInnerImageAssembler.cs
// and Containers/ProsperoPfsv3Writer.cs (stored path).
//
// Layout, data first: [file payloads][32-byte block-info table][metadata in a
// PFSv3 container]. Files are placed in afid order; the metadata region (a
// superblock, the inode table and the content blocks: super-root dirents,
// inode_flat_path_table, apr_flat_path_table, afid_to_ino_table and every
// folder's dirents) describes the logical mount, whose data region the NAPS
// table maps onto the physical payloads.
#include "alembic/fpkg_build.hpp"
#include "alembic/thread_pool.hpp"

#include "alembic/bytes.hpp"
#include "alembic/crypto.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

namespace alembic::fpkg::build {

namespace {

constexpr std::uint64_t kBs = kInnerBlock;
constexpr std::uint64_t kMetadataReserveBlocks = 60;
constexpr std::uint32_t kInodeSize = 0xA8;
constexpr std::uint32_t kInodesPerBlock = kInnerBlock / kInodeSize;

constexpr int kDirentFile = 2;
constexpr int kDirentDirectory = 3;
constexpr int kDirentDot = 4;
constexpr int kDirentDotDot = 5;

std::uint64_t round_up(std::uint64_t v, std::uint64_t g) { return (v + g - 1) / g * g; }

std::uint64_t rotl(std::uint64_t x, int n) { return (x << n) | (x >> (64 - n)); }
std::uint64_t rotr(std::uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

constexpr std::uint64_t kFltSeed0 = 0x92CA8AAB26A24F51ULL;
constexpr std::uint64_t kFltSeed1 = 0x09BBB761A41BC44DULL;
constexpr std::uint64_t kFltRound = 0x8000000080008081ULL;

std::uint64_t hash_bytes(const std::string& s) {
    const std::size_t len = s.size();
    const auto* str = reinterpret_cast<const std::uint8_t*>(s.data());
    std::uint64_t s0 = kFltSeed0, s1 = rotl(kFltSeed0, 11), s2 = rotl(kFltSeed0, 23);
    std::uint64_t tail = 0;
    if (len != 0) {
        std::size_t nwords = (len - 1) >> 3;
        std::uint64_t a0 = s0, a1 = s1, a2 = s2;
        std::size_t off = 0;
        for (std::size_t i = 0; i < nwords; ++i) {
            std::uint64_t w = get_u64(str + off);
            off += 8;
            a0 ^= w;
            std::uint64_t t18 = rotr(rotl(a2 ^ a1, 5) ^ a0, 11);
            std::uint64_t t12 = rotl(rotl(a2 ^ a0, 17) ^ a1, 11);
            a2 = rotr(rotl(a1 ^ a0, 1) ^ a2, 5);
            a0 = (~t12 & a2) ^ t18 ^ kFltRound;
            a1 = (~a2 & t18) ^ t12;
            a2 = (~t18 & t12) ^ a2;
        }
        s0 = a0;
        s1 = a1;
        s2 = a2;
        std::size_t tlen = ((len - 1) & 7) + 1;
        for (std::size_t j = 0; j < tlen; ++j) tail |= static_cast<std::uint64_t>(str[off + j]) << (8 * j);
    }
    std::uint64_t u16 = s1, u11 = s2, u6 = tail ^ s0 ^ kFltSeed1;
    std::uint64_t u17 = rotl(u11 ^ u16, 5) ^ u6;
    std::uint64_t u18 = rotl(u11 ^ u6, 17) ^ u16;
    u11 = rotl(u16 ^ u6, 1) ^ u11;
    return (~rotl(u18, 11) & rotr(u11, 5)) ^ rotr(u17, 11) ^ kFltRound;
}

struct Dir;

struct FileNode {
    std::string name;
    std::string full_path; // "/a/b"
    const InnerFile* in = nullptr;
    std::uint64_t length = 0;
    Dir* parent = nullptr;
    std::uint32_t inode = 0;
    std::uint32_t afid = 0;
    bool exec = false;
    bool store_raw = false;
    bool whole_block_raw = false;
    std::uint64_t logical = 0;
    std::uint64_t on_disk = 0;
    std::uint64_t encoded = 0;
    crypto::Digest sha3{};
    std::int32_t dirent_offset = -1;
};

struct Dirent {
    std::uint32_t inode;
    int type;
    std::string name;
};

struct Dir {
    std::string name;
    std::string full_path; // "" for uroot, else "a/b" (no leading slash)
    Dir* parent = nullptr;
    std::map<std::string, std::unique_ptr<Dir>> subdirs; // ordinal name order
    std::map<std::string, FileNode*> files;
    std::uint32_t inode = 0;
    std::int32_t dirent_offset = -1;
    std::vector<Dirent> dirents;
};

bool in_sce_sys(const Dir* d) { return d->full_path == "sce_sys" || d->full_path.rfind("sce_sys/", 0) == 0; }

void dirs_pre_order(Dir* d, std::vector<Dir*>& out) {
    out.push_back(d);
    for (auto& [n, c] : d->subdirs) dirs_pre_order(c.get(), out);
}
void dirs_post_order(Dir* d, std::vector<Dir*>& out) {
    for (auto& [n, c] : d->subdirs) dirs_post_order(c.get(), out);
    out.push_back(d);
}
void files_pre_order(Dir* d, std::vector<FileNode*>& out) {
    for (auto& [n, f] : d->files) out.push_back(f);
    for (auto& [n, c] : d->subdirs) files_pre_order(c.get(), out);
}
// Non-sce_sys files: at each folder, subfolders and files merged in name order.
void files_entry_order(Dir* d, const Dir* excluded, std::vector<FileNode*>& out) {
    auto di = d->subdirs.begin();
    auto fi = d->files.begin();
    while (di != d->subdirs.end() || fi != d->files.end()) {
        bool take_dir = fi == d->files.end() || (di != d->subdirs.end() && di->first < fi->first);
        if (take_dir) {
            if (di->second.get() != excluded) files_entry_order(di->second.get(), excluded, out);
            ++di;
        } else {
            out.push_back(fi->second);
            ++fi;
        }
    }
}

int system_rank(const FileNode* f) {
    if (f->full_path == "/sce_sys/keystone") return 0;
    if (f->exec) return 1;
    if (f->full_path == "/sce_sys/pfs-version.dat") return 3;
    return 2;
}

std::size_t dirent_size(std::size_t name_length) { return align_up(name_length + 17, 8); }

std::vector<std::uint8_t> serialize_dirents(const std::vector<Dirent>& list) {
    std::vector<std::uint8_t> out;
    for (const Dirent& d : list) {
        std::size_t size = dirent_size(d.name.size());
        std::size_t at = out.size();
        out.resize(at + size, 0);
        put_u32(&out[at], d.inode);
        put_u32(&out[at + 4], static_cast<std::uint32_t>(d.type));
        put_u32(&out[at + 8], static_cast<std::uint32_t>(d.name.size()));
        put_u32(&out[at + 12], static_cast<std::uint32_t>(size));
        std::memcpy(&out[at + 16], d.name.data(), d.name.size());
    }
    return out;
}

struct FltEntry {
    std::uint64_t hash;
    std::uint64_t packed;
};

std::vector<std::uint8_t> serialize_flt(std::vector<FltEntry> entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const FltEntry& a, const FltEntry& b) { return a.hash < b.hash; });
    std::vector<std::uint8_t> out(0x40 + entries.size() * 16, 0);
    put_u32(&out[0], 1);
    out[4] = 0x10;
    put_u32(&out[8], 0x40);
    out[0x20] = 0x7F;
    out[0x21] = 'F';
    out[0x22] = 'L';
    out[0x23] = 'T';
    put_u32(&out[0x2C], static_cast<std::uint32_t>(entries.size()));
    put_u64(&out[0x30], kFltSeed0);
    put_u64(&out[0x38], kFltSeed1);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        put_u64(&out[0x40 + i * 16], entries[i].hash);
        put_u64(&out[0x40 + i * 16 + 8], entries[i].packed);
    }
    return out;
}

std::uint64_t pack_inode_entry(std::uint32_t inode, bool is_dir, bool subtree, std::uint32_t afid) {
    std::uint64_t packed = inode & 0xFFFFFFULL;
    if (is_dir) packed |= 0x40000000ULL;
    if (subtree) packed |= 0x80000000ULL;
    packed |= (is_dir ? 0xFFFFFFULL : static_cast<std::uint64_t>(afid)) << 40;
    return packed;
}

std::uint64_t pack_apr_entry(std::uint64_t size, std::uint32_t afid) {
    return (size & 0xFFFFFFFFFFULL) | (static_cast<std::uint64_t>(afid) << 40);
}

std::uint64_t allocated_blocks(std::size_t length) { return std::max<std::uint64_t>(1, div_round_up(length, kBs)); }

std::vector<std::uint8_t> block_info_table(std::uint64_t data_end) {
    constexpr std::uint32_t kVersion = 0x00400003, kTemplate = 0x00FCFF27, kBase = 0x0027FFFC;
    std::uint32_t sub = static_cast<std::uint32_t>((data_end & 0xFFFF) * 4);
    std::uint32_t be = (kBase - sub) & 0xFFFFFF;
    std::uint32_t value = ((be & 0xFF) << 16) | (be & 0xFF00) | ((be >> 16) & 0xFF);
    std::vector<std::uint8_t> t(32, 0);
    for (int i = 0; i < 4; ++i) {
        put_u32(&t[i * 8], i < 3 ? kTemplate : value);
        put_u32(&t[i * 8 + 4], kVersion);
    }
    return t;
}

// Appends to the sink, filling gaps with zeros.
class Writer {
public:
    explicit Writer(ByteSink& sink) : sink_(sink) {}
    std::uint64_t pos() const { return pos_; }
    // Resuming: the sink already holds this many bytes.
    void skip_to(std::uint64_t pos) { pos_ = pos; }
    Status zeros_to(std::uint64_t target) {
        if (target < pos_) return make_error(Errc::invalid_data, "inner image layout error: writing backwards");
        static const std::vector<std::uint8_t> zeros(1 << 16, 0);
        while (pos_ < target) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(zeros.size(), target - pos_));
            ALEMBIC_TRY(sink_.write(Bytes(zeros.data(), n)));
            pos_ += n;
        }
        return {};
    }
    Status write(Bytes data) {
        ALEMBIC_TRY(sink_.write(data));
        pos_ += data.size();
        return {};
    }

private:
    ByteSink& sink_;
    std::uint64_t pos_ = 0;
};

Result<std::shared_ptr<const RandomAccess>> open_input(const InnerFile& f) {
    if (f.node) return f.source->open(*f.node);
    struct Memory final : RandomAccess {
        const std::vector<std::uint8_t>* data;
        Result<std::uint64_t> size() const override { return data->size(); }
        Status read_at(std::uint64_t offset, MutableBytes out) const override {
            if (offset > data->size() || out.size() > data->size() - offset)
                return make_error(Errc::invalid_argument, "read past the end");
            if (!out.empty()) std::memcpy(out.data(), data->data() + offset, out.size());
            return {};
        }
    };
    auto m = std::make_shared<Memory>();
    m->data = &f.data;
    return std::shared_ptr<const RandomAccess>(m);
}

bool is_executable_magic(const std::uint8_t* p) {
    std::uint32_t magic = get_u32(p);
    return magic == 0x1D3D154F || magic == 0xEEF51454 || magic == 0x464C457F;
}

} // namespace

std::uint64_t inner_path_hash(const std::string& path) {
    std::string p = !path.empty() && path[0] == '/' ? path.substr(1) : path;
    for (char& c : p)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return hash_bytes(p);
}

std::vector<std::uint8_t> pfsv3_stored(const std::vector<std::uint8_t>& payload, int level, std::uint32_t block_size) {
    static const std::uint8_t kGitHash[20] = {0x23, 0x98, 0x7d, 0x16, 0xc9, 0x20, 0x9a, 0xc7, 0x28, 0x37,
                                              0x19, 0x32, 0x7e, 0x0f, 0x50, 0x6b, 0xbc, 0xf4, 0x59, 0xf4};
    static const std::uint8_t kShuffle[64] = {
        0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x01, 0x01, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x08, 0x02, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x06, 0x02, 0x02, 0x04, 0x00, 0x00,
        0x01, 0x01, 0x06, 0x01, 0x01, 0x06, 0x00, 0x00, 0x04, 0x04, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00,
    };
    constexpr std::size_t kHeader = 0x48, kDir = 16, kSections = 7;
    const std::size_t n = payload.size();
    const std::size_t blocks = n == 0 ? 1 : div_round_up(n, block_size);
    const std::size_t sec1 = sizeof kGitHash, sec2 = sizeof kShuffle, sec3 = (blocks + 1) * kDir, sec4 = blocks * 32,
                      sec5 = blocks * kDir;
    const std::size_t off1 = kHeader + kSections * kDir;
    const std::size_t off2 = align_up(off1 + sec1, 8), off3 = align_up(off2 + sec2, 8), off4 = align_up(off3 + sec3, 8),
                      off5 = align_up(off4 + sec4, 8), off6 = align_up(off5 + sec5, 8), off7 = align_up(off6, 0x400);
    std::vector<std::uint8_t> b(off7 + n, 0);
    put_u32(&b[0], 0x43534650); // 'PFSC'
    put_u16(&b[4], 3);
    put_u16(&b[6], kSections);
    put_u32(&b[8], block_size);
    put_u32(&b[0x0C], 0x0802);
    put_u64(&b[0x10], 2 | (static_cast<std::uint64_t>(static_cast<std::uint8_t>(level)) << 8) | (18ULL << 16));
    put_u64(&b[0x18], n);
    put_u64(&b[0x20], b.size());
    auto dir = [&](std::size_t i, std::uint16_t id, std::size_t off, std::size_t size) {
        std::size_t p = kHeader + i * kDir;
        put_u16(&b[p], id);
        put_u32(&b[p + 2], static_cast<std::uint32_t>(off));
        put_u32(&b[p + 10], static_cast<std::uint32_t>(size));
    };
    dir(0, 1, off1, sec1);
    dir(1, 2, off2, sec2);
    dir(2, 3, off3, sec3);
    dir(3, 4, off4, sec4);
    dir(4, 5, off5, sec5);
    dir(5, 6, off6, 0);
    dir(6, 7, off7, n);
    std::memcpy(&b[off1], kGitHash, sec1);
    std::memcpy(&b[off2], kShuffle, sec2);
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < blocks; ++i) {
        std::size_t size = n == 0 ? 0 : static_cast<std::size_t>(std::min<std::uint64_t>(block_size, n - cumulative));
        std::uint64_t flags = 0x0C | (size > block_size / 2 ? 0xC0 : 0);
        std::uint64_t hint = std::min<std::uint64_t>(size ? size - 1 : 0, 0x1FFFF);
        put_u64(&b[off3 + i * kDir], cumulative | (flags << 48));
        put_u64(&b[off3 + i * kDir + 8], cumulative | (hint << 44));
        crypto::Digest d = crypto::sha3_256(Bytes(payload.data() + cumulative, size));
        std::memcpy(&b[off4 + i * 32], d.data(), 32);
        if (size) std::memcpy(&b[off7 + cumulative], payload.data() + cumulative, size);
        cumulative += size;
    }
    put_u64(&b[off3 + blocks * kDir], n);
    put_u64(&b[off3 + blocks * kDir + 8], n);
    // File digest: SHA3-256(header32 || shuffle || boundaries || block hashes).
    std::uint8_t header32[32] = {};
    put_u32(header32, 1);
    std::memcpy(header32 + 4, &b[0x08], 8);
    std::memcpy(header32 + 16, &b[0x10], 16);
    crypto::Sha3_256 h;
    h.update(Bytes(header32, 32));
    h.update(Bytes(&b[off2], sec2));
    h.update(Bytes(&b[off3], sec3));
    h.update(Bytes(&b[off4], sec4));
    crypto::Digest digest = h.finish();
    std::memcpy(&b[0x28], digest.data(), 32);
    return b;
}

Result<InnerImage> write_inner_image(const std::vector<InnerFile>& files, const InnerOptions& options, ByteSink& sink,
                                     const ProgressFn& progress) {
    if (files.empty()) return make_error(Errc::invalid_argument, "a package needs at least one file");
    if (options.compression != Compression::stored)
        return make_error(Errc::unsupported, "Kraken-compressed packages are not supported yet");

    // ---- 1. The folder tree (paths in ordinal order). ----
    std::vector<const InnerFile*> sorted;
    for (const InnerFile& f : files) sorted.push_back(&f);
    std::stable_sort(sorted.begin(), sorted.end(), [](const InnerFile* a, const InnerFile* b) { return a->path < b->path; });
    auto uroot = std::make_unique<Dir>();
    uroot->name = "uroot";
    std::vector<std::unique_ptr<FileNode>> all_files;
    for (const InnerFile* in : sorted) {
        std::string path = !in->path.empty() && in->path[0] == '/' ? in->path.substr(1) : in->path;
        for (char c : path)
            if (static_cast<unsigned char>(c) > 0x7F || c == '\0')
                return make_error(Errc::invalid_argument, "package file names must be ASCII: " + path);
        if (path.empty() || path.back() == '/') return make_error(Errc::invalid_argument, "bad package path: " + in->path);
        Dir* d = uroot.get();
        std::size_t start = 0, slash;
        while ((slash = path.find('/', start)) != std::string::npos) {
            std::string part = path.substr(start, slash - start);
            if (part.empty()) return make_error(Errc::invalid_argument, "bad package path: " + in->path);
            auto& child = d->subdirs[part];
            if (!child) {
                child = std::make_unique<Dir>();
                child->name = part;
                child->full_path = path.substr(0, slash);
                child->parent = d;
            }
            d = child.get();
            start = slash + 1;
        }
        std::string name = path.substr(start);
        if (d->files.count(name) || d->subdirs.count(name))
            return make_error(Errc::invalid_argument, "the package lists a path twice: " + in->path);
        auto f = std::make_unique<FileNode>();
        f->name = name;
        f->full_path = "/" + path;
        f->in = in;
        f->length = in->length();
        f->parent = d;
        d->files[name] = f.get();
        all_files.push_back(std::move(f));
    }
    // Executable modules (by magic) are stored raw on whole blocks.
    for (auto& f : all_files) {
        if (f->length < 4) continue;
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, open_input(*f->in));
        std::uint8_t magic[4];
        ALEMBIC_TRY(in->read_at(0, MutableBytes(magic, 4)));
        f->exec = is_executable_magic(magic);
    }

    std::vector<Dir*> pre;
    dirs_pre_order(uroot.get(), pre);

    // ---- 2. Inodes: super root, three tables, folders (pre-order), files (post-order folders). ----
    std::uint32_t next = 4;
    for (Dir* d : pre) d->inode = next++;
    std::vector<Dir*> post;
    dirs_post_order(uroot.get(), post);
    std::vector<FileNode*> file_nodes;
    for (Dir* d : post)
        for (auto& [n, f] : d->files) {
            f->inode = next++;
            file_nodes.push_back(f);
        }

    // ---- 3. Afids: sce_sys files first by system rank, then the rest in entry order. ----
    std::vector<FileNode*> afid_order;
    Dir* sce_sys = uroot->subdirs.count("sce_sys") ? uroot->subdirs["sce_sys"].get() : nullptr;
    if (sce_sys) {
        std::vector<FileNode*> sce;
        files_pre_order(sce_sys, sce);
        std::stable_sort(sce.begin(), sce.end(), [](const FileNode* a, const FileNode* b) {
            int ra = system_rank(a), rb = system_rank(b);
            return ra != rb ? ra < rb : a->full_path < b->full_path;
        });
        afid_order.insert(afid_order.end(), sce.begin(), sce.end());
    }
    files_entry_order(uroot.get(), sce_sys, afid_order);
    std::vector<FileNode*> empty_files;
    for (FileNode* f : afid_order)
        if (f->length == 0) empty_files.push_back(f);
    std::erase_if(afid_order, [](const FileNode* f) { return f->length == 0; });

    // Sparse slots: 28 files in the first 256 KiB ublock, 30 in each later one;
    // a full ublock gets a 256 KiB hole (a null slot).
    std::vector<FileNode*> slots;
    std::vector<std::uint64_t> afid_offsets;
    std::uint64_t cursor = 0;
    std::int64_t current_ublock = -1;
    int in_ublock = 0;
    for (FileNode* f : afid_order) {
        auto ublock = static_cast<std::int64_t>(cursor / (4 * kBs));
        if (ublock != current_ublock) {
            current_ublock = ublock;
            in_ublock = 0;
        }
        if (in_ublock >= (current_ublock == 0 ? 28 : 30)) {
            afid_offsets.push_back(cursor);
            slots.push_back(nullptr);
            cursor += 4 * kBs;
            current_ublock = static_cast<std::int64_t>(cursor / (4 * kBs));
            in_ublock = 0;
        }
        f->afid = static_cast<std::uint32_t>(slots.size());
        f->logical = cursor;
        slots.push_back(f);
        afid_offsets.push_back(cursor);
        cursor += f->length;
        ++in_ublock;
    }
    const auto empty_afid = static_cast<std::uint32_t>(slots.size() + empty_files.size() + 1);
    for (FileNode* f : empty_files) f->afid = empty_afid;

    // Keystone, executable modules and PlayGo chunk changes start on a new block.
    {
        bool have_previous = false;
        std::uint8_t previous = 0;
        for (FileNode* f : afid_order) {
            bool transition = have_previous && previous != f->in->playgo_chunk;
            have_previous = true;
            previous = f->in->playgo_chunk;
            f->whole_block_raw = f->full_path == "/sce_sys/keystone" || f->exec || transition;
        }
    }

    // ---- 4. Payloads, in afid order. ----
    // Resuming, files and chunks before the checkpoint are only booked: their
    // bytes are already in the sink and their hashes come from the checkpoint.
    InnerImage result;
    Writer out(sink);
    const InnerProgress* from = options.resume_from;
    if (from) {
        if (from->file > afid_order.size() || from->finished.size() != from->file)
            return make_error(Errc::invalid_data, "the package checkpoint does not match the game's files");
        out.skip_to(from->written);
    }
    std::uint64_t pos = 0;
    std::uint32_t source_block = 0;
    std::uint64_t payload_total = 0, payload_done = 0, next_checkpoint = options.checkpoint_interval;
    for (FileNode* f : afid_order) payload_total += f->length;
    std::vector<std::uint8_t> buffer(kChunk);
    // The files' SHA3 runs beside the reading (the hasher copies each piece).
    crypto::BackgroundSha3 hasher(worker_count(options.threads) > 1, kChunk);
    for (std::size_t index = 0; index < afid_order.size(); ++index) {
        FileNode* f = afid_order[index];
        f->store_raw = true; // stored mode (Kraken would leave non-raw files compressible)
        bool align_before = f->whole_block_raw ||
                            (f->store_raw && pos % kBs != 0 && f->length > kBs - pos % kBs);
        if (align_before) pos = round_up(pos, kBs);
        f->on_disk = pos;
        const bool done_before = from && index < from->file;
        const bool current = from && index == from->file;
        const std::uint64_t skip = done_before ? f->length : current ? from->file_offset : 0;
        if (current && (skip > f->length || (skip % kChunk != 0 && skip != f->length) || from->written != pos + skip))
            return make_error(Errc::invalid_data, "the package checkpoint does not match the game's files");
        std::shared_ptr<const RandomAccess> in;
        if (skip < f->length) {
            if (skip == 0) ALEMBIC_TRY(out.zeros_to(pos)); // continuing mid-file, the padding is written already
            ALEMBIC_ASSIGN(in, open_input(*f->in));
            ALEMBIC_ASSIGN(std::uint64_t actual, in->size());
            if (actual != f->length) return make_error(Errc::invalid_data, "a file changed size: " + f->full_path);
        }
        std::uint64_t physical = pos;
        crypto::Sha3_256 hash = current ? crypto::Sha3_256::restore(from->hash.data()) : crypto::Sha3_256();
        // On an early return the helper must be done with `hash` before it goes.
        struct WaitFirst {
            crypto::BackgroundSha3& h;
            ~WaitFirst() { h.wait(); }
        } wait_first{hasher};
        for (std::uint64_t off = 0; off < f->length;) {
            auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(kChunk, f->length - off));
            if (off >= skip) {
                ALEMBIC_TRY(in->read_at(off, MutableBytes(buffer.data(), n)));
                ALEMBIC_TRY(out.write(Bytes(buffer.data(), n)));
                hasher.update(hash, Bytes(buffer.data(), n));
            }
            EncodedBlock e;
            e.source_block_index = source_block++;
            e.logical_offset = f->logical + off;
            e.uncompressed_length = n;
            e.stored_length = n;
            e.is_stored = true;
            e.physical_offset = physical;
            physical += n;
            result.encoded_blocks.push_back(e);
            off += n;
            payload_done += n;
            if (off <= skip) continue;
            // A stop (pause or cancel) saves a checkpoint at this very point too.
            const bool stop = progress && !progress({"Building the inner image", payload_done, payload_total});
            if (options.checkpoint && (stop || (options.checkpoint_interval && payload_done >= next_checkpoint))) {
                next_checkpoint = payload_done + options.checkpoint_interval;
                hasher.wait();
                InnerProgress p;
                p.written = out.pos();
                p.file = index;
                p.file_offset = off;
                p.source_block = source_block;
                p.finished.reserve(index);
                for (std::size_t k = 0; k < index; ++k) p.finished.push_back(afid_order[k]->sha3);
                p.hash = hash.save();
                ALEMBIC_TRY(options.checkpoint(p));
            }
            if (stop) return make_error(Errc::cancelled, "cancelled");
        }
        f->encoded = f->length;
        if (done_before) {
            f->sha3 = from->finished[index];
        } else {
            hasher.wait();
            f->sha3 = hash.finish();
        }
        pos += f->encoded;
        if (f->whole_block_raw) pos = round_up(pos, kBs);
    }
    if (from && from->file >= afid_order.size() && out.pos() < from->written)
        return make_error(Errc::invalid_data, "the package checkpoint does not match the game's files");
    result.raw_bytes = payload_total;
    result.encoded_bytes = payload_total;

    std::uint64_t data_end = 0;
    for (FileNode* f : afid_order) data_end = std::max(data_end, f->logical + f->length);
    for (FileNode* f : empty_files) f->logical = data_end;
    const std::uint64_t logical_data_blocks = round_up(std::max<std::uint64_t>(data_end, 1), 4 * kBs) / kBs;

    // ---- 5. Dirents. ----
    for (Dir* d : pre) {
        d->dirents.push_back({d->inode, kDirentDot, "."});
        d->dirents.push_back({d->parent ? d->parent->inode : d->inode, kDirentDotDot, ".."});
        for (auto& [n, c] : d->subdirs) d->dirents.push_back({c->inode, kDirentDirectory, n});
        for (auto& [n, f] : d->files) d->dirents.push_back({f->inode, kDirentFile, n});
        std::int32_t off = 0;
        std::map<std::string, std::int32_t> by_name;
        for (const Dirent& e : d->dirents) {
            by_name[e.name] = off;
            off += static_cast<std::int32_t>(dirent_size(e.name.size()));
        }
        for (auto& [n, c] : d->subdirs) c->dirent_offset = by_name[n];
        for (auto& [n, f] : d->files) f->dirent_offset = by_name[n];
    }

    // ---- 6. Metadata content and geometry. ----
    std::vector<FltEntry> inode_flt, apr_flt;
    for (Dir* d : pre)
        if (d != uroot.get())
            inode_flt.push_back({inner_path_hash(d->full_path), pack_inode_entry(d->inode, true, in_sce_sys(d), 0)});
    for (FileNode* f : file_nodes) {
        bool apr = f->full_path.rfind("/sce_sys/", 0) != 0;
        inode_flt.push_back({inner_path_hash(f->full_path), pack_inode_entry(f->inode, false, !apr, f->afid)});
        if (apr) apr_flt.push_back({inner_path_hash(f->full_path), pack_apr_entry(f->length, f->afid)});
    }
    std::vector<std::vector<std::uint8_t>> content;
    content.push_back(serialize_dirents({{1, kDirentFile, "inode_flat_path_table"},
                                         {2, kDirentFile, "apr_flat_path_table"},
                                         {3, kDirentFile, "afid_to_ino_table"},
                                         {uroot->inode, kDirentDirectory, "uroot"}}));
    content.push_back(serialize_flt(std::move(inode_flt)));
    content.push_back(serialize_flt(std::move(apr_flt)));
    {
        std::vector<std::uint8_t> t((slots.size() + 3) * 4);
        put_u32(&t[0], static_cast<std::uint32_t>(slots.size() + 2));
        for (std::size_t i = 0; i < slots.size(); ++i)
            put_u32(&t[4 + i * 4], slots[i] ? slots[i]->inode : 0xFFFFFFFFu);
        put_u32(&t[4 + slots.size() * 4], 0xFFFFFFFFu);
        put_u32(&t[8 + slots.size() * 4], 0xFFFFFFFFu);
        content.push_back(std::move(t));
    }
    for (Dir* d : pre) content.push_back(serialize_dirents(d->dirents));

    const std::uint64_t node_count = 4 + pre.size() + file_nodes.size();
    if (node_count > 0x7FFFFFFF) return make_error(Errc::too_large, "too many files for a package");
    const std::uint64_t inode_blocks = div_round_up(node_count, kInodesPerBlock);
    std::uint64_t content_blocks = 0;
    for (const auto& c : content) content_blocks += allocated_blocks(c.size());
    const std::uint64_t natural = 1 + inode_blocks + content_blocks;
    if (natural & 1) content.emplace_back(); // pad to an even block count
    const std::uint64_t meta_base_block = logical_data_blocks + kMetadataReserveBlocks;
    const std::uint64_t meta_base = meta_base_block * kBs;
    const std::uint64_t ndblock = meta_base_block + natural + (natural & 1);
    std::vector<std::uint64_t> content_offsets(content.size());
    {
        std::uint64_t c = meta_base + (1 + inode_blocks) * kBs;
        for (std::size_t i = 0; i < content.size(); ++i) {
            content_offsets[i] = c;
            c += allocated_blocks(content[i].size()) * kBs;
        }
    }
    auto block_size_of = [&](std::size_t i) { return allocated_blocks(content[i].size()) * kBs; };

    // Nodes, inode order.
    std::vector<MetaNode> nodes;
    auto internal = [&](std::uint32_t ino, const char* name, bool dir, std::uint64_t size) {
        MetaNode n;
        n.name = name;
        n.inode = ino;
        n.is_directory = dir;
        n.mode = dir ? 0x416D : 0x816D;
        n.flags = 0x00020010;
        n.size = size;
        n.logical_offset = content_offsets[ino];
        nodes.push_back(n);
    };
    internal(0, "", true, block_size_of(0));
    internal(1, "inode_flat_path_table", false, content[1].size());
    internal(2, "apr_flat_path_table", false, content[2].size());
    internal(3, "afid_to_ino_table", false, content[3].size());
    for (std::size_t j = 0; j < pre.size(); ++j) {
        Dir* d = pre[j];
        bool is_uroot = d == uroot.get();
        bool sce = !is_uroot && in_sce_sys(d);
        MetaNode n;
        n.name = d->name;
        n.inode = d->inode;
        n.is_directory = true;
        n.mode = sce ? 0x4168 : 0x416D;
        n.nlink = static_cast<std::uint16_t>(2 + d->subdirs.size() + (is_uroot ? 1 : 0));
        n.flags = sce ? 0x00020010u : 0x00000010u;
        n.size = block_size_of(4 + j);
        n.logical_offset = content_offsets[4 + j];
        n.parent_inode = is_uroot ? -1 : static_cast<std::int32_t>(d->parent->inode);
        n.dirent_offset = is_uroot ? -1 : d->dirent_offset;
        nodes.push_back(n);
    }
    for (FileNode* f : file_nodes) {
        bool sce = f->full_path.rfind("/sce_sys/", 0) == 0;
        MetaNode n;
        n.name = f->name;
        n.full_path = f->full_path;
        n.inode = f->inode;
        n.is_apr = !sce;
        n.mode = sce ? 0x8168 : 0x816D;
        n.flags = 0x10u | (f->exec ? 0x40u : 0x20u) | (sce ? 0x20000u : 0u);
        n.afid = f->afid;
        n.size = f->length;
        n.logical_offset = f->logical;
        n.parent_inode = static_cast<std::int32_t>(f->parent->inode);
        n.dirent_offset = f->dirent_offset;
        nodes.push_back(n);
    }

    // ---- 7. Metadata plaintext: superblock, inode table, content blocks. ----
    std::uint64_t total_content = 0;
    for (const auto& c : content) total_content += allocated_blocks(c.size());
    std::vector<std::uint8_t>& meta = result.metadata_plaintext;
    meta.assign(static_cast<std::size_t>((1 + inode_blocks + total_content) * kBs), 0);
    {
        std::uint8_t* sb = meta.data();
        put_u64(sb, 2);
        put_u64(sb + 8, 20130315);
        sb[0x1A] = 1;
        put_u16(sb + 0x1C, 0x18);
        put_u32(sb + 0x20, kInnerBlock);
        put_u64(sb + 0x28, 1);
        put_u64(sb + 0x30, node_count);
        put_u64(sb + 0x38, ndblock);
        put_u64(sb + 0x40, inode_blocks);
        put_u32(sb + 0x50, kInnerBlock);
        put_u32(sb + 0x54, 0x10);
        put_u32(sb + 0x58, kInnerBlock);
        put_u32(sb + 0x60, kInnerBlock);
        for (int t = 0; t < 4; ++t) put_u64(sb + 0x68 + t * 8, static_cast<std::uint64_t>(options.time_seconds));
        for (int t = 0; t < 4; ++t) put_u32(sb + 0x88 + t * 4, options.time_nanoseconds);
        put_u64(sb + 0xB0, 1);
        put_u64(sb + 0xD8, meta_base_block + 1);
        sb[0x368] = 1;
    }
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const MetaNode& n = nodes[i];
        std::uint8_t* d = meta.data() + (1 + i / kInodesPerBlock) * kBs + (i % kInodesPerBlock) * kInodeSize;
        put_u16(d, n.mode);
        put_u16(d + 2, n.nlink);
        put_u32(d + 4, n.flags);
        put_u64(d + 8, n.size);
        put_u64(d + 0x10, n.size);
        for (int t = 0; t < 4; ++t) put_u64(d + 0x18 + t * 8, static_cast<std::uint64_t>(options.time_seconds));
        for (int t = 0; t < 4; ++t) put_u32(d + 0x38 + t * 4, options.time_nanoseconds);
        put_u64(d + 0x60, n.logical_offset);
        std::int32_t db1 = n.parent_inode < 0 ? -1 : (n.is_directory ? -1 : static_cast<std::int32_t>(n.afid));
        put_u32(d + 0x68, static_cast<std::uint32_t>(db1));
        put_u32(d + 0x6C, static_cast<std::uint32_t>(n.parent_inode));
        put_u32(d + 0x70, static_cast<std::uint32_t>(n.dirent_offset));
    }
    {
        std::uint64_t c = (1 + inode_blocks) * kBs;
        for (const auto& block : content) {
            if (!block.empty()) std::memcpy(meta.data() + c, block.data(), block.size());
            c += allocated_blocks(block.size()) * kBs;
        }
    }

    // ---- 8. Block-info table and the stored metadata container. ----
    std::vector<std::uint8_t> info = block_info_table(data_end);
    pos = round_up(pos, kBs);
    result.block_info_on_disk_offset = pos;
    ALEMBIC_TRY(out.zeros_to(pos));
    ALEMBIC_TRY(out.write(info));
    pos += info.size();
    result.container = pfsv3_stored(meta, options.kraken_level);
    for (std::size_t i = 0; i < meta.size(); i += kContainerBlock) {
        auto size = static_cast<std::uint32_t>(std::min<std::size_t>(kContainerBlock, meta.size() - i));
        result.metadata_blocks.push_back({size, size, false, 0, 0});
    }
    pos = round_up(pos, kBs);
    result.metadata_on_disk_offset = pos;
    ALEMBIC_TRY(out.zeros_to(pos));
    ALEMBIC_TRY(out.write(result.container));
    pos += result.container.size();
    result.image_length = pos;

    result.ndblock = ndblock;
    result.meta_base_logical = ndblock * kBs - meta.size();
    result.data_end_logical = data_end;
    result.inode_count = static_cast<std::uint32_t>(nodes.size());
    for (const MetaNode& n : nodes) {
        if (n.parent_inode >= 0) ++result.inner_content_inodes;
        if (!n.is_directory && n.inode >= 4 && n.full_path.rfind("/sce_sys/", 0) != 0) ++result.app_file_count;
    }
    for (FileNode* f : afid_order)
        result.placements.push_back({f->afid, f->on_disk, f->logical, f->encoded, f->length, f->store_raw, f->sha3});
    result.afid_logical_offsets = std::move(afid_offsets);
    result.nodes = std::move(nodes);
    return result;
}

} // namespace alembic::fpkg::build
