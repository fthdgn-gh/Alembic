// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/dds.hpp"

#include "alembic/ampr.hpp"
#include "alembic/bytes.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <thread>

namespace alembic::dds {

namespace {

constexpr std::uint32_t kMagic = 0x20534444;         // "DDS "
constexpr std::uint32_t kDx10 = 0x30315844;          // "DX10"
constexpr std::uint32_t kFlagPitch = 0x8, kFlagLinearSize = 0x80000;
constexpr std::uint32_t kTexture2D = 3;

// ---------------------------------------------------------------- BC1

struct Color {
    float r, g, b;
};

std::uint16_t pack565(const Color& c) {
    auto q = [](float v, int max) { return static_cast<int>(std::lround(std::clamp(v, 0.0f, 255.0f) * max / 255.0f)); };
    return static_cast<std::uint16_t>(q(c.r, 31) << 11 | q(c.g, 63) << 5 | q(c.b, 31));
}
Color unpack565(std::uint16_t v) {
    const int r = v >> 11 & 31, g = v >> 5 & 63, b = v & 31;
    return {static_cast<float>(r << 3 | r >> 2), static_cast<float>(g << 2 | g >> 4), static_cast<float>(b << 3 | b >> 2)};
}

float distance2(const Color& a, const Color& b) {
    const float dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr * dr + dg * dg + db * db;
}

// The four colours a decoder makes of two 565 end colours (4-colour mode).
void palette_of(std::uint16_t c0, std::uint16_t c1, Color out[4]) {
    const Color a = unpack565(c0), b = unpack565(c1);
    out[0] = a;
    out[1] = b;
    out[2] = {std::floor((2 * a.r + b.r) / 3), std::floor((2 * a.g + b.g) / 3), std::floor((2 * a.b + b.b) / 3)};
    out[3] = {std::floor((a.r + 2 * b.r) / 3), std::floor((a.g + 2 * b.g) / 3), std::floor((a.b + 2 * b.b) / 3)};
}

// Nearest palette entry per texel; the total squared error.
float choose_indices(const Color px[16], std::uint16_t c0, std::uint16_t c1, std::uint8_t idx[16]) {
    Color pal[4];
    palette_of(c0, c1, pal);
    float total = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        float best_d = distance2(px[i], pal[0]);
        for (int k = 1; k < 4; ++k)
            if (float d = distance2(px[i], pal[k]); d < best_d) best_d = d, best = k;
        idx[i] = static_cast<std::uint8_t>(best);
        total += best_d;
    }
    return total;
}

void encode_block(const Color px[16], std::uint8_t out[8]) {
    Color mean{0, 0, 0};
    for (int i = 0; i < 16; ++i) mean.r += px[i].r, mean.g += px[i].g, mean.b += px[i].b;
    mean.r /= 16, mean.g /= 16, mean.b /= 16;
    float cov[6] = {0, 0, 0, 0, 0, 0}; // rr rg rb gg gb bb
    for (int i = 0; i < 16; ++i) {
        const float r = px[i].r - mean.r, g = px[i].g - mean.g, b = px[i].b - mean.b;
        cov[0] += r * r, cov[1] += r * g, cov[2] += r * b, cov[3] += g * g, cov[4] += g * b, cov[5] += b * b;
    }
    // The principal axis, by power iteration.
    float ax = 1, ay = 1, az = 1;
    for (int it = 0; it < 8; ++it) {
        const float x = cov[0] * ax + cov[1] * ay + cov[2] * az;
        const float y = cov[1] * ax + cov[3] * ay + cov[4] * az;
        const float z = cov[2] * ax + cov[4] * ay + cov[5] * az;
        const float n = std::max({std::fabs(x), std::fabs(y), std::fabs(z)});
        if (n < 1e-6f) break;
        ax = x / n, ay = y / n, az = z / n;
    }
    float lo = 0, hi = 0;
    for (int i = 0; i < 16; ++i) {
        const float t = (px[i].r - mean.r) * ax + (px[i].g - mean.g) * ay + (px[i].b - mean.b) * az;
        lo = std::min(lo, t), hi = std::max(hi, t);
    }
    const float n2 = ax * ax + ay * ay + az * az;
    if (n2 > 0) lo /= n2, hi /= n2;
    Color e0{mean.r + ax * hi, mean.g + ay * hi, mean.b + az * hi};
    Color e1{mean.r + ax * lo, mean.g + ay * lo, mean.b + az * lo};

    std::uint16_t best0 = pack565(e0), best1 = pack565(e1);
    std::uint8_t best_idx[16];
    float best_err = choose_indices(px, best0, best1, best_idx);
    // Refine: the end colours that fit the chosen indices best (least squares).
    static constexpr float kWeight[4] = {0.0f, 1.0f, 1.0f / 3, 2.0f / 3}; // share of c1
    std::uint8_t idx[16];
    std::memcpy(idx, best_idx, 16);
    for (int pass = 0; pass < 3; ++pass) {
        float aa = 0, bb = 0, ab = 0;
        Color ax0{0, 0, 0}, bx1{0, 0, 0};
        for (int i = 0; i < 16; ++i) {
            const float b = kWeight[idx[i]], a = 1 - b;
            aa += a * a, bb += b * b, ab += a * b;
            ax0.r += a * px[i].r, ax0.g += a * px[i].g, ax0.b += a * px[i].b;
            bx1.r += b * px[i].r, bx1.g += b * px[i].g, bx1.b += b * px[i].b;
        }
        const float det = aa * bb - ab * ab;
        if (std::fabs(det) < 1e-6f) break;
        const Color c0{(bb * ax0.r - ab * bx1.r) / det, (bb * ax0.g - ab * bx1.g) / det, (bb * ax0.b - ab * bx1.b) / det};
        const Color c1{(aa * bx1.r - ab * ax0.r) / det, (aa * bx1.g - ab * ax0.g) / det, (aa * bx1.b - ab * ax0.b) / det};
        const std::uint16_t q0 = pack565(c0), q1 = pack565(c1);
        const float err = choose_indices(px, q0, q1, idx);
        if (err >= best_err) break;
        best_err = err, best0 = q0, best1 = q1;
        std::memcpy(best_idx, idx, 16);
    }
    // Then one 565 step at a time on each end and channel, while it helps.
    static constexpr std::uint16_t kStep[3] = {1 << 11, 1 << 5, 1}, kMask[3] = {31 << 11, 63 << 5, 31};
    for (int round = 0; round < 8; ++round) {
        bool better = false;
        for (int end = 0; end < 2; ++end)
            for (int c = 0; c < 3; ++c)
                for (int dir = -1; dir <= 1; dir += 2) {
                    std::uint16_t q[2] = {best0, best1};
                    const int field = q[end] & kMask[c];
                    const int moved = field + dir * kStep[c];
                    if (moved < 0 || moved > kMask[c]) continue;
                    q[end] = static_cast<std::uint16_t>((q[end] & ~kMask[c]) | moved);
                    const float err = choose_indices(px, q[0], q[1], idx);
                    if (err < best_err) {
                        best_err = err, best0 = q[0], best1 = q[1], better = true;
                        std::memcpy(best_idx, idx, 16);
                    }
                }
        if (!better) break;
    }
    // 4-colour mode needs c0 > c1: swap the ends (indices 0<->1, 2<->3);
    // equal ends use index 0 only (in 3-colour mode index 3 is transparent).
    if (best0 < best1) {
        std::swap(best0, best1);
        for (auto& i : best_idx) i ^= 1;
    } else if (best0 == best1) {
        std::memset(best_idx, 0, 16);
    }
    put_u16(out, best0);
    put_u16(out + 2, best1);
    std::uint32_t bits = 0;
    for (int i = 0; i < 16; ++i) bits |= std::uint32_t{best_idx[i]} << (2 * i);
    put_u32(out + 4, bits);
}

// ---------------------------------------------------------------- the tree

class MemoryFile final : public RandomAccess {
public:
    explicit MemoryFile(std::shared_ptr<const std::vector<std::uint8_t>> data) : data_(std::move(data)) {}
    Result<std::uint64_t> size() const override { return data_->size(); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > data_->size() || out.size() > data_->size() - offset)
            return make_error(Errc::invalid_argument, "read past the end");
        std::memcpy(out.data(), data_->data() + offset, out.size());
        return {};
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> data_;
};

// `base` with some files' contents replaced (tree paths, '/'-separated).
class ReplacedTree final : public tree::Source {
public:
    using Contents = std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>>;
    ReplacedTree(const tree::Source& base, Contents contents) : base_(base), contents_(std::move(contents)) {
        root_ = copy(base_.root(), "");
    }
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return base_.label(); }
    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory || file.id >= entries_.size()) return make_error(Errc::invalid_argument, "not a file of this tree");
        const Entry& e = entries_[file.id];
        if (e.data) return std::shared_ptr<const RandomAccess>(std::make_shared<MemoryFile>(e.data));
        return base_.open(*e.node);
    }

private:
    struct Entry {
        const tree::Node* node;
        std::shared_ptr<const std::vector<std::uint8_t>> data;
    };
    tree::Node copy(const tree::Node& in, const std::string& rel) {
        tree::Node out;
        out.name = in.name;
        out.is_directory = in.is_directory;
        out.size = in.size;
        out.mtime = in.mtime;
        if (in.is_directory) {
            out.children.reserve(in.children.size());
            for (const tree::Node& c : in.children) out.children.push_back(copy(c, rel.empty() ? c.name : rel + "/" + c.name));
            return out;
        }
        Entry e{&in, nullptr};
        if (auto it = contents_.find(rel); it != contents_.end()) {
            e.data = it->second;
            out.size = e.data->size();
        }
        entries_.push_back(std::move(e));
        out.id = entries_.size() - 1;
        return out;
    }

    const tree::Source& base_;
    Contents contents_;
    tree::Node root_;
    std::vector<Entry> entries_;
};

Result<std::vector<std::uint8_t>> read_all(const tree::Source& source, const tree::Node& node, std::uint64_t limit) {
    if (node.size > limit) return make_error(Errc::unsupported, "too large");
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(node));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(node.size));
    ALEMBIC_TRY(file->read_at(0, bytes));
    return bytes;
}

} // namespace

Result<Info> read_info(Bytes head) {
    auto bad = [](const char* why) { return make_error(Errc::invalid_data, std::string("DDS: ") + why); };
    if (head.size() < 128 || get_u32(head.data()) != kMagic || get_u32(head.data() + 4) != 124) return bad("not a DDS file");
    Info info;
    info.height = get_u32(head.data() + 12);
    info.width = get_u32(head.data() + 16);
    info.mips = std::max<std::uint32_t>(1, get_u32(head.data() + 28));
    if (get_u32(head.data() + 84) == kDx10) {
        if (head.size() < kHeaderSize) return bad("truncated DX10 header");
        info.dxgi_format = get_u32(head.data() + 128);
    }
    return info;
}

std::vector<std::uint8_t> encode_bc1(const png::Image& image, unsigned threads) {
    const std::uint32_t bw = image.width / 4, bh = image.height / 4;
    std::vector<std::uint8_t> out(std::size_t{bw} * bh * 8);
    if (threads == 0) threads = std::max(1u, std::thread::hardware_concurrency());
    threads = std::min<unsigned>(threads, std::max<std::uint32_t>(1, bh));
    std::atomic<std::uint32_t> next{0};
    auto work = [&] {
        for (std::uint32_t by; (by = next.fetch_add(1)) < bh;) {
            for (std::uint32_t bx = 0; bx < bw; ++bx) {
                Color px[16];
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        const std::uint8_t* p = image.rgba.data() + ((std::size_t{by} * 4 + y) * image.width + bx * 4 + x) * 4;
                        px[y * 4 + x] = {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2])};
                    }
                encode_block(px, out.data() + (std::size_t{by} * bw + bx) * 8);
            }
        }
    };
    std::vector<std::thread> pool;
    for (unsigned i = 1; i < threads; ++i) pool.emplace_back(work);
    work();
    for (std::thread& t : pool) t.join();
    return out;
}

Result<std::vector<std::uint8_t>> bc1_like(Bytes bc7_header, const png::Image& image, unsigned threads) {
    ALEMBIC_ASSIGN(Info info, read_info(bc7_header));
    if (info.dxgi_format != kBc7Unorm && info.dxgi_format != kBc7Srgb)
        return make_error(Errc::unsupported, "not a BC7 picture with a DX10 header");
    if (info.mips != 1) return make_error(Errc::unsupported, "has mipmaps");
    if (get_u32(bc7_header.data() + 132) != kTexture2D || get_u32(bc7_header.data() + 140) > 1)
        return make_error(Errc::unsupported, "not a single 2D texture");
    if (info.width != image.width || info.height != image.height)
        return make_error(Errc::unsupported, "its PNG is " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                                                 ", the DDS " + std::to_string(info.width) + "x" + std::to_string(info.height));
    if (image.width % 4 || image.height % 4) return make_error(Errc::unsupported, "a side is not a multiple of 4");
    std::vector<std::uint8_t> blocks = encode_bc1(image, threads);
    std::vector<std::uint8_t> out(kHeaderSize + blocks.size());
    std::memcpy(out.data(), bc7_header.data(), kHeaderSize);
    put_u32(out.data() + 8, (get_u32(out.data() + 8) & ~kFlagPitch) | kFlagLinearSize);
    put_u32(out.data() + 20, static_cast<std::uint32_t>(blocks.size()));
    put_u32(out.data() + 128, info.dxgi_format == kBc7Srgb ? kBc1Srgb : kBc1Unorm);
    std::memcpy(out.data() + kHeaderSize, blocks.data(), blocks.size());
    return out;
}

bool is_picture_path(std::string_view path) {
    if (path.size() < 8 || !unicode::ascii_iequals(path.substr(0, 8), "sce_sys/")) return false;
    std::string_view name = path.substr(8);
    if (name.size() < 8 || !unicode::ascii_iequals(name.substr(0, 3), "pic") ||
        !unicode::ascii_iequals(name.substr(name.size() - 4), ".dds"))
        return false;
    std::string_view number = name.substr(3, name.size() - 7);
    return !number.empty() && std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; });
}

Result<Smaller> smaller_pictures(const tree::Source& source, const ProgressFn& progress, unsigned threads) {
    Smaller result;
    std::string game_rel;
    auto game = ffpfsc::resolve_game_root(source.root(), &game_rel);
    if (!game.ok()) return result;
    const tree::Node* sce_sys = tree::find_child_icase(**game, "sce_sys");
    if (!sce_sys || !sce_sys->is_directory) return result;
    const std::string prefix = game_rel.empty() ? "" : game_rel + "/";
    ReplacedTree::Contents contents;
    std::map<std::string, std::pair<std::uint64_t, std::uint64_t>> sizes; // game path (lower case) -> old, new
    for (const tree::Node& f : sce_sys->children) {
        const std::string rel = sce_sys->name + "/" + f.name;
        if (f.is_directory || !is_picture_path(rel)) continue;
        const std::string tree_path = prefix + rel;
        if (progress && !progress({"Re-encoding pictures", 0, 0})) return make_error(Errc::cancelled, "cancelled");
        auto skip = [&](const std::string& why) { result.skipped.emplace_back(tree_path, why); };
        // Its PNG: the same name with .png.
        const std::string png_name = f.name.substr(0, f.name.size() - 4) + ".png";
        const tree::Node* png_node = tree::find_child_icase(*sce_sys, png_name);
        if (!png_node || png_node->is_directory) {
            skip("no " + png_name + " to make it from");
            continue;
        }
        if (f.size < kHeaderSize) {
            skip("not a DDS file");
            continue;
        }
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> dds_file, source.open(f));
        std::uint8_t head[kHeaderSize];
        ALEMBIC_TRY(dds_file->read_at(0, MutableBytes(head, sizeof head)));
        auto info = read_info(Bytes(head, sizeof head));
        if (!info.ok() || (info->dxgi_format != kBc7Unorm && info->dxgi_format != kBc7Srgb)) {
            skip("not BC7 with a DX10 header");
            continue;
        }
        auto png_bytes = read_all(source, *png_node, 256u << 20);
        if (!png_bytes.ok()) {
            skip(png_name + ": " + png_bytes.error().message);
            continue;
        }
        auto image = png::decode(*png_bytes);
        if (!image.ok()) {
            skip(png_name + ": " + image.error().message);
            continue;
        }
        auto encoded = bc1_like(Bytes(head, sizeof head), *image, threads);
        if (!encoded.ok()) {
            skip(encoded.error().message);
            continue;
        }
        if (encoded->size() >= f.size) {
            skip("BC1 would not be smaller");
            continue;
        }
        result.saved += f.size - encoded->size();
        std::string key = rel;
        std::transform(key.begin(), key.end(), key.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; });
        sizes[key] = {f.size, encoded->size()};
        contents[tree_path] = std::make_shared<const std::vector<std::uint8_t>>(std::move(*encoded));
        result.paths.push_back(tree_path);
    }
    if (contents.empty()) return result;
    // ampr_emu.index lists sizes (its runtime answers /app0 lookups from it).
    const std::string index_path = prefix + "ampr_emu.index";
    if (const tree::Node* index = tree::find(source.root(), index_path); index && !index->is_directory) {
        auto bytes = read_all(source, *index, 64u << 20);
        if (bytes.ok()) {
            result.index_records_resized =
                ampr::resize_records(*bytes, [&](std::string_view path, std::uint64_t size) -> std::optional<std::uint64_t> {
                    std::string key(path);
                    std::transform(key.begin(), key.end(), key.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; });
                    auto it = sizes.find(key);
                    if (it == sizes.end() || it->second.first != size) return std::nullopt;
                    return it->second.second;
                });
            if (result.index_records_resized)
                contents[index_path] = std::make_shared<const std::vector<std::uint8_t>>(std::move(*bytes));
        }
    }
    result.tree = std::make_unique<ReplacedTree>(source, std::move(contents));
    return result;
}

} // namespace alembic::dds
