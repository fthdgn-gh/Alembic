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
#include <functional>
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
    // zlib level, 0-9. 6 is 2.4x faster than 9 for ~0.2% larger output;
    // PS5PKGTool uses 9 (pass 9 to match its bytes).
    int level = 6;
    int minimum_gain_percent = 5; // keep a compressed block only if it saves this much
    unsigned threads = 0;         // compression threads; 0 = one per CPU core
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
// Blocks are compressed on `options.threads` worker threads and written in
// order, so the output does not depend on the thread count.
class Encoder final : public ByteSink {
public:
    Encoder(File& destination, std::uint64_t start, std::uint64_t source_length,
            CompressionOptions options = {}, ProgressFn progress = {});
    ~Encoder() override;
    Encoder(const Encoder&) = delete;
    Encoder& operator=(const Encoder&) = delete;

    // Where an interrupted encode stopped: blocks fully written and the end
    // of the last one (relative to `start`).
    struct Checkpoint {
        std::uint32_t blocks = 0;
        std::uint64_t written = 0;
        std::uint32_t compressed = 0;
    };
    // Every `interval` input bytes, the offsets of the finished blocks are
    // written into the table, the file is synced and `save` is called. Set
    // before begin().
    void set_checkpoints(std::uint64_t interval, std::function<Status(const Checkpoint&)> save);
    // Saves a checkpoint at the blocks written so far (after a pause or an
    // error stopped the input; blocks still being compressed are redone).
    Status save_now() { return save_checkpoint_ && begun_ ? checkpoint() : Status{}; }

    // Starts a new encode, or continues one from `from`: the offset table is
    // read back from the file and checked, and the file is cut at the
    // checkpoint. The caller then writes the input from block `from->blocks`
    // on (input offset blocks * kBlockSize).
    Status begin(const std::optional<Checkpoint>& from = std::nullopt);
    Status write(Bytes data) override;
    Result<WriteResult> finish();

private:
    struct Pipeline;
    Status submit_current(std::uint32_t valid_bytes);
    Status drain_one();

    File& destination_;
    std::uint64_t start_;
    std::uint64_t source_length_;
    CompressionOptions options_;
    ProgressFn progress_;
    std::unique_ptr<Pipeline> pipeline_;
    std::vector<std::uint64_t> offsets_;
    std::uint32_t block_count_ = 0;
    std::uint32_t blocks_done_ = 0;
    std::uint32_t compressed_blocks_ = 0;
    std::uint64_t header_size_ = 0;
    std::uint64_t write_position_ = 0; // relative to start_
    std::uint64_t received_ = 0;
    std::uint64_t last_progress_ = 0;
    bool begun_ = false;
    std::uint64_t checkpoint_interval_ = 0;
    std::function<Status(const Checkpoint&)> save_checkpoint_;
    std::uint32_t checkpoint_blocks_ = 0; // table entries up to here are in the file
    Status checkpoint();
};

// Encodes the whole of `source` into `destination` at `start`.
Result<WriteResult> encode_file(const File& source, File& destination, std::uint64_t start,
                                CompressionOptions options = {}, const ProgressFn& progress = {});

// Reads and validates the header and offset table.
Result<Info> inspect(const RandomAccess& source, std::uint64_t base_offset = 0,
                     std::optional<std::uint64_t> stored_length = std::nullopt);

// Decodes block `index` into `out`, which must hold kBlockSize bytes.
Status read_block(const RandomAccess& source, const Info& info, std::uint32_t index, std::uint64_t base_offset,
                  MutableBytes out);

// Decodes the payload into `destination`. `content_length` trims the padded
// final block; by default the full padded length is written.
Status decode(const RandomAccess& source, ByteSink& destination, std::optional<std::uint64_t> content_length = std::nullopt,
              std::uint64_t base_offset = 0, std::optional<std::uint64_t> stored_length = std::nullopt,
              const ProgressFn& progress = {});

// Random access to a PFSC payload, decoding 64 KiB blocks on demand with a
// small LRU cache. Not thread-safe.
class Reader final : public RandomAccess {
public:
    // `content_length` trims the padded final block (default: padded length).
    static Result<std::shared_ptr<Reader>> open(std::shared_ptr<const RandomAccess> source, std::uint64_t base_offset = 0,
                                                std::optional<std::uint64_t> stored_length = std::nullopt,
                                                std::optional<std::uint64_t> content_length = std::nullopt);
    Result<std::uint64_t> size() const override { return length_; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override;
    const Info& info() const { return info_; }

private:
    struct CachedBlock {
        std::uint32_t index;
        std::uint64_t last_used;
        std::vector<std::uint8_t> data;
    };
    Reader() = default;
    Result<const std::uint8_t*> block(std::uint32_t index) const;

    std::shared_ptr<const RandomAccess> source_;
    std::uint64_t base_offset_ = 0;
    std::uint64_t length_ = 0;
    Info info_;
    mutable std::vector<CachedBlock> cache_;
    mutable std::uint64_t clock_ = 0;
};

} // namespace alembic::pfsc
