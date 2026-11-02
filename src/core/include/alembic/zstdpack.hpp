// SPDX-License-Identifier: GPL-3.0-or-later
// zstd asset packs (AMPRZPK1 index `ampr_zstd.index` + AMPRZDT1 volumes
// `ampr_zstd-NNN.pak`), read on the console by quer3q/ampr-emu-ng's
// libSceAmpr.sprx (which reads LZ4 packs too). The format follows
// ampr-emu-ng's docs/zstd-format.md; the writer is a port of
// quer3q/ps5-dump-forge's ps5-dump-forge-zstd crate (writer.rs, index.rs,
// volume.rs; GPL-3.0) and the choice of packed files of its
// ps5-dump-forge-core/src/zstd_pack.rs and ps5-dump-forge-lz4/src/rules.rs.
//
// Every packed file is cut into 256 KiB chunks; each chunk is one zstd frame
// (libzstd 1.5.7, content size and checksum on, window 2^18) or stored raw
// when the frame is not smaller. A file whose chunks save less than 1% is
// stored raw entirely ("store-only"). Files are written in (path hash, path)
// order, chunks back to back from offset 64 of each volume; a volume holds
// at most 4 GiB less 64 KiB.
//
// Like the LZ4 packer this is a tree -> tree transformation with two passes
// and no temporary copy: pass 1 compresses every chunk to fix the layout;
// the volumes are made while the output writer reads them (pass 2
// compresses again on all cores and checks each chunk against pass 1).
// Given the same archive ID and level the index and volumes are meant to be
// byte-identical to ps5-dump-forge's (tools/zstd-oracle/compare.sh).
#pragma once

#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::zstdpack {

inline constexpr const char* kIndexName = "ampr_zstd.index";
inline constexpr const char* kVolumePrefix = "ampr_zstd-";
inline constexpr const char* kVolumeSuffix = ".pak";
inline constexpr const char* kRuntimeRelativePath = "fakelib/libSceAmpr.sprx";

inline constexpr std::uint32_t kFormatVersion = 1;
inline constexpr std::uint32_t kChunkShift = 18;
inline constexpr std::uint64_t kChunkSize = 1ull << kChunkShift; // 256 KiB
inline constexpr std::uint32_t kIndexHeaderSize = 128;
inline constexpr std::uint32_t kVolumeHeaderSize = 64;
inline constexpr std::uint64_t kVolumeLimit = (1ull << 32) - (1ull << 16); // header included
inline constexpr std::uint32_t kMaxVolumes = 1u << 12;
inline constexpr int kDefaultLevel = 5; // ps5-dump-forge's DEFAULT_ZSTD_LEVEL
inline constexpr int kMinLevel = 1;
inline constexpr int kMaxLevel = 22;

using ArchiveId = std::array<std::uint8_t, 16>;

// ---------------------------------------------------------------- format

enum class Codec : std::uint8_t { raw = 0, zstd = 1 };

// One 256 KiB chunk: an absolute place in a volume. Stored in the index as
// a u64: bits 0..31 offset, 32..43 volume, 44..61 stored_size - 1, 62..63 codec.
struct Chunk {
    std::uint32_t offset = 0;
    std::uint16_t volume = 0;
    std::uint32_t stored_size = 0;
    Codec codec = Codec::raw;
    bool operator==(const Chunk&) const = default;
};
Result<std::uint64_t> pack_chunk(const Chunk& chunk);
Result<Chunk> unpack_chunk(std::uint64_t word);

struct FileRecord {
    std::string path; // relative, '/', case kept
    std::uint64_t size = 0;
    std::uint32_t first_chunk = 0;
    bool store_only = false;
    bool operator==(const FileRecord&) const = default;
};

struct Index {
    ArchiveId archive_id{};
    std::uint8_t level = kDefaultLevel;
    std::vector<FileRecord> files; // sorted by (path_hash, path bytes)
    std::vector<Chunk> chunks;
    std::vector<std::uint64_t> volume_sizes; // header included
    bool operator==(const Index&) const = default;
};

// FNV-1a 64 over the path's bytes (basis 14695981039346656037).
std::uint64_t path_hash(std::string_view path);
// Relative, no empty, "." or ".." parts, no '\\' or NUL.
bool valid_path(std::string_view path);
// ampr_zstd-000.pak, ... (three digits at least).
std::string volume_name(std::uint32_t id);
// Chunk i of a file of `size` bytes decodes to this many bytes.
inline std::uint64_t chunk_raw_size(std::uint64_t size, std::uint64_t i) {
    std::uint64_t left = size - i * kChunkSize;
    return left < kChunkSize ? left : kChunkSize;
}
inline std::uint64_t chunk_count(std::uint64_t size) { return (size + kChunkSize - 1) >> kChunkShift; }

// The checks ps5-dump-forge's Index::validate makes (order, ranges, sizes,
// volume continuity, no file that is also a folder). `volume_limit` is
// kVolumeLimit except in tests.
Status validate(const Index& index, std::uint64_t volume_limit = kVolumeLimit);
Result<std::vector<std::uint8_t>> encode_index(const Index& index, std::uint64_t volume_limit = kVolumeLimit);
// Parses and fully validates, with both CRCs (what the runtime accepts).
Result<Index> parse_index(Bytes data, std::uint64_t volume_limit = kVolumeLimit);

struct VolumeHeader {
    std::uint32_t id = 0;
    std::uint32_t count = 0;
    ArchiveId archive_id{};
    std::uint64_t payload_bytes = 0;
    bool operator==(const VolumeHeader&) const = default;
};
Result<std::array<std::uint8_t, kVolumeHeaderSize>> encode_volume_header(const VolumeHeader& header);
Result<VolumeHeader> parse_volume_header(Bytes data);

// One chunk as one zstd frame, exactly as ps5-dump-forge makes it: a context
// with the level, window log 18, checksum and content size flags; the chunk
// pledged, fed with ZSTD_e_continue and closed with ZSTD_e_end, into an
// output buffer of the chunk's own size. Returns false (store raw) when the
// frame would not be smaller than the chunk.
class Encoder {
public:
    Encoder() = default;
    ~Encoder();
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;
    Result<bool> compress(Bytes raw, int level, std::vector<std::uint8_t>& frame);

private:
    void* context_ = nullptr; // ZSTD_CCtx
    int level_ = 0;
};

// Decodes one chunk, with the checks ampr-emu-ng makes on a frame
// (checksum and content size present, no dictionary, one frame filling
// `stored`, exactly `raw.size()` bytes, window at most 2^18; the checksum
// is verified here, the runtime skips it).
class Decoder {
public:
    Decoder() = default;
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Status decode(Codec codec, Bytes stored, MutableBytes raw);

private:
    void* context_ = nullptr; // ZSTD_DCtx
};

// ---------------------------------------------------------------- which files

// ps5-dump-forge's rules: never packed (eboot.bin, *.elf, *.self, *.prx,
// *.sprx, files under sce_sys/, sce_module/, fakelib*/, the ampr_emu and LZ4
// pack artifacts, the zstd pack files, ampr_emu.index) ...
bool always_loose(std::string_view path);
// ... its keep-loose list (configs, media, containers; protected folders) ...
bool keep_loose(std::string_view path);
// ... and its default choice: not empty, not always_loose, not at the root,
// not keep_loose. (Its LZ4-packed sources keep their LZ4 pack set instead;
// see PackOptions::packed_set.)
bool default_selected(std::string_view path, std::uint64_t size);

// ---------------------------------------------------------------- packing

struct PackOptions {
    int level = kDefaultLevel; // 1..22
    // The archive ID in the index and every volume header. Random when unset
    // (ps5-dump-forge's is always random); fix it for reproducible output.
    std::optional<ArchiveId> archive_id;
    // The runtime to place at fakelib/libSceAmpr.sprx (replacing any existing
    // one): ampr-emu-ng's libSceAmpr.sprx, the one that reads zstd packs.
    // Without it the game's own must be there (the packer refuses otherwise).
    std::shared_ptr<const RandomAccess> runtime_module;
    // Modification time of the files the packer adds. Default: now.
    std::optional<std::int64_t> timestamp;
    unsigned workers = 0; // compression threads, 0 = every core; never changes the output
    // Which files to pack instead of default_selected (paths relative to the
    // root). Paths always_loose would keep loose stay loose anyway.
    std::optional<std::vector<std::string>> packed_set;
    // Tests only: a smaller volume limit, to see volumes roll over.
    std::uint64_t volume_limit = kVolumeLimit;
};

struct PackStats {
    std::uint64_t files_total = 0; // in the output tree, before packing
    std::uint64_t files_packed = 0;
    std::uint64_t files_store_only = 0;
    std::uint64_t chunks = 0;
    std::uint64_t chunks_zstd = 0;
    std::uint64_t chunks_raw = 0;
    std::uint64_t logical_bytes = 0; // packed files, before packing
    std::uint64_t stored_bytes = 0;  // chunk bytes in the volumes
    std::uint64_t volume_bytes = 0;  // all volume files, headers included
    std::uint64_t loose_bytes = 0;
    std::uint32_t volumes = 0;
    std::vector<std::string> warnings;
};

// A packed tree: the input without the packed files, plus the runtime, a new
// ampr_emu.index (every file, loose and packed), ampr_zstd.index and the
// volumes, which are produced while they are read.
class PackedTree : public tree::Source {
public:
    virtual const PackStats& stats() const = 0;
    virtual const Index& index() const = 0;
};

// Pass 1. `source` must outlive the result. Progress stage: "zstd pass 1"
// (bytes of packed files); returning false cancels.
Result<std::unique_ptr<PackedTree>> pack(const tree::Source& source, const PackOptions& options,
                                         const ProgressFn& progress = {});

// ---------------------------------------------------------------- reading

// True when the root has an ampr_zstd.index.
bool is_packed(const tree::Node& root);

struct VerifyResult {
    std::uint64_t files = 0;
    std::uint64_t chunks = 0;
    std::uint64_t chunks_zstd = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t raw_bytes = 0;
    std::uint32_t volumes = 0;
};
// Checks the index and every volume header and decodes every chunk (frame
// checksums verified), as the runtime would read them.
Result<VerifyResult> verify(const tree::Source& packed, const ProgressFn& progress = {});

// The original tree back: packed files served from the volumes; the index
// and the volumes are left out.
Result<std::unique_ptr<tree::Source>> unpack(const tree::Source& packed);

} // namespace alembic::zstdpack
