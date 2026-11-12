// SPDX-License-Identifier: GPL-3.0-or-later
// Smaller home-screen pictures: the PNG decoder, the BC1 encoder and the tree
// step that replaces sce_sys/pic*.dds (dds.hpp).
#include "alembic/ampr.hpp"
#include "alembic/bytes.hpp"
#include "alembic/convert.hpp"
#include "alembic/dds.hpp"
#include "alembic/png.hpp"
#include "test.hpp"

#include <zlib.h>

#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace alembic;

namespace {

void be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    be32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    be32(out, static_cast<std::uint32_t>(crc32(0, out.data() + start, static_cast<uInt>(data.size() + 4))));
}

// A PNG of `samples` (rows of width * channels samples, `depth` 8 or 16),
// each row with the next filter type (0-4), split into two IDAT chunks.
std::vector<std::uint8_t> make_png(std::uint32_t w, std::uint32_t h, int color, int depth, const std::vector<std::uint16_t>& samples,
                                   bool interlaced = false) {
    const int ch = color == 2 ? 3 : color == 4 ? 2 : color == 6 ? 4 : 1;
    const std::size_t bpp = static_cast<std::size_t>(ch * depth / 8), stride = w * bpp;
    std::vector<std::uint8_t> raw, prior(stride, 0);
    for (std::uint32_t y = 0; y < h; ++y) {
        std::vector<std::uint8_t> row(stride);
        for (std::size_t s = 0; s < std::size_t{w} * ch; ++s) {
            const std::uint16_t v = samples[y * w * ch + s];
            if (depth == 16) row[s * 2] = static_cast<std::uint8_t>(v >> 8), row[s * 2 + 1] = static_cast<std::uint8_t>(v);
            else row[s] = static_cast<std::uint8_t>(v);
        }
        const int filter = static_cast<int>(y % 5);
        raw.push_back(static_cast<std::uint8_t>(filter));
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= bpp ? row[i - bpp] : 0, b = prior[i], c = i >= bpp ? prior[i - bpp] : 0;
            int p = 0;
            if (filter == 1) p = a;
            if (filter == 2) p = b;
            if (filter == 3) p = (a + b) >> 1;
            if (filter == 4) {
                const int e = a + b - c, pa = std::abs(e - a), pb = std::abs(e - b), pc = std::abs(e - c);
                p = pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
            }
            raw.push_back(static_cast<std::uint8_t>(row[i] - p));
        }
        prior = row;
    }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<std::uint8_t> z(size);
    compress(z.data(), &size, raw.data(), static_cast<uLong>(raw.size()));
    z.resize(size);
    std::vector<std::uint8_t> out = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<std::uint8_t> ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.push_back(static_cast<std::uint8_t>(depth));
    ihdr.push_back(static_cast<std::uint8_t>(color));
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(interlaced ? 1 : 0);
    chunk(out, "IHDR", ihdr);
    chunk(out, "tEXt", {'k', 0, 'v'}); // an ancillary chunk is skipped
    const std::size_t half = z.size() / 2;
    chunk(out, "IDAT", std::vector<std::uint8_t>(z.begin(), z.begin() + static_cast<std::ptrdiff_t>(half)));
    chunk(out, "IDAT", std::vector<std::uint8_t>(z.begin() + static_cast<std::ptrdiff_t>(half), z.end()));
    chunk(out, "IEND", {});
    return out;
}

// An RGB picture: smooth gradients, as home-screen art mostly is.
std::vector<std::uint16_t> picture(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint16_t> s;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x) {
            s.push_back(static_cast<std::uint16_t>(x * 255 / (w - 1)));
            s.push_back(static_cast<std::uint16_t>(y * 255 / (h - 1)));
            s.push_back(static_cast<std::uint16_t>((x + y) * 255 / (w + h - 2)));
        }
    return s;
}

// A BC7 DDS's header as the games have them (DX10, one level) and its blocks.
std::vector<std::uint8_t> bc7_dds(std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> d(dds::kHeaderSize + std::size_t{w / 4} * (h / 4) * 16, 0x5A);
    std::memset(d.data(), 0, dds::kHeaderSize);
    put_u32(d.data(), 0x20534444);
    put_u32(d.data() + 4, 124);
    put_u32(d.data() + 8, 0xA1007);
    put_u32(d.data() + 12, h);
    put_u32(d.data() + 16, w);
    put_u32(d.data() + 20, (w / 4) * (h / 4) * 16);
    put_u32(d.data() + 28, 1);
    put_u32(d.data() + 76, 32);
    put_u32(d.data() + 80, 4);
    put_u32(d.data() + 84, 0x30315844);
    put_u32(d.data() + 108, 0x1000);
    put_u32(d.data() + 128, dds::kBc7Unorm);
    put_u32(d.data() + 132, 3);
    put_u32(d.data() + 140, 1);
    return d;
}

std::array<int, 3> rgb565(std::uint16_t v) {
    const int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    return {r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2};
}

// BC1 blocks decoded to RGB (4-colour mode, as the checks expect).
std::vector<std::uint8_t> decode_bc1(const std::uint8_t* blocks, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> out(std::size_t{w} * h * 3);
    for (std::uint32_t by = 0; by < h / 4; ++by)
        for (std::uint32_t bx = 0; bx < w / 4; ++bx) {
            const std::uint8_t* b = blocks + (std::size_t{by} * (w / 4) + bx) * 8;
            const auto a = rgb565(get_u16(b)), c = rgb565(get_u16(b + 2));
            std::array<int, 3> pal[4] = {a, c, {}, {}};
            for (int k = 0; k < 3; ++k) pal[2][k] = (2 * a[k] + c[k]) / 3, pal[3][k] = (a[k] + 2 * c[k]) / 3;
            const std::uint32_t bits = get_u32(b + 4);
            for (int i = 0; i < 16; ++i) {
                const auto& p = pal[bits >> (2 * i) & 3];
                std::uint8_t* o = out.data() + ((std::size_t{by} * 4 + i / 4) * w + bx * 4 + i % 4) * 3;
                for (int k = 0; k < 3; ++k) o[k] = static_cast<std::uint8_t>(p[k]);
            }
        }
    return out;
}

double psnr(const std::vector<std::uint8_t>& rgb, const png::Image& image) {
    double se = 0;
    for (std::size_t i = 0; i < std::size_t{image.width} * image.height; ++i)
        for (int k = 0; k < 3; ++k) {
            const double d = double(rgb[i * 3 + k]) - image.rgba[i * 4 + k];
            se += d * d;
        }
    const double mse = se / (double(image.width) * image.height * 3);
    return mse == 0 ? 99.0 : 10 * std::log10(255.0 * 255.0 / mse);
}

std::vector<std::uint8_t> read(const tree::Source& t, const std::string& path) {
    const tree::Node* n = tree::find(t.root(), path);
    if (!n) return {};
    auto f = t.open(*n);
    if (!f.ok()) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(n->size));
    if (!(*f)->read_at(0, bytes).ok()) return {};
    return bytes;
}

} // namespace

TEST_CASE(png_decodes_every_filter_and_format) {
    // RGB, 8 bits, the five filters.
    const auto rgb = picture(13, 11);
    auto image = png::decode(make_png(13, 11, 2, 8, rgb));
    REQUIRE_OK(image);
    CHECK_EQ(image->width, 13u);
    CHECK_EQ(image->height, 11u);
    bool same = true;
    for (std::size_t i = 0; i < 13 * 11; ++i) {
        for (int k = 0; k < 3; ++k) same = same && image->rgba[i * 4 + k] == rgb[i * 3 + k];
        same = same && image->rgba[i * 4 + 3] == 255;
    }
    CHECK(same);
    // RGBA 16 bits: the high bytes.
    std::vector<std::uint16_t> wide;
    for (int i = 0; i < 5 * 6 * 4; ++i) wide.push_back(static_cast<std::uint16_t>(i * 517 + 3));
    auto rgba = png::decode(make_png(5, 6, 6, 16, wide));
    REQUIRE_OK(rgba);
    same = true;
    for (int i = 0; i < 5 * 6 * 4; ++i) same = same && rgba->rgba[i] == (wide[i] >> 8);
    CHECK(same);
    // Grey with alpha.
    std::vector<std::uint16_t> ga;
    for (int i = 0; i < 4 * 4 * 2; ++i) ga.push_back(static_cast<std::uint16_t>(i * 8));
    auto grey = png::decode(make_png(4, 4, 4, 8, ga));
    REQUIRE_OK(grey);
    CHECK_EQ(grey->rgba[5 * 4 + 0], ga[10]);
    CHECK_EQ(grey->rgba[5 * 4 + 2], ga[10]);
    CHECK_EQ(grey->rgba[5 * 4 + 3], ga[11]);
    auto dims = png::dimensions(make_png(13, 11, 2, 8, rgb));
    REQUIRE_OK(dims);
    CHECK_EQ(dims->first, 13u);
}

TEST_CASE(png_refuses_damage_and_interlacing) {
    auto good = make_png(8, 8, 2, 8, picture(8, 8));
    auto bad = good;
    bad[bad.size() - 20] ^= 0x40; // inside the last IDAT: its CRC no longer matches
    CHECK(!png::decode(bad).ok());
    CHECK(!png::decode(std::vector<std::uint8_t>(good.begin(), good.begin() + 40)).ok());
    auto interlaced = png::decode(make_png(8, 8, 2, 8, picture(8, 8), true));
    CHECK(!interlaced.ok());
    CHECK(!png::decode(std::vector<std::uint8_t>(64, 0)).ok());
}

TEST_CASE(bc1_encodes_close_to_the_picture) {
    auto image = png::decode(make_png(64, 32, 2, 8, picture(64, 32)));
    REQUIRE_OK(image);
    auto blocks = dds::encode_bc1(*image, 3);
    CHECK_EQ(blocks.size(), std::size_t{16 * 8 * 8});
    // Always the 4-colour mode (c0 > c1), or one colour throughout.
    bool four = true;
    for (std::size_t i = 0; i < blocks.size(); i += 8)
        four = four && (get_u16(&blocks[i]) > get_u16(&blocks[i + 2]) || get_u32(&blocks[i + 4]) == 0);
    CHECK(four);
    const double q = psnr(decode_bc1(blocks.data(), 64, 32), *image);
    // A steep ramp (4-8 levels a pixel) is near BC1's limit here (36.8 dB);
    // the reference games' pictures come out at 43-44 dB.
    CHECK(q > 36.0);
    // The same with one thread.
    CHECK(dds::encode_bc1(*image, 1) == blocks);
    // A single colour comes back as itself (565 permitting).
    png::Image solid{4, 4, std::vector<std::uint8_t>(64, 0)};
    for (int i = 0; i < 16; ++i) solid.rgba[i * 4] = 255, solid.rgba[i * 4 + 1] = 0, solid.rgba[i * 4 + 2] = 255;
    auto one = dds::encode_bc1(solid);
    auto back = decode_bc1(one.data(), 4, 4);
    CHECK_EQ(int(back[0]), 255);
    CHECK_EQ(int(back[1]), 0);
    CHECK_EQ(int(back[2]), 255);
}

TEST_CASE(bc1_dds_keeps_the_header) {
    auto image = png::decode(make_png(16, 8, 2, 8, picture(16, 8)));
    REQUIRE_OK(image);
    const auto bc7 = bc7_dds(16, 8);
    auto dds_file = dds::bc1_like(Bytes(bc7.data(), dds::kHeaderSize), *image);
    REQUIRE_OK(dds_file);
    CHECK_EQ(dds_file->size(), dds::kHeaderSize + 8 * 8);
    auto info = dds::read_info(*dds_file);
    REQUIRE_OK(info);
    CHECK_EQ(info->dxgi_format, dds::kBc1Unorm);
    CHECK_EQ(info->width, 16u);
    CHECK_EQ(get_u32(dds_file->data() + 20), 64u); // linear size
    // Everything else as it was.
    CHECK(std::memcmp(dds_file->data(), bc7.data(), 20) == 0);
    CHECK(std::memcmp(dds_file->data() + 24, bc7.data() + 24, 128 - 24) == 0);
    CHECK(std::memcmp(dds_file->data() + 132, bc7.data() + 132, dds::kHeaderSize - 132) == 0);
    // Another size than the PNG's, or not BC7: refused.
    CHECK(!dds::bc1_like(Bytes(bc7_dds(16, 12).data(), dds::kHeaderSize), *image).ok());
    auto bc1 = bc7;
    put_u32(bc1.data() + 128, dds::kBc1Unorm);
    CHECK(!dds::bc1_like(Bytes(bc1.data(), dds::kHeaderSize), *image).ok());
    CHECK(dds::is_picture_path("sce_sys/pic0.dds"));
    CHECK(dds::is_picture_path("SCE_SYS/PIC12.DDS"));
    CHECK(!dds::is_picture_path("sce_sys/icon0.dds"));
    CHECK(!dds::is_picture_path("sce_sys/pic.dds"));
    CHECK(!dds::is_picture_path("sce_sys/about/pic0.dds"));
}

TEST_CASE(bc1_pictures_in_conversions) {
    auto dir = test::make_temp_dir();
    std::string root = dir + "/game";
    for (const char* d : {"", "/sce_sys"}) test::make_dir(root + d);
    const char* param = "{\"titleId\":\"ABCD99999\",\"contentVersion\":\"01.000.000\"}";
    test::write_file(root + "/sce_sys/param.json", std::vector<std::uint8_t>(param, param + std::strlen(param)));
    const auto pic = bc7_dds(32, 16);
    test::write_file(root + "/sce_sys/pic0.dds", pic);
    test::write_file(root + "/sce_sys/pic0.png", make_png(32, 16, 2, 8, picture(32, 16)));
    test::write_file(root + "/sce_sys/pic1.dds", pic); // no PNG: left as it is
    test::write_file(root + "/sce_sys/icon0.dds", bc7_dds(8, 8));
    test::write_file(root + "/sce_sys/icon0.png", make_png(8, 8, 2, 8, picture(8, 8)));
    auto index = ampr::build({{"sce_sys/pic0.dds", pic.size(), 7}, {"sce_sys/param.json", std::strlen(param), 7}});
    REQUIRE_OK(index);
    test::write_file(root + "/ampr_emu.index", *index);

    auto folder = tree::open_folder(root);
    REQUIRE_OK(folder);
    auto sm = dds::smaller_pictures(**folder);
    REQUIRE_OK(sm);
    REQUIRE_OK(sm->tree ? Status{} : Status(make_error(Errc::invalid_data, "nothing re-encoded")));
    CHECK_EQ(sm->paths.size(), 1u);
    CHECK_EQ(sm->skipped.size(), 1u);
    CHECK_EQ(sm->index_records_resized, 1u);
    CHECK_EQ(sm->saved, std::uint64_t{8 * 4 * 8});
    const std::uint64_t bc1_size = dds::kHeaderSize + 8 * 4 * 8;
    CHECK_EQ(tree::find(sm->tree->root(), "sce_sys/pic0.dds")->size, bc1_size);
    CHECK(read(*sm->tree, "sce_sys/pic1.dds") == pic);
    auto rebuilt = ampr::build({{"sce_sys/pic0.dds", bc1_size, 7}, {"sce_sys/param.json", std::strlen(param), 7}});
    REQUIRE_OK(rebuilt);
    CHECK(read(*sm->tree, "ampr_emu.index") == *rebuilt);

    // Through a conversion; off by default.
    convert::Options on;
    on.bc1_pictures = true;
    auto report = convert::run(root, dir + "/game.exfat", convert::Format::exfat, on);
    REQUIRE_OK(report);
    CHECK_EQ(report->bc1_pictures.size(), 1u);
    CHECK(report->method.rfind("made the home-screen pictures BC1 and ", 0) == 0);
    auto image = convert::open(dir + "/game.exfat");
    REQUIRE_OK(image);
    auto served = read(*image->tree, "sce_sys/pic0.dds");
    CHECK_EQ(served.size(), bc1_size);
    auto info = dds::read_info(served);
    REQUIRE_OK(info);
    CHECK_EQ(info->dxgi_format, dds::kBc1Unorm);
    CHECK(read(*image->tree, "sce_sys/icon0.dds") == bc7_dds(8, 8));
    auto plain = convert::run(root, dir + "/plain.exfat", convert::Format::exfat);
    REQUIRE_OK(plain);
    CHECK(plain->bc1_pictures.empty());
}
