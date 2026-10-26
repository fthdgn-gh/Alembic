// SPDX-License-Identifier: GPL-3.0-or-later
// PS5 debug packages (FPKG), read side, first layer: the package envelope
// (FIH / LIH / CNT), CNT entries, key derivation and the encrypted outer PFS.
// PS4 fake packages are a bare CNT whose header (0x400..0x420) points at the
// outer PFS; they use SHA-256 where PS5 uses SHA3, one HMAC step for the PFS
// keys, and XTS over 4 KiB sectors.
// The inner image (pfs_image.dat + naps_pkg_layout.dat) is decoded elsewhere.
// Formats follow pearlxcore/ProsperoPkgTool (MIT); see THIRD_PARTY_NOTICES.md.
//
// Finalized package:  FIH header (64 KiB) | outer PFS | CNT | SI (ZIP)
//   FIH 0x05 signed byte (0x00 debug, 0x80 retail), 0x06 u16 format version (3),
//   0x10 PFS offset, 0x18 PFS size, 0x20 0x10000 + superblock offset,
//   0x30/0x70/0xD0 SHA3-256 of the plaintext outer superblock block,
//   0x50 inner metadata base (64 KiB blocks), 0x58 CNT offset.
// CNT (big-endian!): header 0x5A0 bytes, entry table of 0x20-byte rows
//   {id, name offset, flags1, flags2, data offset, data size, 8 reserved}.
#pragma once

#include "alembic/crypto.hpp"
#include "alembic/io.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alembic::fpkg {

inline constexpr std::uint64_t kBlockSize = 0x10000;
inline const std::string kDefaultPasscode(32, '0');

enum class Kind { metadata_container, finalized_debug, finalized_retail, patch_debug, ps4_package };
const char* name(Kind kind);

struct CntEntry {
    std::uint32_t id = 0;
    std::uint32_t name_offset = 0;
    std::uint32_t flags1 = 0;
    std::uint32_t flags2 = 0;
    std::uint32_t data_offset = 0; // relative to the CNT start
    std::uint32_t data_size = 0;
    std::string name;              // from the entry-names table, else a known name, else ""
    std::uint8_t row[32] = {};     // the raw table row (key derivation binds to it)
    bool encrypted() const { return (flags1 & 0x80000000u) != 0; }
    std::uint32_t key_index() const { return (flags2 >> 12) & 0xF; }
};

struct Segment {
    std::string name;
    std::uint64_t offset;
    std::uint64_t size;
};

struct Package {
    Kind kind = Kind::metadata_container;
    std::uint64_t file_size = 0;
    // FIH (finalized packages)
    std::uint16_t fih_version = 0;
    std::uint64_t fih_offset = 0; // 0, or after the LIH for patches
    std::uint64_t pfs_offset = 0;
    std::uint64_t pfs_size = 0;
    std::uint64_t cnt_offset = 0;
    std::int64_t superblock_hint = -1;  // outer PFS block index from FIH 0x20
    std::uint64_t inner_meta_base = 0;  // FIH 0x50, 64 KiB blocks
    // CNT header
    std::uint16_t cnt_version = 0;
    std::uint16_t cnt_flags = 0;
    std::uint32_t entry_table_offset = 0;
    std::uint64_t body_offset = 0;
    std::uint64_t body_size = 0;
    std::string content_id; // 36 characters
    std::uint32_t drm_type = 0;
    std::uint32_t content_type = 0;
    std::uint32_t content_flags = 0;
    std::vector<CntEntry> entries;
    std::vector<Segment> segments;
    std::vector<std::string> warnings; // structural issues that do not stop reading
    bool ps4() const { return kind == Kind::ps4_package; }
};

Result<Package> read_package(const RandomAccess& file);

// Raw stored bytes of an entry (still encrypted if the entry is protected).
Result<std::vector<std::uint8_t>> read_entry_raw(const RandomAccess& file, const Package& package, const CntEntry& entry);
// Plaintext of an entry; protected entries are decrypted with AES-128-CBC.
Result<std::vector<std::uint8_t>> read_entry(const RandomAccess& file, const Package& package, const CntEntry& entry,
                                             const std::string& passcode = kDefaultPasscode);

struct DigestCheck {
    bool fih_digest_ok = false;   // FIH 0x30/0x70/0xD0 == SHA3(superblock block);
                                  // PS4: CNT 0x460 == SHA-256(first signed bytes of the PFS)
    bool entry_digests_ok = false;
    std::string detail;
};
// Checks the digests that need no key.
Result<DigestCheck> check_digests(const RandomAccess& file, const Package& package);

// SHA3( SHA3(BE32(index)) || SHA3(content id padded to 48) || passcode ).
crypto::Digest compute_keys(const std::string& content_id, const std::string& passcode, std::uint32_t index);
// The same with SHA-256 (PS4 packages).
crypto::Digest compute_keys_ps4(const std::string& content_id, const std::string& passcode, std::uint32_t index);

struct OuterFileInfo {
    std::string name;
    std::uint64_t size = 0;
    std::uint64_t size_compressed = 0;
    std::uint32_t inode = 0;
    bool is_directory = false;
};

// The decrypted outer PFS. Holds a reference to the package file.
class OuterPfs : public std::enable_shared_from_this<OuterPfs> {
public:
    static Result<std::shared_ptr<OuterPfs>> open(std::shared_ptr<const RandomAccess> file, const Package& package,
                                                  const std::string& passcode = kDefaultPasscode);

    std::uint32_t superblock_index() const { return superblock_index_; }
    std::uint32_t block_count() const { return block_count_; }
    bool plain_no_auth() const { return plain_; }
    bool superblock_icv_ok() const { return icv_ok_; }
    const std::vector<OuterFileInfo>& files() const { return files_; } // the uroot directory

    // One decrypted 64 KiB block. `is_signed` selects the metadata sector numbering.
    Status read_block(std::uint32_t index, bool is_signed, MutableBytes out) const;
    // A file of the uroot directory by name (blocks resolved through the inode).
    Result<std::shared_ptr<const RandomAccess>> open_file(const std::string& name) const;
    // pfs_image.dat's on-disk extent: the contiguous data blocks before the NAPS block.
    Result<std::shared_ptr<const RandomAccess>> open_inner_image() const;

private:
    struct Inode {
        std::uint16_t mode = 0;
        std::uint64_t size = 0;
        std::uint64_t size_compressed = 0;
        std::uint32_t blocks = 0;
        std::int32_t direct[12] = {};
        std::int32_t indirect[5] = {};
    };
    OuterPfs() = default;
    Result<std::vector<std::uint32_t>> resolve_blocks(const Inode& inode) const;

    std::shared_ptr<const RandomAccess> file_;
    std::uint64_t pfs_offset_ = 0;
    std::uint32_t block_count_ = 0;
    std::uint32_t superblock_index_ = 0;
    std::vector<std::uint8_t> superblock_;
    bool plain_ = false;
    bool icv_ok_ = false;
    bool ps4_ = false;
    std::unique_ptr<crypto::Aes128> data_key_;
    std::unique_ptr<crypto::Aes128> tweak_key_;
    std::vector<Inode> inodes_;
    std::vector<OuterFileInfo> files_;
};

} // namespace alembic::fpkg
