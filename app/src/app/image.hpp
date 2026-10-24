// SPDX-License-Identifier: GPL-3.0-or-later
// PNG decoding for game icons and the app's logo (stb_image, PNG only).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace alembic::app {

struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;
};

// Decodes a PNG; with max_side > 0 it is shrunk (box filter) to fit.
bool decode_png(const std::string& data, int max_side, Image* out);
// The image as "width height" (two little-endian uint32) and its pixels, the
// form the icon fetches hand from the worker thread to the frame loop.
std::string pack_image(const Image& image);
bool unpack_image(const std::string& data, Image* out);

}  // namespace alembic::app
