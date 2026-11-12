// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/png.hpp"

#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <string_view>

namespace alembic::png {

namespace {

constexpr std::uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::uint32_t kMaxSide = 16384;

std::uint32_t be32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} << 24 | std::uint32_t{p[1]} << 16 | std::uint32_t{p[2]} << 8 | p[3];
}

Error bad(const std::string& why) { return make_error(Errc::invalid_data, "PNG: " + why); }

struct Header {
    std::uint32_t width = 0, height = 0;
    int depth = 0, color = 0, interlace = 0;
};

Result<Header> read_header(Bytes file) {
    if (file.size() < 33 || std::memcmp(file.data(), kSignature, 8) != 0) return bad("not a PNG file");
    const std::uint8_t* c = file.data() + 8;
    if (be32(c) != 13 || std::memcmp(c + 4, "IHDR", 4) != 0) return bad("no IHDR chunk first");
    Header h;
    h.width = be32(c + 8);
    h.height = be32(c + 12);
    h.depth = c[16];
    h.color = c[17];
    h.interlace = c[20];
    if (h.width == 0 || h.height == 0 || h.width > kMaxSide || h.height > kMaxSide) return bad("unsupported size");
    if (c[18] != 0 || c[19] != 0) return bad("unknown compression or filter method");
    const int d = h.depth;
    bool ok = false;
    switch (h.color) {
    case 0: ok = d == 1 || d == 2 || d == 4 || d == 8 || d == 16; break;
    case 3: ok = d == 1 || d == 2 || d == 4 || d == 8; break;
    case 2:
    case 4:
    case 6: ok = d == 8 || d == 16; break;
    default: break;
    }
    if (!ok) return bad("invalid colour type and bit depth");
    if (h.interlace > 1) return bad("unknown interlace method");
    return h;
}

int channels(int color) {
    switch (color) {
    case 2: return 3;
    case 4: return 2;
    case 6: return 4;
    default: return 1; // grey, palette
    }
}

std::uint8_t paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return static_cast<std::uint8_t>(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

} // namespace

Result<std::pair<std::uint32_t, std::uint32_t>> dimensions(Bytes head) {
    ALEMBIC_ASSIGN(Header h, read_header(head));
    return std::pair{h.width, h.height};
}

Result<Image> decode(Bytes file) {
    ALEMBIC_ASSIGN(Header h, read_header(file));
    if (h.interlace) return make_error(Errc::unsupported, "PNG: interlaced pictures are not supported");
    const int ch = channels(h.color);
    const std::uint64_t bits = std::uint64_t{h.width} * ch * h.depth;
    const std::size_t stride = static_cast<std::size_t>((bits + 7) / 8);
    const std::size_t step = std::max<std::size_t>(1, static_cast<std::size_t>(ch * h.depth / 8)); // filter's "bpp"
    std::vector<std::uint8_t> raw((stride + 1) * h.height);

    std::uint8_t palette[256][4];
    for (auto& p : palette) p[0] = p[1] = p[2] = 0, p[3] = 255;
    std::size_t palette_size = 0;

    z_stream z{};
    if (inflateInit(&z) != Z_OK) return make_error(Errc::io, "zlib inflateInit failed");
    struct End {
        z_stream* z;
        ~End() { inflateEnd(z); }
    } end{&z};
    z.next_out = raw.data();
    z.avail_out = static_cast<uInt>(raw.size());
    bool stream_end = false, seen_end = false, seen_data = false;
    for (std::size_t at = 8; at < file.size();) {
        if (file.size() - at < 12) return bad("truncated chunk");
        const std::uint32_t length = be32(file.data() + at);
        if (length > file.size() - at - 12) return bad("truncated chunk");
        const std::uint8_t* type = file.data() + at + 4;
        const std::uint8_t* data = type + 4;
        const std::uint32_t crc = static_cast<std::uint32_t>(crc32(0, type, length + 4));
        if (crc != be32(data + length)) return bad("chunk CRC mismatch");
        at += 12 + std::size_t{length};
        const std::string_view name(reinterpret_cast<const char*>(type), 4);
        if (name == "IHDR") {
            continue; // read above
        } else if (name == "PLTE") {
            if (length % 3 || length / 3 > 256) return bad("bad palette");
            palette_size = length / 3;
            for (std::size_t i = 0; i < palette_size; ++i)
                for (int k = 0; k < 3; ++k) palette[i][k] = data[i * 3 + k];
        } else if (name == "tRNS" && h.color == 3) {
            for (std::size_t i = 0; i < length && i < 256; ++i) palette[i][3] = data[i];
        } else if (name == "IDAT") {
            seen_data = true;
            if (stream_end) continue; // padding after the stream
            z.next_in = const_cast<Bytef*>(data);
            z.avail_in = length;
            while (z.avail_in) {
                const int rc = inflate(&z, Z_NO_FLUSH);
                // The end, or the picture is complete and only the stream's tail is left.
                if (rc == Z_STREAM_END || (rc == Z_BUF_ERROR && z.avail_out == 0)) {
                    stream_end = true;
                    break;
                }
                if (rc != Z_OK) return bad("damaged image data");
                if (z.avail_out == 0 && z.avail_in) return bad("more image data than the picture holds");
            }
        } else if (name == "IEND") {
            seen_end = true;
            break;
        } else if (!(type[0] & 0x20)) {
            return bad("unknown critical chunk " + std::string(name));
        }
    }
    if (!seen_data || !seen_end) return bad("missing IDAT or IEND");
    if (z.avail_out != 0) return bad("less image data than the picture holds");
    if (h.color == 3 && palette_size == 0) return bad("no palette");

    // Undo the filters, row by row (each row starts with its filter type).
    std::vector<std::uint8_t> prior(stride, 0);
    for (std::uint32_t y = 0; y < h.height; ++y) {
        std::uint8_t* row = raw.data() + std::size_t{y} * (stride + 1);
        const std::uint8_t filter = row[0];
        std::uint8_t* cur = row + 1;
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= step ? cur[i - step] : 0, b = prior[i], c = i >= step ? prior[i - step] : 0;
            switch (filter) {
            case 0: break;
            case 1: cur[i] = static_cast<std::uint8_t>(cur[i] + a); break;
            case 2: cur[i] = static_cast<std::uint8_t>(cur[i] + b); break;
            case 3: cur[i] = static_cast<std::uint8_t>(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = static_cast<std::uint8_t>(cur[i] + paeth(a, b, c)); break;
            default: return bad("unknown filter type");
            }
        }
        std::memcpy(prior.data(), cur, stride);
    }

    Image out;
    out.width = h.width;
    out.height = h.height;
    out.rgba.resize(std::size_t{h.width} * h.height * 4);
    const int d = h.depth;
    for (std::uint32_t y = 0; y < h.height; ++y) {
        const std::uint8_t* src = raw.data() + std::size_t{y} * (stride + 1) + 1;
        std::uint8_t* dst = out.rgba.data() + std::size_t{y} * h.width * 4;
        // Sample `s` of the row: 8 bits (high byte of a 16-bit one), or a 1-4 bit value.
        auto sample8 = [&](std::size_t s) -> std::uint8_t { return d == 16 ? src[s * 2] : src[s]; };
        auto small = [&](std::size_t s) -> int {
            const std::size_t bit = s * d;
            return (src[bit / 8] >> (8 - d - bit % 8)) & ((1 << d) - 1);
        };
        for (std::uint32_t x = 0; x < h.width; ++x, dst += 4) {
            switch (h.color) {
            case 0: {
                const std::uint8_t g = d >= 8 ? sample8(x) : static_cast<std::uint8_t>(small(x) * 255 / ((1 << d) - 1));
                dst[0] = dst[1] = dst[2] = g;
                dst[3] = 255;
                break;
            }
            case 3: {
                const int i = d == 8 ? src[x] : small(x);
                if (static_cast<std::size_t>(i) >= palette_size) return bad("palette index out of range");
                std::memcpy(dst, palette[i], 4);
                break;
            }
            case 2:
                for (int k = 0; k < 3; ++k) dst[k] = sample8(std::size_t{x} * 3 + k);
                dst[3] = 255;
                break;
            case 4:
                dst[0] = dst[1] = dst[2] = sample8(std::size_t{x} * 2);
                dst[3] = sample8(std::size_t{x} * 2 + 1);
                break;
            case 6:
                for (int k = 0; k < 4; ++k) dst[k] = sample8(std::size_t{x} * 4 + k);
                break;
            }
        }
    }
    return out;
}

} // namespace alembic::png
