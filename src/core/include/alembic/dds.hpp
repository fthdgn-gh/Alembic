// SPDX-License-Identifier: GPL-3.0-or-later
// Smaller home-screen pictures: a game's sce_sys/pic*.dds are 3840x2160 BC7
// (8 bits a pixel). A console test by the reporter of issue #1 showed BC1
// (4 bits a pixel, no alpha) works too, the pic0/pic1/pic2 animation
// included; BC3 did not. Each one is encoded again as BC1 from the PNG of the
// same name (the lossless original; BC7 to BC1 would lose quality twice),
// keeping the DDS header but for its format and size. Opt-in.
//
// Written from Microsoft's DDS and BC1 documentation; the encoder fits each
// 4x4 block's colours to a line (principal axis), then refines the two end
// colours by least squares on the chosen indices.
#pragma once

#include "alembic/io.hpp"
#include "alembic/png.hpp"
#include "alembic/result.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace alembic::dds {

constexpr std::size_t kHeaderSize = 148; // "DDS ", DDS_HEADER, DDS_HEADER_DXT10
constexpr std::uint32_t kBc1Unorm = 71, kBc1Srgb = 72, kBc7Unorm = 98, kBc7Srgb = 99;

struct Info {
    std::uint32_t width = 0, height = 0, mips = 0;
    std::uint32_t dxgi_format = 0; // 0 without a DX10 header
};
// The first kHeaderSize bytes of a DDS file.
Result<Info> read_info(Bytes head);

// BC1 blocks of `image` (width and height multiples of 4), 8 bytes per 4x4
// block, rows of blocks top to bottom. Always the 4-colour mode (alpha is
// ignored), so no texel reads as transparent.
std::vector<std::uint8_t> encode_bc1(const png::Image& image, unsigned threads = 0);

// A single-level BC1 DDS of `image` from a BC7 one's header: the same
// header with BC1's format (sRGB kept) and linear size.
Result<std::vector<std::uint8_t>> bc1_like(Bytes bc7_header, const png::Image& image, unsigned threads = 0);

// Whether a tree path is one of the pictures this changes: sce_sys/pic<N>.dds
// (any case), not icon0.dds or any other.
bool is_picture_path(std::string_view game_relative_path);

struct Smaller {
    // `source` with the pictures re-encoded (and ampr_emu.index's records of
    // them resized); null when none could be.
    std::unique_ptr<tree::Source> tree;
    std::vector<std::string> paths;  // tree paths re-encoded
    std::uint64_t saved = 0;         // bytes
    std::vector<std::pair<std::string, std::string>> skipped; // tree path, why
    std::size_t index_records_resized = 0;
};
// The pictures of the game at `source`'s game root. A picture is skipped
// (left as it is) when it is not a single-level BC7 DDS with a DX10 header,
// its PNG is missing, unreadable or of another size, or a side is not a
// multiple of 4.
Result<Smaller> smaller_pictures(const tree::Source& source, const ProgressFn& progress = {}, unsigned threads = 0);

} // namespace alembic::dds
