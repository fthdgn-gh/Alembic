// SPDX-License-Identifier: GPL-3.0-or-later
// PFSC: the block container inside FFPFSC images. The payload is split into
// 64 KiB blocks, each stored either zlib-compressed or raw, with an offset
// table in front. Ported from PS5PKGTool's PfscCodec.cs.
//
// Layout (all little-endian):
//   0x00 u32 magic "PFSC"     0x04 u32 0         0x08 u32 version (6)
//   0x0C u32 block size       0x10 u64 block size again
//   0x18 u64 offset table position (0x400)
//   0x20 u64 data start (64 KiB aligned)   0x28 u64 padded logical length
//   0x400: (block count + 1) u64 offsets, relative to the PFSC start.
// A block whose stored size equals the block size is raw; anything smaller is zlib.
#pragma once

#include "alembic/io.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace alembic::pfsc {

inline constexpr std::uint32_t kMagic = 0x43534650; // "PFSC"
inline constexpr std::uint32_t kVersionField = 6;
inline constexpr std::uint32_t kBlockSize = 0x10000;
inline constexpr std::uint32_t kHeaderRecordSize = 0x30;
inline constexpr std::uint32_t kOffsetTableOffset = 0x400;
inline constexpr std::uint32_t kInitialDataOffset = 0x10000;
inline constexpr std::uint64_t kMaxBlockCount = 16'777'216;

struct CompressionOptions {
    int level = 9;                // zlib level, 0-9
    int minimum_gain_percent = 5; // keep a compressed block only if it saves this much
};

struct WriteResult {
    std::uint64_t source_length = 0;
    std::uint64_t padded_logical_length = 0;
    std::uint64_t stored_length = 0;
    std::uint64_t header_size = 0;
    std::uint32_t block_count = 0;
    std::uint32_t compressed_block_count = 0;
};

struct Info {
    std::uint64_t offset_table_offset = 0;
    std::uint64_t data_offset = 0;
    std::uint64_t padded_logical_length = 0;
    std::uint64_t stored_length = 0;
    std::vector<std::uint64_t> block_offsets; // block count + 1 entries
    std::uint32_t block_count() const { return static_cast<std::uint32_t>(block_offsets.size() - 1); }
};

// Space reserved for the header and offset table of a payload this long.
std::uint64_t header_size_for(std::uint64_t source_length);

// Streaming encoder. Push exactly `source_length` bytes through write(), then
// call finish(). The output is written to `destination` starting at `start`.
class Encoder final : public ByteSink {
public:
    Encoder(File& destination, std::uint64_t start, std::uint64_t source_length,
            CompressionOptions options = {}, ProgressFn progress = {});
    ~Encoder() override;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    Status begin();
    Status write(Bytes data) override;
    Result<WriteResult> finish();

private:
    Status flush_block(std::uint32_t valid_bytes);

    struct Zlib;
    File& destination_;
    std::uint64_t start_;
    std::uint64_t source_length_;
    CompressionOptions options_;
    ProgressFn progress_;
    std::unique_ptr<Zlib> zlib_;
    std::vector<std::uint8_t> block_;
    std::vector<std::uint8_t> compressed_;
    std::vector<std::uint64_t> offsets_;
    std::uint32_t block_fill_ = 0;
    std::uint32_t block_count_ = 0;
    std::uint32_t blocks_done_ = 0;
    std::uint32_t compressed_blocks_ = 0;
    std::uint64_t header_size_ = 0;
    std::uint64_t write_position_ = 0; // relative to start_
    std::uint64_t received_ = 0;
    std::uint64_t last_progress_ = 0;
    bool begun_ = false;
};

// Encodes the whole of `source` into `destination` at `start`.
Result<WriteResult> encode_file(const File& source, File& destination, std::uint64_t start,
                                CompressionOptions options = {}, const ProgressFn& progress = {});

// Reads and validates the header and offset table.
Result<Info> inspect(const File& source, std::uint64_t base_offset = 0,
                     std::optional<std::uint64_t> stored_length = std::nullopt);

// Decodes block `index` into `out`, which must hold kBlockSize bytes.
Status read_block(const File& source, const Info& info, std::uint32_t index, std::uint64_t base_offset,
                  MutableBytes out);

// Decodes the payload into `destination`. `content_length` trims the padded
// final block; by default the full padded length is written.
Status decode(const File& source, ByteSink& destination, std::optional<std::uint64_t> content_length = std::nullopt,
              std::uint64_t base_offset = 0, std::optional<std::uint64_t> stored_length = std::nullopt,
              const ProgressFn& progress = {});

} // namespace alembic::pfsc
