// SPDX-License-Identifier: GPL-3.0-or-later
// A PNG decoder for the pictures in a game's sce_sys (dds.hpp encodes them
// for the home screen). Written from the PNG specification (ISO/IEC 15948):
// every colour type at 8 or 16 bits (palette and grey at 1-8), no
// interlacing (Adam7 is refused), chunk CRCs checked, zlib from third_party.
#pragma once

#include "alembic/io.hpp"
#include "alembic/result.hpp"

#include <cstdint>
#include <utility>
#include <vector>

namespace alembic::png {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba; // width * height * 4, rows top to bottom
};

// The width and height from the IHDR chunk (the first 24 bytes are enough).
Result<std::pair<std::uint32_t, std::uint32_t>> dimensions(Bytes head);

// The whole picture as 8-bit RGBA (16-bit samples keep their high byte).
// At most 16384 pixels a side.
Result<Image> decode(Bytes file);

} // namespace alembic::png
