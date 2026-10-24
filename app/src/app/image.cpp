// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/image.hpp"

#include <algorithm>
#include <cstring>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_ASSERT(x) ((void)0)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#endif
#include "../../../third_party/stb/stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace alembic::app {

bool decode_png(const std::string& data, int max_side, Image* out) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(data.data()),
                                              static_cast<int>(data.size()), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) return false;
    if (max_side <= 0 || (w <= max_side && h <= max_side)) {
        out->width = w;
        out->height = h;
        out->rgba.assign(px, px + static_cast<std::size_t>(w) * h * 4);
        stbi_image_free(px);
        return true;
    }
    // Box filter: each output pixel averages the source pixels it covers.
    const int ow = w >= h ? max_side : std::max(1, max_side * w / h);
    const int oh = h >= w ? max_side : std::max(1, max_side * h / w);
    out->width = ow;
    out->height = oh;
    out->rgba.assign(static_cast<std::size_t>(ow) * oh * 4, 0);
    for (int y = 0; y < oh; ++y) {
        const int y0 = y * h / oh, y1 = std::max(y0 + 1, (y + 1) * h / oh);
        for (int x = 0; x < ow; ++x) {
            const int x0 = x * w / ow, x1 = std::max(x0 + 1, (x + 1) * w / ow);
            unsigned sum[4] = {0, 0, 0, 0};
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx) {
                    const unsigned char* p = px + (static_cast<std::size_t>(sy) * w + sx) * 4;
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            const unsigned count = static_cast<unsigned>((y1 - y0) * (x1 - x0));
            std::uint8_t* o = &out->rgba[(static_cast<std::size_t>(y) * ow + x) * 4];
            for (int c = 0; c < 4; ++c) o[c] = static_cast<std::uint8_t>(sum[c] / count);
        }
    }
    stbi_image_free(px);
    return true;
}

std::string pack_image(const Image& image) {
    std::string out(8 + image.rgba.size(), '\0');
    const std::uint32_t w = static_cast<std::uint32_t>(image.width), h = static_cast<std::uint32_t>(image.height);
    std::memcpy(&out[0], &w, 4);
    std::memcpy(&out[4], &h, 4);
    if (!image.rgba.empty()) std::memcpy(&out[8], image.rgba.data(), image.rgba.size());
    return out;
}

bool unpack_image(const std::string& data, Image* out) {
    if (data.size() < 8) return false;
    std::uint32_t w = 0, h = 0;
    std::memcpy(&w, data.data(), 4);
    std::memcpy(&h, data.data() + 4, 4);
    if (w == 0 || h == 0 || data.size() != 8 + static_cast<std::size_t>(w) * h * 4) return false;
    out->width = static_cast<int>(w);
    out->height = static_cast<int>(h);
    out->rgba.assign(data.begin() + 8, data.end());
    return true;
}

}  // namespace alembic::app
