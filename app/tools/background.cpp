// SPDX-License-Identifier: GPL-3.0-or-later
// The console app's home-screen background (sce_sys/pic0.dds, shown behind
// the selected tile, and pic1.dds, shown while it starts): drawn here from
// the icon's shapes (assets/icon/alembic.svg) on the app's colours, and
// written as the single 3840x2160 BC7_UNORM DX10 DDS the console takes.
// Nothing is read: the picture is code, so any machine builds the same bytes.
//
//   background <out.dds> [preview.png]   (the preview is a quarter the size)
//
// The left part stays calm: the home screen puts the title and the Play
// button there.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int W = 3840, H = 2160;

struct Rgb {
    float r, g, b;
};
Rgb hex(std::uint32_t c) { return {((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f}; }
Rgb mix(Rgb a, Rgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
float clamp01(float v) { return std::min(1.0f, std::max(0.0f, v)); }
float smooth(float e0, float e1, float x) {
    float t = clamp01((x - e0) / (e1 - e0));
    return t * t * (3 - 2 * t);
}

// ---- shapes, in the icon's 512-unit coordinates ----

float circle(float x, float y, float cx, float cy, float r) { return std::hypot(x - cx, y - cy) - r; }

float segment(float x, float y, float ax, float ay, float bx, float by) {
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float t = clamp01((px * dx + py * dy) / (dx * dx + dy * dy));
    return std::hypot(px - dx * t, py - dy * t);
}

// The neck and spout: the icon's cubic from (222,212) to (404,262), as a polyline.
struct Neck {
    std::vector<std::array<float, 2>> p;
    Neck() {
        const float x0 = 222, y0 = 212, x1 = 236, y1 = 136, x2 = 330, y2 = 116, x3 = 404, y3 = 262;
        for (int i = 0; i <= 96; ++i) {
            float t = i / 96.0f, u = 1 - t;
            p.push_back({u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                         u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3});
        }
    }
    float distance(float x, float y) const {
        float d = 1e9f;
        for (std::size_t i = 1; i < p.size(); ++i) d = std::min(d, segment(x, y, p[i - 1][0], p[i - 1][1], p[i][0], p[i][1]));
        return d;
    }
};

// The falling drop: a circle with a point above it.
float drop(float x, float y) {
    const float cx = 410, cy = 372, r = 24, tip = 318;
    float dc = circle(x, y, cx, cy, r);
    // The cone from the tip to the circle's tangent points.
    float h = cy - tip, s = r / h, c = std::sqrt(1 - s * s);
    float ty = cy - r * s, tx = r * c;
    float dl = segment(x, y, cx, tip, cx - tx, ty), dr = segment(x, y, cx, tip, cx + tx, ty);
    bool inside_cone = y >= tip && y <= ty && std::fabs(x - cx) <= (y - tip) / (ty - tip) * tx;
    float dcone = inside_cone ? -std::min(dl, dr) : std::min(dl, dr);
    return std::min(dc, dcone);
}

// The liquid's surface: a gentle wave across the flask.
float wave(float x) { return 331.0f - 11.0f * std::sin((x - 206.0f) / 256.0f * 6.2831853f); }

// ---- the picture ----

Rgb pixel(float px, float py, const Neck& neck) {
    const Rgb bg = hex(0x0b0e14), bg_far = hex(0x111829), accent = hex(0x6f93ff), accent2 = hex(0x9a7bff),
              teal = hex(0x3fb68f), mint = hex(0x7fe0c2), white = {1, 1, 1};
    float fx = px / W, fy = py / H;
    Rgb c = mix(bg, bg_far, clamp01(0.55f * fx + 0.35f * (1 - fy)));
    auto glow = [&](Rgb color, float gx, float gy, float radius, float strength) {
        float d = std::hypot((px - gx * W) / (radius * H), (py - gy * H) / (radius * H));
        c = mix(c, color, strength * std::exp(-d * d));
    };
    glow(accent, -0.05f, -0.15f, 0.55f, 0.10f); // the page's light at the top left
    glow(accent, 0.68f, 0.28f, 0.48f, 0.30f);
    glow(accent2, 0.86f, 0.80f, 0.52f, 0.30f);

    // The retort, large on the right: icon unit -> pixels.
    const float S = 3.55f, ox = 0.705f * W - 256 * S, oy = 0.53f * H - 300 * S;
    const float x = (px - ox) / S, y = (py - oy) / S, aa = 0.75f / S; // half a pixel, in icon units
    auto cover = [&](float d) { return clamp01(0.5f - d / (2 * aa)); };

    float body = circle(x, y, 206, 318, 112);
    float tube = neck.distance(x, y) - 24;
    float glass = std::min(body, tube);
    float dd = drop(x, y);
    glow(teal, (ox + 410 * S) / W, (oy + 372 * S) / H, 0.16f, 0.22f);

    // Glass: a faint fill, a lit rim and a reflection.
    if (glass < 8) {
        c = mix(c, white, 0.045f * cover(glass));
        // Liquid inside the body, lighter at the top.
        float liquid = std::max(body + 5, wave(x) - y);
        if (liquid < 2) {
            Rgb l = mix(mint, teal, clamp01((y - 320) / 110));
            c = mix(c, l, 0.88f * cover(liquid));
            for (auto b : {std::array<float, 3>{170, 380, 13}, {222, 400, 9}, {246, 366, 7}})
                c = mix(c, white, 0.55f * cover(circle(x, y, b[0], b[1], b[2])));
            // The surface catches the light (only the wave, not along the glass).
            c = mix(c, white, 0.35f * cover(std::fabs(wave(x) - y) - 1.2f) * cover(body + 7));
        }
        float rim = std::fabs(glass) - 2.6f;
        c = mix(c, white, 0.78f * cover(rim));
        // The reflection: an arc on the upper left of the body.
        float r = std::hypot(x - 206, y - 318), a = std::atan2(y - 318, x - 206);
        float arc = std::max(std::fabs(r - 88) - 4.5f, 0.0f) + std::max(0.0f, std::fabs(a + 2.25f) - 0.42f) * 88;
        c = mix(c, white, 0.30f * cover(arc));
    }
    // The drop, glowing.
    c = mix(c, mix(mint, white, 0.25f), 0.95f * cover(dd));

    // A little darker at the bottom, where the shell draws its controls.
    c = mix(c, bg, 0.35f * smooth(0.70f, 1.0f, fy));
    return c;
}

// ---- BC7 mode 6: one subset, RGBA endpoints of 7 bits plus a p-bit, 4-bit indices ----

constexpr int kWeights[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Bits {
    std::array<std::uint8_t, 16> b{};
    int at = 0;
    void put(std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i, ++at)
            if ((v >> i) & 1) b[at / 8] |= static_cast<std::uint8_t>(1u << (at % 8));
    }
};

// Endpoints are (c7 << 1) | 1: alpha stays 255 and colour keeps 7 bits.
int expand(int c7) { return (c7 << 1) | 1; }
int quantize(float v) { return std::clamp(static_cast<int>(std::lround((v - 1) / 2)), 0, 127); }

struct Fit {
    std::array<std::array<int, 3>, 2> e; // 7-bit
    std::array<int, 16> idx;
    float err;
};

Fit fit(const std::array<std::array<float, 3>, 16>& px, std::array<std::array<float, 3>, 2> ends) {
    Fit f;
    for (int k = 0; k < 2; ++k)
        for (int ch = 0; ch < 3; ++ch) f.e[k][ch] = quantize(ends[k][ch]);
    std::array<std::array<float, 3>, 16> pal;
    for (int i = 0; i < 16; ++i)
        for (int ch = 0; ch < 3; ++ch)
            pal[i][ch] = static_cast<float>(((64 - kWeights[i]) * expand(f.e[0][ch]) + kWeights[i] * expand(f.e[1][ch]) + 32) >> 6);
    f.err = 0;
    for (int p = 0; p < 16; ++p) {
        float best = 1e30f;
        for (int i = 0; i < 16; ++i) {
            float d = 0;
            for (int ch = 0; ch < 3; ++ch) d += (pal[i][ch] - px[p][ch]) * (pal[i][ch] - px[p][ch]);
            if (d < best) best = d, f.idx[p] = i;
        }
        f.err += best;
    }
    return f;
}

std::array<std::uint8_t, 16> encode_block(const std::array<std::array<float, 3>, 16>& px) {
    // The principal axis through the block's colours.
    std::array<float, 3> mean{};
    for (auto& p : px)
        for (int ch = 0; ch < 3; ++ch) mean[ch] += p[ch] / 16;
    float cov[3][3] = {};
    for (auto& p : px)
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) cov[i][j] += (p[i] - mean[i]) * (p[j] - mean[j]);
    std::array<float, 3> axis{1, 1, 1};
    for (int it = 0; it < 8; ++it) {
        std::array<float, 3> n{};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) n[i] += cov[i][j] * axis[j];
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len < 1e-6f) break;
        for (int i = 0; i < 3; ++i) axis[i] = n[i] / len;
    }
    float lo = 1e30f, hi = -1e30f;
    for (auto& p : px) {
        float t = (p[0] - mean[0]) * axis[0] + (p[1] - mean[1]) * axis[1] + (p[2] - mean[2]) * axis[2];
        lo = std::min(lo, t), hi = std::max(hi, t);
    }
    std::array<std::array<float, 3>, 2> ends;
    for (int ch = 0; ch < 3; ++ch) ends[0][ch] = mean[ch] + axis[ch] * lo, ends[1][ch] = mean[ch] + axis[ch] * hi;
    Fit best = fit(px, ends);
    // Least-squares endpoints for the chosen indices, while that helps.
    for (int round = 0; round < 2; ++round) {
        float a = 0, b = 0, c = 0;
        std::array<float, 3> x0{}, x1{};
        for (int p = 0; p < 16; ++p) {
            float w = kWeights[best.idx[p]] / 64.0f, u = 1 - w;
            a += u * u, b += u * w, c += w * w;
            for (int ch = 0; ch < 3; ++ch) x0[ch] += u * px[p][ch], x1[ch] += w * px[p][ch];
        }
        float det = a * c - b * b;
        if (std::fabs(det) < 1e-6f) break;
        for (int ch = 0; ch < 3; ++ch) {
            ends[0][ch] = std::clamp((c * x0[ch] - b * x1[ch]) / det, 0.0f, 255.0f);
            ends[1][ch] = std::clamp((a * x1[ch] - b * x0[ch]) / det, 0.0f, 255.0f);
        }
        Fit f = fit(px, ends);
        if (f.err >= best.err) break;
        best = f;
    }
    // The first index's top bit is implied 0: swap the endpoints when it is set.
    if (best.idx[0] >= 8) {
        std::swap(best.e[0], best.e[1]);
        for (int& i : best.idx) i = 15 - i;
    }
    Bits out;
    out.put(1u << 6, 7); // mode 6
    for (int ch = 0; ch < 3; ++ch) out.put(static_cast<std::uint32_t>(best.e[0][ch]), 7), out.put(static_cast<std::uint32_t>(best.e[1][ch]), 7);
    out.put(127, 7), out.put(127, 7); // alpha
    out.put(1, 1), out.put(1, 1);     // p-bits
    out.put(static_cast<std::uint32_t>(best.idx[0]), 3);
    for (int p = 1; p < 16; ++p) out.put(static_cast<std::uint32_t>(best.idx[p]), 4);
    return out.b;
}

// ---- files ----

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (8 * i)));
}

std::vector<std::uint8_t> dds_header() {
    std::vector<std::uint8_t> h = {'D', 'D', 'S', ' '};
    put32(h, 124);
    put32(h, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000); // caps, height, width, pixel format, linear size
    put32(h, H);
    put32(h, W);
    put32(h, W / 4 * (H / 4) * 16);
    put32(h, 0); // depth
    put32(h, 1); // mipmaps
    for (int i = 0; i < 11; ++i) put32(h, 0);
    put32(h, 32);  // pixel format size
    put32(h, 0x4); // FOURCC
    h.insert(h.end(), {'D', 'X', '1', '0'});
    for (int i = 0; i < 5; ++i) put32(h, 0);
    put32(h, 0x1000); // texture
    for (int i = 0; i < 4; ++i) put32(h, 0);
    put32(h, 98); // DXGI_FORMAT_BC7_UNORM
    put32(h, 3);  // 2D
    put32(h, 0);
    put32(h, 1); // array size
    put32(h, 0);
    return h;
}

std::uint32_t crc32(const std::uint8_t* p, std::size_t n, std::uint32_t c = 0) {
    c = ~c;
    for (std::size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

// A PNG with stored (uncompressed) deflate blocks: no zlib needed.
bool write_png(const std::string& path, int w, int h, const std::vector<std::uint8_t>& rgb) {
    std::vector<std::uint8_t> raw;
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + static_cast<std::ptrdiff_t>(y) * w * 3, rgb.begin() + static_cast<std::ptrdiff_t>(y + 1) * w * 3);
    }
    std::vector<std::uint8_t> z = {0x78, 0x01};
    std::uint32_t a = 1, b = 0;
    for (std::uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
    for (std::size_t at = 0; at < raw.size();) {
        std::size_t n = std::min<std::size_t>(65535, raw.size() - at);
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n)), z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n)), z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<std::ptrdiff_t>(at), raw.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    std::uint32_t adler = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back(static_cast<std::uint8_t>(adler >> (8 * i)));
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        std::uint8_t len[4] = {static_cast<std::uint8_t>(data.size() >> 24), static_cast<std::uint8_t>(data.size() >> 16),
                               static_cast<std::uint8_t>(data.size() >> 8), static_cast<std::uint8_t>(data.size())};
        std::fwrite(len, 1, 4, f);
        std::vector<std::uint8_t> td(type, type + 4);
        td.insert(td.end(), data.begin(), data.end());
        std::fwrite(td.data(), 1, td.size(), f);
        std::uint32_t c = crc32(td.data(), td.size());
        std::uint8_t cb[4] = {static_cast<std::uint8_t>(c >> 24), static_cast<std::uint8_t>(c >> 16), static_cast<std::uint8_t>(c >> 8),
                              static_cast<std::uint8_t>(c)};
        std::fwrite(cb, 1, 4, f);
    };
    std::fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    std::vector<std::uint8_t> ihdr;
    for (int v : {w, h})
        for (int i = 3; i >= 0; --i) ihdr.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    return std::fclose(f) == 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: %s <out.dds> [preview.png]\n", argv[0]);
        return 2;
    }
    const Neck neck;
    std::vector<std::uint8_t> img(static_cast<std::size_t>(W) * H * 3);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            Rgb c = pixel(x + 0.5f, y + 0.5f, neck);
            // A fixed ordered dither against banding in the dark gradients.
            static const float kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
            float d = (kBayer[y & 3][x & 3] + 0.5f) / 16 - 0.5f;
            std::uint8_t* o = &img[(static_cast<std::size_t>(y) * W + x) * 3];
            o[0] = static_cast<std::uint8_t>(std::clamp(c.r * 255 + d, 0.0f, 255.0f) + 0.5f);
            o[1] = static_cast<std::uint8_t>(std::clamp(c.g * 255 + d, 0.0f, 255.0f) + 0.5f);
            o[2] = static_cast<std::uint8_t>(std::clamp(c.b * 255 + d, 0.0f, 255.0f) + 0.5f);
        }
    std::vector<std::uint8_t> dds = dds_header();
    for (int by = 0; by < H; by += 4)
        for (int bx = 0; bx < W; bx += 4) {
            std::array<std::array<float, 3>, 16> px;
            for (int i = 0; i < 16; ++i) {
                const std::uint8_t* s = &img[(static_cast<std::size_t>(by + i / 4) * W + bx + i % 4) * 3];
                px[i] = {static_cast<float>(s[0]), static_cast<float>(s[1]), static_cast<float>(s[2])};
            }
            auto block = encode_block(px);
            dds.insert(dds.end(), block.begin(), block.end());
        }
    std::FILE* f = std::fopen(argv[1], "wb");
    if (!f || std::fwrite(dds.data(), 1, dds.size(), f) != dds.size() || std::fclose(f) != 0) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    if (argc == 3) {
        const int pw = W / 4, ph = H / 4;
        std::vector<std::uint8_t> small(static_cast<std::size_t>(pw) * ph * 3);
        for (int y = 0; y < ph; ++y)
            for (int x = 0; x < pw; ++x)
                for (int ch = 0; ch < 3; ++ch) {
                    int sum = 0;
                    for (int j = 0; j < 4; ++j)
                        for (int i = 0; i < 4; ++i) sum += img[(static_cast<std::size_t>(y * 4 + j) * W + x * 4 + i) * 3 + ch];
                    small[(static_cast<std::size_t>(y) * pw + x) * 3 + ch] = static_cast<std::uint8_t>(sum / 16);
                }
        if (!write_png(argv[2], pw, ph, small)) {
            std::fprintf(stderr, "cannot write %s\n", argv[2]);
            return 1;
        }
    }
    return 0;
}
