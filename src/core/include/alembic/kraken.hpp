// SPDX-License-Identifier: GPL-3.0-or-later
// Decoder for the header-stripped Oodle Kraken ("newLZ") blocks inside a PS5
// package's inner image. Ported from pearlxcore/ProsperoPkgTool (MIT)
// Compression/KrakenDecoder.cs, whose structure follows the public "ooz"
// Kraken decoder. Every index derived from the input is bounds-checked, so a
// corrupt block fails cleanly instead of reading or writing out of bounds.
#pragma once

#include "alembic/io.hpp"

namespace alembic::kraken {

inline constexpr std::size_t kChunkSize = 0x20000;    // one sub-chunk
inline constexpr std::size_t kMaxBlockSize = 0x40000; // two sub-chunks

// Per-sub-chunk flags (low nibble: first sub-chunk, high nibble: second).
inline constexpr int kChunk0SubLiterals = 0x01; // delta literals (else raw literals)
inline constexpr int kChunk0NewLz = 0x02;       // newLZ (else a bare entropy array)
inline constexpr int kChunk1SubLiterals = 0x10;
inline constexpr int kChunk1NewLz = 0x20;
inline constexpr int kChunk1Restart = 0x40; // second sub-chunk starts its own window

enum class DecodeStatus { success, malformed, unsupported_entropy };

// Decodes one block of up to 256 KiB into `dst` (whose size is the block's
// decoded size). `first_chunk_size` is the compressed size of the first
// sub-chunk; it is only used when the block has two sub-chunks.
DecodeStatus decode_block(Bytes payload, int flags, std::size_t first_chunk_size, MutableBytes dst);

} // namespace alembic::kraken
