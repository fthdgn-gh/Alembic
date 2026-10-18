// SPDX-License-Identifier: GPL-3.0-or-later
// The outer PFS of a debug package: pfs_image.dat and naps_pkg_layout.dat in a
// signed, AES-XTS-encrypted PFS. Ported from pearlxcore/ProsperoPkgTool (MIT)
// Containers/ProsperoOuterPfsBuilder.cs (BuildPlaintext, Encrypt,
// ComputeImageDigests) and Crypto/ProsperoKeys.cs (DeriveOuterPfsXtsKeys).
//
// Layout, data first: [file blocks][superblock (plaintext)][inode table]
// [super-root dirents][flat path table][indirect blocks][uroot dirents].
// Here the file blocks are hashed and encrypted as they are streamed in; the
// metadata blocks are built from those hashes in finish().
#include "alembic/fpkg_build.hpp"

#include "alembic/bytes.hpp"
#include "alembic/fpkg.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>

namespace alembic::fpkg::build {

namespace {

constexpr std::uint32_t kBs = kInnerBlock;
constexpr std::uint64_t kSignedSector = 0x800000000000ULL;
constexpr std::uint32_t kS32InodeSize = 0x2C8;
constexpr int kDirectSlots = 12;
constexpr int kLevels = 5;
constexpr std::uint32_t kSigStride = 36;
constexpr std::uint32_t kPerIndirect = kBs / kSigStride;
constexpr std::uint32_t kDirectOffset = 0x64;
constexpr std::uint32_t kIndirectOffset = 0x214;
constexpr std::uint32_t kMetadataInodes = 3;

std::uint64_t capacity(int depth) {
    std::uint64_t c = 1;
    for (int i = 0; i < depth; ++i) c *= kPerIndirect;
    return c;
}

struct TreeNode {
    std::uint32_t block = 0;
    int depth = 0;
    std::uint64_t first_data = 0; // offset within the file, in blocks
    std::uint64_t data_count = 0;
    std::vector<std::unique_ptr<TreeNode>> children;
};

std::unique_ptr<TreeNode> allocate(int depth, std::uint64_t first, std::uint64_t count, std::uint32_t& next) {
    auto node = std::make_unique<TreeNode>();
    node->block = next++;
    node->depth = depth;
    node->first_data = first;
    node->data_count = count;
    if (depth > 1) {
        std::uint64_t child_capacity = capacity(depth - 1);
        for (std::uint64_t left = count, off = first; left > 0;) {
            std::uint64_t take = std::min(left, child_capacity);
            node->children.push_back(allocate(depth - 1, off, take, next));
            off += take;
            left -= take;
        }
    }
    return node;
}

void put_dirent(std::vector<std::uint8_t>& block, std::size_t& pos, std::uint32_t inode, std::uint32_t type,
                const std::string& name) {
    std::size_t size = align_up(name.size() + 17, 8);
    put_u32(&block[pos], inode);
    put_u32(&block[pos + 4], type);
    put_u32(&block[pos + 8], static_cast<std::uint32_t>(name.size()));
    put_u32(&block[pos + 12], static_cast<std::uint32_t>(size));
    std::memcpy(&block[pos + 16], name.data(), name.size());
    pos += size;
}

void stamp_times(std::uint8_t* inode, std::int64_t sec, std::uint32_t nsec) {
    for (int i = 0; i < 4; ++i) put_u64(inode + 24 + i * 8, static_cast<std::uint64_t>(sec));
    for (int i = 0; i < 4; ++i) put_u32(inode + 56 + i * 4, nsec);
}

} // namespace

std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>> outer_xts_keys(const std::string& content_id,
                                                                                     const std::string& passcode,
                                                                                     const std::uint8_t seed[16]) {
    crypto::Digest ekpfs = compute_keys(content_id, passcode, 1);
    crypto::Digest inner = crypto::hmac_sha256(Bytes(ekpfs.data(), 32), Bytes(seed, 16));
    std::uint8_t message[20];
    put_u32(message, 1);
    std::memcpy(message + 4, seed, 16);
    crypto::Digest key = crypto::hmac_sha256(Bytes(inner.data(), 32), Bytes(message, sizeof message));
    std::pair<std::array<std::uint8_t, 16>, std::array<std::uint8_t, 16>> out;
    std::memcpy(out.first.data(), key.data(), 16);       // tweak key
    std::memcpy(out.second.data(), key.data() + 16, 16); // data key
    return out;
}

OuterPfsWriter::OuterPfsWriter(ByteSink& out, const OuterOptions& options)
    : out_(out), options_(options), block_(kBs), scratch_(kBs) {
    auto keys = outer_xts_keys(options.content_id, options.passcode, options.seed.data());
    tweak_ = std::make_unique<crypto::Aes128>(keys.first.data());
    data_ = std::make_unique<crypto::Aes128>(keys.second.data());
}

OuterPfsWriter::~OuterPfsWriter() = default;

Status OuterPfsWriter::emit_block(const std::uint8_t* plain, Kind kind) {
    if (blocks_.size() >= 0x7FFFFFFF) return make_error(Errc::too_large, "the outer PFS has too many blocks");
    auto index = static_cast<std::uint32_t>(blocks_.size());
    blocks_.push_back(crypto::sha3_256(Bytes(plain, kBs)));
    if (kind == Kind::plaintext) return out_.write(Bytes(plain, kBs));
    std::memcpy(scratch_.data(), plain, kBs);
    std::uint64_t sector = kind == Kind::signed_block ? (index | kSignedSector) : index;
    crypto::xts_encrypt(*data_, *tweak_, sector, MutableBytes(scratch_.data(), kBs));
    return out_.write(Bytes(scratch_.data(), kBs));
}

Status OuterPfsWriter::begin_file(const std::string& name, bool signed_blocks) {
    if (open_) return make_error(Errc::invalid_argument, "outer PFS: a file is still open");
    for (char c : name)
        if (static_cast<unsigned char>(c) > 0x7F) return make_error(Errc::invalid_argument, "outer PFS names are ASCII");
    File f;
    f.name = name;
    f.signed_blocks = signed_blocks;
    f.first_block = static_cast<std::uint32_t>(blocks_.size());
    files_.push_back(f);
    fill_ = 0;
    open_ = true;
    return {};
}

Status OuterPfsWriter::write(Bytes data) {
    if (!open_) return make_error(Errc::invalid_argument, "outer PFS: no file is open");
    File& f = files_.back();
    f.size += data.size();
    while (!data.empty()) {
        std::size_t n = std::min<std::size_t>(kBs - fill_, data.size());
        std::memcpy(block_.data() + fill_, data.data(), n);
        fill_ += n;
        data = data.subspan(n);
        if (fill_ == kBs) {
            ALEMBIC_TRY(emit_block(block_.data(), f.signed_blocks ? Kind::signed_block : Kind::data));
            fill_ = 0;
        }
    }
    return {};
}

Status OuterPfsWriter::end_file(std::uint64_t size_compressed) {
    if (!open_) return make_error(Errc::invalid_argument, "outer PFS: no file is open");
    File& f = files_.back();
    if (fill_ > 0 || f.size == 0) { // the last partial block, or one zero block for an empty file
        std::fill(block_.begin() + static_cast<std::ptrdiff_t>(fill_), block_.end(), 0);
        ALEMBIC_TRY(emit_block(block_.data(), f.signed_blocks ? Kind::signed_block : Kind::data));
        fill_ = 0;
    }
    f.block_count = static_cast<std::uint32_t>(blocks_.size()) - f.first_block;
    f.size_compressed = size_compressed;
    open_ = false;
    return {};
}

Result<OuterPfs> OuterPfsWriter::finish() {
    if (open_ || files_.empty()) return make_error(Errc::invalid_argument, "outer PFS: nothing to finish");
    const auto data_blocks = static_cast<std::uint32_t>(blocks_.size());
    const std::uint32_t superblock = data_blocks, inode_table = data_blocks + 1, super_root = data_blocks + 2,
                        flt = data_blocks + 3;
    if ((kMetadataInodes + files_.size()) * kS32InodeSize > kBs)
        return make_error(Errc::too_large, "too many outer PFS files for one inode block");

    // Indirect trees for files longer than 12 blocks, allocated after the path table.
    std::uint32_t next = flt + 1;
    std::vector<std::vector<std::pair<int, std::unique_ptr<TreeNode>>>> plans(files_.size());
    for (std::size_t i = 0; i < files_.size(); ++i) {
        std::uint64_t excess = files_[i].block_count > kDirectSlots ? files_[i].block_count - kDirectSlots : 0;
        std::uint64_t first = kDirectSlots;
        for (int level = 0; level < kLevels && excess > 0; ++level) {
            std::uint64_t take = std::min<std::uint64_t>(excess, capacity(level + 1));
            plans[i].push_back({level, allocate(level + 1, first, take, next)});
            first += take;
            excess -= take;
        }
        if (excess > 0) return make_error(Errc::too_large, "an outer PFS file is too large");
    }
    const std::uint32_t uroot = next;
    const std::uint32_t total = uroot + 1;

    // Metadata blocks, kept until written (index -> plaintext).
    std::vector<std::vector<std::uint8_t>> meta(total - superblock);
    auto block_at = [&](std::uint32_t index) -> std::vector<std::uint8_t>& {
        auto& b = meta[index - superblock];
        if (b.empty()) b.assign(kBs, 0);
        return b;
    };
    std::vector<crypto::Digest> meta_digest(total - superblock);
    auto digest_of = [&](std::uint32_t index) -> const crypto::Digest& {
        return index < data_blocks ? blocks_[index] : meta_digest[index - superblock];
    };
    auto seal = [&](std::uint32_t index) { meta_digest[index - superblock] = crypto::sha3_256(block_at(index)); };

    {
        auto& b = block_at(super_root);
        std::size_t pos = 0;
        put_dirent(b, pos, 1, 2, "inode_flat_path_table");
        put_dirent(b, pos, 2, 3, "uroot");
        seal(super_root);
    }
    {
        auto& b = block_at(flt);
        put_u32(&b[0], 1);
        put_u32(&b[4], 0x10);
        put_u32(&b[8], 0x40);
        b[0x20] = 0x7F;
        b[0x21] = 'F';
        b[0x22] = 'L';
        b[0x23] = 'T';
        put_u32(&b[0x2C], static_cast<std::uint32_t>(files_.size()));
        put_u64(&b[0x30], 0x92CA8AAB26A24F51ULL);
        put_u64(&b[0x38], 0x09BBB761A41BC44DULL);
        for (std::size_t i = 0; i < files_.size(); ++i) {
            put_u64(&b[0x40 + i * 16], inner_path_hash(files_[i].name));
            put_u64(&b[0x48 + i * 16], (kMetadataInodes + i) | (static_cast<std::uint64_t>(i) << 40));
        }
        seal(flt);
    }
    {
        auto& b = block_at(uroot);
        std::size_t pos = 0;
        put_dirent(b, pos, 2, 4, ".");
        put_dirent(b, pos, 2, 5, "..");
        for (std::size_t i = 0; i < files_.size(); ++i)
            put_dirent(b, pos, static_cast<std::uint32_t>(kMetadataInodes + i), 2, files_[i].name);
        seal(uroot);
    }

    // Indirect nodes: children first, each entry {SHA3 of the block, block index}.
    std::function<crypto::Digest(const TreeNode&, std::uint32_t)> serialize = [&](const TreeNode& n, std::uint32_t file_first) {
        auto& b = block_at(n.block);
        if (n.depth == 1) {
            for (std::uint64_t i = 0; i < n.data_count; ++i) {
                auto data_block = static_cast<std::uint32_t>(file_first + n.first_data + i);
                std::memcpy(&b[i * kSigStride], digest_of(data_block).data(), 32);
                put_u32(&b[i * kSigStride + 32], data_block);
            }
        } else {
            for (std::size_t j = 0; j < n.children.size(); ++j) {
                crypto::Digest h = serialize(*n.children[j], file_first);
                std::memcpy(&b[j * kSigStride], h.data(), 32);
                put_u32(&b[j * kSigStride + 32], n.children[j]->block);
            }
        }
        seal(n.block);
        return meta_digest[n.block - superblock];
    };

    // Inode table: super root, path table, uroot, then the files.
    {
        auto& t = block_at(inode_table);
        auto meta_inode = [&](std::uint32_t number, std::uint16_t mode, std::uint16_t nlink, std::uint32_t flags,
                              std::uint64_t size, std::uint32_t owned) {
            std::uint8_t* d = &t[number * kS32InodeSize];
            put_u16(d, mode);
            put_u16(d + 2, nlink);
            put_u32(d + 4, flags);
            put_u64(d + 8, size);
            put_u64(d + 16, size);
            stamp_times(d, options_.time_seconds, options_.time_nanoseconds);
            put_u32(d + 96, 1);
            std::memcpy(d + kDirectOffset, digest_of(owned).data(), 32);
            put_u32(d + kDirectOffset + 32, owned);
        };
        meta_inode(0, 0x416D, 1, 0x2000C, kBs, super_root);
        meta_inode(1, 0x816D, 1, 0x2000C, 0x40 + files_.size() * 16, flt);
        meta_inode(2, 0x416D, 3, 0x000C, kBs, uroot);
        for (std::size_t i = 0; i < files_.size(); ++i) {
            const File& f = files_[i];
            std::uint8_t* d = &t[(kMetadataInodes + i) * kS32InodeSize];
            put_u16(d, 0x816D);
            put_u16(d + 2, 1);
            put_u32(d + 4, 0x000D);
            put_u64(d + 8, f.size);
            put_u64(d + 16, f.size_compressed);
            stamp_times(d, options_.time_seconds, options_.time_nanoseconds);
            put_u32(d + 96, f.block_count);
            for (std::uint32_t j = 0; j < std::min<std::uint32_t>(f.block_count, kDirectSlots); ++j) {
                std::memcpy(d + kDirectOffset + j * kSigStride, digest_of(f.first_block + j).data(), 32);
                put_u32(d + kDirectOffset + j * kSigStride + 32, f.first_block + j);
            }
            for (auto& [level, root] : plans[i]) {
                crypto::Digest h = serialize(*root, f.first_block);
                std::memcpy(d + kIndirectOffset + level * kSigStride, h.data(), 32);
                put_u32(d + kIndirectOffset + level * kSigStride + 32, root->block);
            }
        }
        seal(inode_table);
    }

    // Superblock (plaintext).
    {
        auto& s = block_at(superblock);
        put_u64(&s[0], 2);
        put_u64(&s[8], 20130315);
        s[0x1A] = 1;
        put_u16(&s[0x1C], 0x0D);
        put_u32(&s[0x20], kBs);
        put_u64(&s[0x28], 1);
        put_u64(&s[0x30], kMetadataInodes + files_.size());
        put_u64(&s[0x38], total);
        put_u64(&s[0x40], 1);
        std::uint8_t* sig = &s[0x50];
        put_u16(sig + 2, 1);
        put_u64(sig + 8, kBs);
        put_u64(sig + 16, kBs);
        stamp_times(sig, options_.time_seconds, options_.time_nanoseconds);
        put_u32(sig + 96, 1);
        std::memcpy(sig + 104, meta_digest[inode_table - superblock].data(), 32);
        put_u64(sig + 136, inode_table);
        put_u32(&s[0x36C], 1);
        std::memcpy(&s[0x370], options_.seed.data(), 16);
        std::vector<std::uint8_t> region(s.begin(), s.begin() + 0x5A0);
        std::fill(region.begin() + 0x380, region.begin() + 0x3A0, 0);
        crypto::Digest icv = crypto::sha3_256(region);
        std::memcpy(&s[0x380], icv.data(), 32);
    }

    OuterPfs result;
    result.superblock_plaintext = block_at(superblock);
    {
        std::vector<std::uint8_t> region(result.superblock_plaintext.begin(), result.superblock_plaintext.begin() + 0x5A0);
        std::fill(region.begin() + 0x380, region.begin() + 0x3A0, 0);
        result.superblock_icv = crypto::sha3_256(region);
    }
    for (std::uint32_t index = superblock; index < total; ++index) {
        Kind kind = index == superblock ? Kind::plaintext : Kind::signed_block;
        ALEMBIC_TRY(emit_block(block_at(index).data(), kind));
    }
    result.block_count = total;
    result.superblock_index = superblock;
    result.block_digests = std::move(blocks_);
    for (const File& f : files_) result.files.push_back({f.name, f.first_block, f.block_count, f.size, f.size_compressed});
    return result;
}

} // namespace alembic::fpkg::build
