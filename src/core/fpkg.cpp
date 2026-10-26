// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from pearlxcore/ProsperoPkgTool (MIT): Containers/ProsperoPackageReader.cs,
// Containers/OuterPfsReader.cs, Containers/ProsperoPackageContent.cs,
// Crypto/ProsperoKeys.cs, Crypto/ProsperoEntryCipher.cs.
#include "alembic/fpkg.hpp"

#include "alembic/bytes.hpp"

#include <algorithm>
#include <cstring>

namespace alembic::fpkg {

namespace {

constexpr std::uint64_t kFihRegion = 0x10000;
constexpr std::size_t kCntHeaderSize = 0x5A0;
constexpr std::uint64_t kPfsMagic = 20130315;
constexpr std::uint64_t kSignedSector = 0x800000000000ull;
constexpr std::size_t kSeedOffset = 0x370;
constexpr std::uint16_t kPfsMode64BitInodes = 0x2;
constexpr std::uint16_t kPfsModeEncrypted = 0x4;
constexpr std::size_t kIcvOffset = 0x380;
constexpr std::size_t kIcvRegion = 0x5A0;
constexpr std::size_t kInodeSize = 0x2C8;
constexpr std::size_t kDirectOffset = 0x64;
constexpr std::size_t kIndirectOffset = 0x214;
constexpr std::size_t kSigStride = 36; // 32-byte digest + u32 block index
constexpr std::size_t kTreeEntries = kBlockSize / kSigStride;
const char kPlainSeed[16] = {'P', 'P', 'R', 'P', 'L', 'A', 'I', 'N', '-', 'N', 'O', 'A', 'U', 'T', 'H', '!'};

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint16_t be16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}

bool in_range(std::uint64_t offset, std::uint64_t size, std::uint64_t total) {
    return offset <= total && size <= total - offset;
}

const char* known_entry_name(std::uint32_t id) {
    switch (id) {
    case 0x00000001: return "digests";
    case 0x00000010: return "entry-keys";
    case 0x00000020: return "image-key";
    case 0x00000080: return "general-digests";
    case 0x00000100: return "metas";
    case 0x00000200: return "entry-names";
    case 0x00000400: return "license.dat";
    case 0x00000401: return "license.info";
    case 0x00000402: return "nptitle.dat";
    case 0x00000403: return "npbind.dat";
    case 0x00000404: return "selfinfo.dat";
    case 0x00000406: return "imageinfo.dat";
    case 0x00000407: return "target-deltainfo.dat";
    case 0x00000408: return "origin-deltainfo.dat";
    case 0x00000409: return "psreserved.dat";
    case 0x0000040A: return "imagedigs.dat";
    case 0x00001000: return "param.sfo";
    case 0x00001001: return "playgo-chunk.dat";
    case 0x00001004: return "pronunciation.xml";
    case 0x00001005: return "pronunciation.sig";
    case 0x00001006: return "pic1.png";
    case 0x00001007: return "pubtoolinfo.dat";
    case 0x0000100B: return "shareparam.json";
    case 0x0000100C: return "shareoverlayimage.png";
    case 0x0000100D: return "save_data.png";
    case 0x0000100E: return "shareprivacyguardimage.png";
    case 0x00003000: return "playgo-scenario.json";
    case 0x00001200: return "icon0.png";
    case 0x00001220: return "pic0.png";
    case 0x00001240: return "snd0.at9";
    case 0x00001280: return "icon0.dds";
    case 0x000012A0: return "pic0.dds";
    case 0x000012C0: return "pic1.dds";
    case 0x00002000: return "param.json";
    case 0x00002010: return "playgo-hash-table.dat";
    case 0x00002011: return "playgo-ficm.dat";
    case 0x00002060: return "pic2.dds";
    default: return "";
    }
}

Status read_cnt(const RandomAccess& file, Package& p, std::uint64_t cnt_offset, std::uint64_t& logical_size) {
    if (!in_range(cnt_offset, kCntHeaderSize, p.file_size)) return invalid("the CNT header lies outside the package");
    std::uint8_t h[kCntHeaderSize];
    ALEMBIC_TRY(file.read_at(cnt_offset, MutableBytes(h, sizeof h)));
    if (std::memcmp(h, "\x7F" "CNT", 4) != 0) return invalid("the embedded container has no CNT magic");
    p.cnt_version = be16(h + 0x04);
    p.cnt_flags = be16(h + 0x06);
    std::uint32_t entry_count = be32(h + 0x10);
    p.entry_table_offset = be32(h + 0x18);
    p.body_offset = be64(h + 0x20);
    p.body_size = be64(h + 0x28);
    std::size_t id_length = 0;
    while (id_length < 0x24 && h[0x40 + id_length] != 0) ++id_length;
    p.content_id.assign(reinterpret_cast<const char*>(h + 0x40), id_length);
    p.drm_type = be32(h + 0x70);
    p.content_type = be32(h + 0x74);
    p.content_flags = be32(h + 0x78);

    std::uint64_t available = p.file_size - cnt_offset;
    if (entry_count > 0x10000) return invalid("the CNT entry count exceeds the safety limit");
    std::uint64_t table_size = std::uint64_t(entry_count) * 0x20;
    if (!in_range(p.entry_table_offset, table_size, available)) return invalid("the CNT entry table lies outside the package");
    std::vector<std::uint8_t> table(static_cast<std::size_t>(table_size));
    ALEMBIC_TRY(file.read_at(cnt_offset + p.entry_table_offset, table));

    logical_size = kCntHeaderSize;
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const std::uint8_t* r = table.data() + i * 0x20;
        CntEntry e;
        e.id = be32(r);
        e.name_offset = be32(r + 4);
        e.flags1 = be32(r + 8);
        e.flags2 = be32(r + 12);
        e.data_offset = be32(r + 16);
        e.data_size = be32(r + 20);
        std::memcpy(e.row, r, 32);
        if (!in_range(e.data_offset, e.data_size, available)) {
            p.warnings.push_back("CNT entry " + std::to_string(e.id) + " data lies outside the package");
        } else {
            logical_size = std::max<std::uint64_t>(logical_size, std::uint64_t(e.data_offset) + e.data_size);
        }
        p.entries.push_back(e);
    }

    // Names from entry 0x200.
    auto names = std::find_if(p.entries.begin(), p.entries.end(), [](const CntEntry& e) { return e.id == 0x200; });
    std::vector<std::uint8_t> blob;
    if (names != p.entries.end() && names->data_size > 0 && names->data_size <= 16u * 1024 * 1024 &&
        in_range(names->data_offset, names->data_size, available)) {
        blob.resize(names->data_size);
        ALEMBIC_TRY(file.read_at(cnt_offset + names->data_offset, blob));
    }
    for (CntEntry& e : p.entries) {
        if (!blob.empty() && e.name_offset != UINT32_MAX && e.name_offset < blob.size()) {
            const auto* start = blob.data() + e.name_offset;
            const auto* end = static_cast<const std::uint8_t*>(std::memchr(start, 0, blob.size() - e.name_offset));
            if (end && end > start) e.name.assign(reinterpret_cast<const char*>(start), end - start);
        }
        if (e.name.empty()) e.name = known_entry_name(e.id);
    }
    return {};
}

} // namespace

const char* name(Kind kind) {
    switch (kind) {
    case Kind::metadata_container: return "metadata container (CNT)";
    case Kind::finalized_debug: return "finalized debug image (FIH)";
    case Kind::finalized_retail: return "finalized retail image (FIH)";
    case Kind::patch_debug: return "debug patch image (LIH)";
    case Kind::ps4_package: return "PS4 package (CNT)";
    }
    return "?";
}

Result<Package> read_package(const RandomAccess& file) {
    Package p;
    ALEMBIC_ASSIGN(p.file_size, file.size());
    if (p.file_size < 4) return invalid("the file is too small to be a PS5 package");
    std::uint8_t magic[4];
    ALEMBIC_TRY(file.read_at(0, MutableBytes(magic, 4)));

    if (std::memcmp(magic, "\x7F" "CNT", 4) == 0) {
        p.kind = Kind::metadata_container;
        std::uint64_t logical = 0;
        ALEMBIC_TRY(read_cnt(file, p, 0, logical));
        // A PS4 package: the CNT header points at a PFS image inside the file.
        std::uint8_t h[0x20];
        ALEMBIC_TRY(file.read_at(0x400, MutableBytes(h, sizeof h)));
        std::uint32_t image_count = be32(h);
        std::uint64_t pfs_offset = be64(h + 0x10), pfs_size = be64(h + 0x18);
        if (image_count >= 1 && pfs_offset >= kCntHeaderSize && pfs_size >= kBlockSize &&
            in_range(pfs_offset, pfs_size, p.file_size)) {
            std::uint8_t sb[0x10];
            ALEMBIC_TRY(file.read_at(pfs_offset, MutableBytes(sb, sizeof sb)));
            if (get_u64(sb + 8) == kPfsMagic) {
                p.kind = Kind::ps4_package;
                p.pfs_offset = pfs_offset;
                p.pfs_size = pfs_size;
                p.superblock_hint = 0;
                p.segments = {{"CNT", 0, pfs_offset}, {"PFS", pfs_offset, pfs_size}};
                if (pfs_offset + pfs_size < p.file_size)
                    p.segments.push_back({"tail", pfs_offset + pfs_size, p.file_size - pfs_offset - pfs_size});
                return p;
            }
        }
        p.segments.push_back({"CNT", 0, p.file_size});
        return p;
    }

    if (std::memcmp(magic, "\x7F" "LIH", 4) == 0) {
        if (p.file_size < kFihRegion) return invalid("the patch image is smaller than its LIH region");
        std::uint8_t lih[0x50];
        ALEMBIC_TRY(file.read_at(0, MutableBytes(lih, sizeof lih)));
        p.kind = Kind::patch_debug;
        p.fih_offset = get_u32(lih + 0x10);
        std::uint64_t fih_size = get_u64(lih + 0x28);
        std::uint64_t cnt_offset = get_u64(lih + 0x30), cnt_size = get_u64(lih + 0x38);
        std::uint64_t si_offset = get_u64(lih + 0x40), si_size = get_u64(lih + 0x48);
        if (!in_range(p.fih_offset, fih_size, p.file_size) || !in_range(cnt_offset, cnt_size, p.file_size) ||
            !in_range(si_offset, si_size, p.file_size) || cnt_offset < p.fih_offset + fih_size)
            return invalid("the LIH segments lie outside the package");
        p.pfs_offset = p.fih_offset + fih_size;
        p.pfs_size = cnt_offset - p.pfs_offset;
        p.cnt_offset = cnt_offset;
        std::uint8_t fih[8];
        ALEMBIC_TRY(file.read_at(p.fih_offset, MutableBytes(fih, 8)));
        if (std::memcmp(fih, "\x7F" "FIH", 4) != 0) return invalid("the LIH does not wrap an FIH");
        p.fih_version = get_u16(fih + 6);
        std::uint64_t logical = 0;
        ALEMBIC_TRY(read_cnt(file, p, cnt_offset, logical));
        p.segments = {{"LIH", 0, kFihRegion}, {"FIH", p.fih_offset, fih_size}, {"PFS", p.pfs_offset, p.pfs_size},
                      {"SC", cnt_offset, cnt_size}, {"SI", si_offset, si_size}};
        return p;
    }

    if (std::memcmp(magic, "\x7F" "FIH", 4) != 0) return invalid("unknown package magic (expected FIH, LIH or CNT)");
    if (p.file_size < kFihRegion) return invalid("the finalized image is smaller than its FIH region");
    std::uint8_t h[0x100];
    ALEMBIC_TRY(file.read_at(0, MutableBytes(h, sizeof h)));
    if (h[5] == 0x00) p.kind = Kind::finalized_debug;
    else if (h[5] == 0x80) p.kind = Kind::finalized_retail;
    else return make_error(Errc::unsupported, "unsupported FIH signed byte " + std::to_string(h[5]));
    p.fih_version = get_u16(h + 0x06);
    p.pfs_offset = get_u64(h + 0x10);
    p.pfs_size = get_u64(h + 0x18);
    p.cnt_offset = get_u64(h + 0x58);
    p.inner_meta_base = get_u64(h + 0x50);
    std::uint64_t sb_field = get_u64(h + 0x20);
    if (sb_field >= kBlockSize && (sb_field - kBlockSize) % kBlockSize == 0)
        p.superblock_hint = static_cast<std::int64_t>((sb_field - kBlockSize) / kBlockSize);

    if (p.fih_version != 3) p.warnings.push_back("FIH format version is " + std::to_string(p.fih_version) + ", expected 3");
    if (p.pfs_offset != kFihRegion) p.warnings.push_back("the PFS does not start at 0x10000");
    if (!in_range(p.pfs_offset, p.pfs_size, p.file_size)) return invalid("the PFS segment lies outside the package");
    if (p.pfs_offset + p.pfs_size != p.cnt_offset) p.warnings.push_back("the PFS does not end at the CNT");
    std::uint64_t logical = 0;
    ALEMBIC_TRY(read_cnt(file, p, p.cnt_offset, logical));
    std::uint64_t cnt_size = align_up(logical, kBlockSize);
    if (!in_range(p.cnt_offset, cnt_size, p.file_size)) cnt_size = p.file_size - p.cnt_offset;
    std::uint64_t si_offset = p.cnt_offset + cnt_size;
    p.segments = {{"FIH", 0, kFihRegion}, {"PFS", p.pfs_offset, p.pfs_size}, {"SC", p.cnt_offset, cnt_size},
                  {"SI", si_offset, p.file_size - si_offset}};
    return p;
}

Result<std::vector<std::uint8_t>> read_entry_raw(const RandomAccess& file, const Package& package, const CntEntry& entry) {
    std::uint64_t start = package.cnt_offset + entry.data_offset;
    std::uint64_t size = entry.encrypted() ? align_up(entry.data_size, 16) : entry.data_size;
    if (!in_range(start, size, package.file_size)) return invalid("CNT entry " + entry.name + " lies outside the package");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    ALEMBIC_TRY(file.read_at(start, data));
    return data;
}

namespace {

crypto::Digest compute_keys_with(crypto::Digest (*hash)(Bytes), const std::string& content_id,
                                 const std::string& passcode, std::uint32_t index) {
    std::uint8_t index_be[4] = {static_cast<std::uint8_t>(index >> 24), static_cast<std::uint8_t>(index >> 16),
                                static_cast<std::uint8_t>(index >> 8), static_cast<std::uint8_t>(index)};
    std::uint8_t cid[48] = {};
    std::memcpy(cid, content_id.data(), std::min<std::size_t>(content_id.size(), sizeof cid));
    crypto::Digest a = hash(Bytes(index_be, 4));
    crypto::Digest b = hash(Bytes(cid, sizeof cid));
    std::vector<std::uint8_t> data(a.begin(), a.end());
    data.insert(data.end(), b.begin(), b.end());
    data.insert(data.end(), passcode.begin(), passcode.end());
    return hash(data);
}

// The package's hash: SHA3-256 on PS5, SHA-256 on PS4.
crypto::Digest package_hash(const Package& package, Bytes data) {
    return package.ps4() ? crypto::sha256(data) : crypto::sha3_256(data);
}

} // namespace

crypto::Digest compute_keys(const std::string& content_id, const std::string& passcode, std::uint32_t index) {
    return compute_keys_with(crypto::sha3_256, content_id, passcode, index);
}

crypto::Digest compute_keys_ps4(const std::string& content_id, const std::string& passcode, std::uint32_t index) {
    return compute_keys_with(crypto::sha256, content_id, passcode, index);
}

Result<std::vector<std::uint8_t>> read_entry(const RandomAccess& file, const Package& package, const CntEntry& entry,
                                             const std::string& passcode) {
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, read_entry_raw(file, package, entry));
    if (!entry.encrypted()) return data;
    // key/iv seed = H(table row || package key[key index]); key = seed[16:32], iv = seed[0:16].
    crypto::Digest package_key = package.ps4() ? compute_keys_ps4(package.content_id, passcode, entry.key_index())
                                               : compute_keys(package.content_id, passcode, entry.key_index());
    std::uint8_t seed_input[64];
    std::memcpy(seed_input, entry.row, 32);
    std::memcpy(seed_input + 32, package_key.data(), 32);
    crypto::Digest seed = package_hash(package, Bytes(seed_input, sizeof seed_input));
    crypto::Aes128 key(seed.data() + 16);
    crypto::cbc_decrypt(key, seed.data(), data);
    data.resize(entry.data_size);
    return data;
}

Result<DigestCheck> check_digests(const RandomAccess& file, const Package& package) {
    DigestCheck check;
    if (package.kind == Kind::finalized_debug || package.kind == Kind::finalized_retail) {
        std::uint8_t h[0x100];
        ALEMBIC_TRY(file.read_at(package.fih_offset, MutableBytes(h, sizeof h)));
        bool mirrored = std::memcmp(h + 0x30, h + 0x70, 32) == 0 && std::memcmp(h + 0x30, h + 0xD0, 32) == 0;
        if (package.superblock_hint >= 0) {
            std::vector<std::uint8_t> sb(kBlockSize);
            ALEMBIC_TRY(file.read_at(package.pfs_offset + std::uint64_t(package.superblock_hint) * kBlockSize, sb));
            crypto::Digest d = crypto::sha3_256(sb);
            check.fih_digest_ok = mirrored && std::memcmp(d.data(), h + 0x30, 32) == 0;
        }
    }
    if (package.ps4()) {
        std::uint8_t h[0x50];
        ALEMBIC_TRY(file.read_at(0x430, MutableBytes(h, sizeof h)));
        std::uint32_t signed_size = be32(h + 0x08);
        if (signed_size > 0 && signed_size <= 16u * 1024 * 1024 && signed_size <= package.pfs_size) {
            std::vector<std::uint8_t> head(signed_size);
            ALEMBIC_TRY(file.read_at(package.pfs_offset, head));
            check.fih_digest_ok = std::memcmp(crypto::sha256(head).data(), h + 0x30, 32) == 0;
        }
    }
    auto digests = std::find_if(package.entries.begin(), package.entries.end(), [](const CntEntry& e) { return e.id == 1; });
    if (digests == package.entries.end()) {
        check.detail = "no digests entry";
        return check;
    }
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> table, read_entry_raw(file, package, *digests));
    std::vector<const CntEntry*> sorted;
    for (const CntEntry& e : package.entries) sorted.push_back(&e);
    std::stable_sort(sorted.begin(), sorted.end(), [](const CntEntry* a, const CntEntry* b) { return a->id < b->id; });
    if (table.size() != sorted.size() * 32) {
        check.detail = "the digests entry has the wrong length";
        return check;
    }
    check.entry_digests_ok = true;
    for (std::size_t i = 0; i < sorted.size(); ++i) {
        const CntEntry& e = *sorted[i];
        if (e.id == 1) continue;
        // Protected entries may be hashed over the padded or the exact length.
        bool matched = false;
        for (std::uint64_t length : {std::uint64_t(e.data_size), align_up(e.data_size, 16)}) {
            if (!in_range(package.cnt_offset + e.data_offset, length, package.file_size)) continue;
            std::vector<std::uint8_t> body(static_cast<std::size_t>(length));
            ALEMBIC_TRY(file.read_at(package.cnt_offset + e.data_offset, body));
            if (std::memcmp(package_hash(package, body).data(), table.data() + i * 32, 32) == 0) matched = true;
            if (matched || !e.encrypted()) break;
        }
        if (!matched) {
            check.entry_digests_ok = false;
            check.detail = "digest mismatch for CNT entry " + (e.name.empty() ? std::to_string(e.id) : e.name);
            break;
        }
    }
    return check;
}

// ---------------------------------------------------------------- outer PFS

Result<std::shared_ptr<OuterPfs>> OuterPfs::open(std::shared_ptr<const RandomAccess> file, const Package& package,
                                                 const std::string& passcode) {
    if (package.kind == Kind::metadata_container) return make_error(Errc::unsupported, "a CNT has no file system");
    if (package.kind == Kind::finalized_retail)
        return make_error(Errc::unsupported, "retail packages need keys this tool does not have");
    if (package.kind == Kind::patch_debug)
        return make_error(Errc::unsupported, "patch packages are not supported yet");
    if (package.content_id.size() != 36) return invalid("the package has no usable content ID");
    if (package.ps4() && package.superblock_hint != 0) return invalid("the PS4 package's PFS has no superblock");
    if (passcode.size() != 32) return make_error(Errc::invalid_argument, "the passcode must be 32 characters");
    if (package.pfs_size < kBlockSize) return invalid("the outer PFS is too small");

    std::shared_ptr<OuterPfs> pfs(new OuterPfs());
    pfs->file_ = std::move(file);
    pfs->pfs_offset_ = package.pfs_offset;
    pfs->ps4_ = package.ps4();
    if (package.pfs_size / kBlockSize > UINT32_MAX) return invalid("the outer PFS is too large");
    pfs->block_count_ = static_cast<std::uint32_t>(package.pfs_size / kBlockSize);
    pfs->superblock_.resize(kBlockSize);

    // Find the plaintext superblock: try the FIH hint, then scan.
    auto is_superblock = [&](std::uint32_t index) -> Result<bool> {
        ALEMBIC_TRY(pfs->file_->read_at(pfs->pfs_offset_ + std::uint64_t(index) * kBlockSize, pfs->superblock_));
        return get_u64(pfs->superblock_.data() + 8) == kPfsMagic;
    };
    bool found = false;
    if (package.superblock_hint >= 0 && package.superblock_hint < pfs->block_count_) {
        ALEMBIC_ASSIGN(found, is_superblock(static_cast<std::uint32_t>(package.superblock_hint)));
        if (found) pfs->superblock_index_ = static_cast<std::uint32_t>(package.superblock_hint);
    }
    for (std::uint32_t i = 0; !found && i < pfs->block_count_; ++i) {
        ALEMBIC_ASSIGN(found, is_superblock(i));
        if (found) pfs->superblock_index_ = i;
    }
    if (!found) return invalid("no plaintext PFS superblock in the outer image");
    const std::uint8_t* sb = pfs->superblock_.data();

    pfs->plain_ = pfs->ps4_ ? (get_u16(sb + 0x1C) & kPfsModeEncrypted) == 0
                            : std::memcmp(sb + kSeedOffset, kPlainSeed, 16) == 0;
    if (pfs->ps4_) {
        if (get_u16(sb + 0x1C) & kPfsMode64BitInodes)
            return make_error(Errc::unsupported, "PS4 packages with 64-bit PFS inodes are not supported yet");
        // EKPFS from the passcode; tweak/data keys = HMAC-SHA256(EKPFS, LE32(1) || seed).
        crypto::Digest ekpfs = compute_keys_ps4(package.content_id, passcode, 1);
        std::uint8_t message[20] = {1, 0, 0, 0};
        std::memcpy(message + 4, sb + kSeedOffset, 16);
        crypto::Digest keys = crypto::hmac_sha256(Bytes(ekpfs.data(), 32), Bytes(message, sizeof message));
        if (!pfs->plain_) {
            pfs->tweak_key_ = std::make_unique<crypto::Aes128>(keys.data());
            pfs->data_key_ = std::make_unique<crypto::Aes128>(keys.data() + 16);
        }
        // The superblock is signed with HMAC-SHA256(EKPFS, LE32(2) || seed); a match proves the passcode.
        message[0] = 2;
        crypto::Digest sign_key = crypto::hmac_sha256(Bytes(ekpfs.data(), 32), Bytes(message, sizeof message));
        std::vector<std::uint8_t> region(sb, sb + kIcvRegion);
        std::memset(region.data() + kIcvOffset, 0, 32);
        pfs->icv_ok_ = std::memcmp(crypto::hmac_sha256(Bytes(sign_key.data(), 32), region).data(), sb + kIcvOffset, 32) == 0;
    } else if (pfs->plain_) {
        pfs->icv_ok_ = true;
    } else {
        std::vector<std::uint8_t> region(sb, sb + kIcvRegion);
        std::memset(region.data() + kIcvOffset, 0, 32);
        pfs->icv_ok_ = std::memcmp(crypto::sha3_256(region).data(), sb + kIcvOffset, 32) == 0;

        crypto::Digest ekpfs = compute_keys(package.content_id, passcode, 1);
        crypto::Digest inner = crypto::hmac_sha256(Bytes(ekpfs.data(), 32), Bytes(sb + kSeedOffset, 16));
        std::uint8_t message[20] = {1, 0, 0, 0};
        std::memcpy(message + 4, sb + kSeedOffset, 16);
        crypto::Digest keys = crypto::hmac_sha256(Bytes(inner.data(), 32), Bytes(message, sizeof message));
        pfs->tweak_key_ = std::make_unique<crypto::Aes128>(keys.data());
        pfs->data_key_ = std::make_unique<crypto::Aes128>(keys.data() + 16);
    }

    // Inode table: after the superblock and its (non-zero) indirect signature blocks.
    std::uint32_t indirect = 0;
    for (int i = 0; i < 5; ++i)
        if (static_cast<std::int64_t>(get_u64(sb + 0x50 + 0x244 + i * 40 + 8)) > 0) ++indirect;
    std::uint64_t inode_count = get_u64(sb + 0x30);
    std::uint64_t inode_blocks = get_u64(sb + 0x40);
    std::uint64_t first = std::uint64_t(pfs->superblock_index_) + 1 + indirect;
    if (inode_count == 0 || inode_blocks == 0 || first + inode_blocks > pfs->block_count_ ||
        inode_count > inode_blocks * (kBlockSize / kInodeSize))
        return invalid("the outer PFS inode table is out of range");
    std::vector<std::uint8_t> table(static_cast<std::size_t>(inode_blocks * kBlockSize));
    for (std::uint64_t b = 0; b < inode_blocks; ++b)
        ALEMBIC_TRY(pfs->read_block(static_cast<std::uint32_t>(first + b), true,
                                    MutableBytes(table.data() + b * kBlockSize, kBlockSize)));
    for (std::uint64_t i = 0; i < inode_count; ++i) {
        const std::uint8_t* e = table.data() + i * kInodeSize;
        Inode inode;
        inode.mode = get_u16(e);
        inode.size = get_u64(e + 8);
        inode.size_compressed = get_u64(e + 16);
        inode.blocks = get_u32(e + 0x60);
        for (int k = 0; k < 12; ++k) inode.direct[k] = static_cast<std::int32_t>(get_u32(e + kDirectOffset + k * kSigStride + 32));
        for (int k = 0; k < 5; ++k) inode.indirect[k] = static_cast<std::int32_t>(get_u32(e + kIndirectOffset + k * kSigStride + 32));
        pfs->inodes_.push_back(inode);
    }
    if (pfs->inodes_.size() <= 2) return invalid("the outer PFS has no uroot inode");
    const Inode& uroot = pfs->inodes_[2];
    if ((uroot.mode & 0xF000) != 0x4000 || uroot.size == 0 || uroot.size > std::uint64_t(pfs->block_count_) * kBlockSize ||
        uroot.blocks == 0 || uroot.blocks > pfs->block_count_)
        return make_error(Errc::invalid_data,
                          "the outer PFS did not decrypt to a valid root folder: wrong passcode, or a corrupt package");

    // The uroot directory.
    ALEMBIC_ASSIGN(std::vector<std::uint32_t> blocks, pfs->resolve_blocks(uroot));
    std::vector<std::uint8_t> dir(blocks.size() * kBlockSize);
    for (std::size_t i = 0; i < blocks.size(); ++i)
        ALEMBIC_TRY(pfs->read_block(blocks[i], true, MutableBytes(dir.data() + i * kBlockSize, kBlockSize)));
    std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(uroot.size, dir.size()));
    for (std::size_t pos = 0; pos + 16 <= length;) {
        std::uint32_t ino = get_u32(&dir[pos]);
        std::uint32_t name_length = get_u32(&dir[pos + 8]);
        std::uint32_t entry_size = get_u32(&dir[pos + 12]);
        if (entry_size < 16 || entry_size > length - pos || name_length > entry_size - 16) break;
        std::string name(reinterpret_cast<const char*>(&dir[pos + 16]), name_length);
        if (name != "." && name != ".." && ino < pfs->inodes_.size()) {
            const Inode& inode = pfs->inodes_[ino];
            pfs->files_.push_back({name, inode.size, inode.size_compressed, ino, (inode.mode & 0xF000) == 0x4000});
        }
        pos += entry_size;
    }
    return pfs;
}

Status OuterPfs::read_block(std::uint32_t index, bool is_signed, MutableBytes out) const {
    if (index >= block_count_ || out.size() < kBlockSize) return invalid("outer PFS block out of range");
    MutableBytes block = out.first(kBlockSize);
    ALEMBIC_TRY(file_->read_at(pfs_offset_ + std::uint64_t(index) * kBlockSize, block));
    if (index == superblock_index_ || plain_) return {};
    if (ps4_) {
        // One XTS unit per 4 KiB sector, numbered by its offset in the image.
        constexpr std::uint64_t kSector = 0x1000;
        for (std::uint64_t s = 0; s < kBlockSize / kSector; ++s)
            crypto::xts_decrypt(*data_key_, *tweak_key_, std::uint64_t(index) * (kBlockSize / kSector) + s,
                                block.subspan(s * kSector, kSector));
        return {};
    }
    std::uint64_t sector = is_signed ? (kSignedSector | index) : index;
    crypto::xts_decrypt(*data_key_, *tweak_key_, sector, block);
    return {};
}

Result<std::vector<std::uint32_t>> OuterPfs::resolve_blocks(const Inode& inode) const {
    if (inode.blocks > block_count_)
        return invalid("an outer inode has more blocks than the image: wrong passcode or corrupt package");
    std::vector<std::uint32_t> result;
    for (int i = 0; i < 12 && result.size() < inode.blocks; ++i) result.push_back(static_cast<std::uint32_t>(inode.direct[i]));
    // Each indirect root ib[level] is a tree of depth level + 1; leaves hold data block indices.
    struct Walker {
        const OuterPfs& pfs;
        std::vector<std::uint32_t>& out;
        std::uint32_t wanted;
        Status walk(std::int64_t root, int depth) {
            if (out.size() >= wanted) return {};
            if (root <= 0 || root >= pfs.block_count_) return invalid("an outer inode points outside the image");
            std::vector<std::uint8_t> node(kBlockSize);
            ALEMBIC_TRY(pfs.read_block(static_cast<std::uint32_t>(root), true, node));
            for (std::size_t i = 0; i < kTreeEntries && out.size() < wanted; ++i) {
                std::int32_t child = static_cast<std::int32_t>(get_u32(&node[i * kSigStride + 32]));
                if (depth == 1) out.push_back(static_cast<std::uint32_t>(child));
                else if (child > 0) ALEMBIC_TRY(walk(child, depth - 1));
                else return invalid("an outer indirect block has a missing child");
            }
            return {};
        }
    } walker{*this, result, inode.blocks};
    for (int level = 0; level < 5 && result.size() < inode.blocks; ++level)
        if (inode.indirect[level] > 0) ALEMBIC_TRY(walker.walk(inode.indirect[level], level + 1));
    if (result.size() < inode.blocks) return invalid("an outer inode's block tree is incomplete");
    for (std::uint32_t b : result)
        if (b >= block_count_) return invalid("an outer inode points outside the image");
    return result;
}

namespace {

class OuterFile final : public RandomAccess {
public:
    OuterFile(std::shared_ptr<const OuterPfs> pfs, std::vector<std::uint32_t> blocks, bool is_signed, std::uint64_t size)
        : pfs_(std::move(pfs)), blocks_(std::move(blocks)), signed_(is_signed), size_(size), cache_(kBlockSize) {}

    Result<std::uint64_t> size() const override { return size_; }

    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > size_ || out.size() > size_ - offset) return invalid("read past the end of an outer PFS file");
        std::size_t done = 0;
        while (done < out.size()) {
            std::uint64_t at = offset + done;
            std::size_t index = static_cast<std::size_t>(at / kBlockSize);
            std::size_t within = static_cast<std::size_t>(at % kBlockSize);
            if (index != cached_) {
                ALEMBIC_TRY(pfs_->read_block(blocks_[index], signed_, cache_));
                cached_ = index;
            }
            std::size_t n = std::min<std::size_t>(out.size() - done, kBlockSize - within);
            std::memcpy(out.data() + done, cache_.data() + within, n);
            done += n;
        }
        return {};
    }

private:
    std::shared_ptr<const OuterPfs> pfs_;
    std::vector<std::uint32_t> blocks_;
    bool signed_;
    std::uint64_t size_;
    mutable std::vector<std::uint8_t> cache_;
    mutable std::size_t cached_ = SIZE_MAX;
};

} // namespace

Result<std::shared_ptr<const RandomAccess>> OuterPfs::open_file(const std::string& name) const {
    auto it = std::find_if(files_.begin(), files_.end(), [&](const OuterFileInfo& f) { return f.name == name; });
    if (it == files_.end()) return make_error(Errc::invalid_data, "the outer PFS has no '" + name + "'");
    const Inode& inode = inodes_[it->inode];
    ALEMBIC_ASSIGN(std::vector<std::uint32_t> blocks, resolve_blocks(inode));
    std::uint64_t size = std::min<std::uint64_t>(inode.size, blocks.size() * kBlockSize);
    // pfs_image.dat data blocks use the plain sector numbering; everything else is "signed".
    bool is_signed = name != "pfs_image.dat";
    return std::shared_ptr<const RandomAccess>(
        std::make_shared<OuterFile>(shared_from_this(), std::move(blocks), is_signed, size));
}

Result<std::shared_ptr<const RandomAccess>> OuterPfs::open_inner_image() const {
    // The data-first layout puts pfs_image.dat's data in blocks 0 .. superblock - 2; block
    // superblock - 1 holds the NAPS file. The inode size of pfs_image.dat is the inner mount's
    // logical size, not this on-disk extent.
    if (superblock_index_ < 2) return invalid("the outer PFS has no pfs_image.dat data region");
    std::vector<std::uint32_t> blocks(superblock_index_ - 1);
    for (std::uint32_t i = 0; i < blocks.size(); ++i) blocks[i] = i;
    std::uint64_t size = std::uint64_t(blocks.size()) * kBlockSize;
    return std::shared_ptr<const RandomAccess>(std::make_shared<OuterFile>(shared_from_this(), std::move(blocks), false, size));
}

} // namespace alembic::fpkg
