// SPDX-License-Identifier: GPL-3.0-or-later
// A whole debug package (FIH): ported from pearlxcore/ProsperoPkgTool (MIT)
// Containers/ProsperoDebugPackageBuilder.cs (Build, RunInMemory,
// BuildSiSegment), CliApplication.cs (img_create's input collection),
// Containers/ProsperoFihBuilder.cs, Containers/ProsperoSiArchive.cs (the
// debug SI zip), Containers/ProsperoNapsMeta.cs (naps_meta_18/300) and
// Content/ProsperoSceSysMedia.cs (Collect).
//
// Package: [FIH header, 64 KiB][outer PFS][CNT][SI zip]. The outer PFS is
// streamed into place first (the inner image flows through it), then the CNT
// and the header are written, and one read-back pass over the package gives
// the SI's whole-image digest and per-block CRC32Cs.
#include "alembic/fpkg_build.hpp"

#include "alembic/bytes.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/json.hpp"
#include "alembic/thread_pool.hpp"

#include <cctype>
#include <tuple>
#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <random>
#include <unistd.h>

namespace alembic::fpkg::build {

namespace {

constexpr std::uint64_t kHeaderRegion = 0x10000;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const char* suffix) {
    std::size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

struct Param {
    std::string content_id;
    std::string content_version; // empty when absent
    std::string drm_type;        // empty when absent
};

Result<Param> read_param(const std::vector<std::uint8_t>& data) {
    std::string text(data.begin(), data.end());
    ALEMBIC_ASSIGN(std::vector<json::Property> props, json::top_level_properties(text));
    Param p;
    for (const json::Property& prop : props) {
        if (!prop.value) continue;
        if (prop.name == "contentId") p.content_id = *prop.value;
        if (prop.name == "contentVersion") p.content_version = *prop.value;
        if (prop.name == "applicationDrmType") p.drm_type = *prop.value;
    }
    // Trimmed and upper-cased, then ^[A-Z]{2}[0-9]{4}-[A-Z0-9]{9}_[0-9]{2}-[A-Z0-9]{16}$.
    std::string& c = p.content_id;
    while (!c.empty() && std::isspace(static_cast<unsigned char>(c.back()))) c.pop_back();
    while (!c.empty() && std::isspace(static_cast<unsigned char>(c.front()))) c.erase(c.begin());
    for (char& ch : c) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    auto alpha = [](char ch) { return ch >= 'A' && ch <= 'Z'; };
    auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    bool ok = c.size() == 36 && alpha(c[0]) && alpha(c[1]) && c[6] == '-' && c[16] == '_' && c[19] == '-';
    for (int i = 2; ok && i < 6; ++i) ok = digit(c[i]);
    for (int i = 7; ok && i < 16; ++i) ok = alpha(c[i]) || digit(c[i]);
    for (int i = 17; ok && i < 19; ++i) ok = digit(c[i]);
    for (int i = 20; ok && i < 36; ++i) ok = alpha(c[i]) || digit(c[i]);
    if (!ok) return make_error(Errc::invalid_data, "param.json has no valid contentId (XX0000-XXXX00000_00-XXXXXXXXXXXXXXXX)");
    return p;
}

// FIH 0x9C: the content version's major number in BCD, in the top byte.
std::uint32_t content_version_high(const std::string& version) {
    std::string major = version.substr(0, version.find('.'));
    while (!major.empty() && major.back() == ' ') major.pop_back();
    while (!major.empty() && major.front() == ' ') major.erase(major.begin());
    if (major.empty() || major.size() > 2) return 0;
    for (char c : major)
        if (c < '0' || c > '9') return 0;
    int value = std::stoi(major);
    return static_cast<std::uint32_t>(((value / 10) << 4) | (value % 10)) << 24;
}

Result<std::vector<std::uint8_t>> read_node(const tree::Source& source, const tree::Node& node) {
    if (node.size > (64u << 20)) return make_error(Errc::too_large, "a sce_sys file is unexpectedly large: " + node.name);
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, source.open(node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node.size));
    if (!data.empty()) ALEMBIC_TRY(in->read_at(0, data));
    return data;
}

struct FihFields {
    std::uint64_t pfs_image_size = 0;
    std::uint64_t superblock_offset = 0;
    crypto::Digest game_digest{};
    crypto::Digest nested_digest{};
    std::uint64_t nested_size = 0;
    std::uint64_t meta_base_blocks = 0;
    std::uint32_t content_version_hi = 0;
    std::uint32_t inner_content_inodes = 0;
    std::uint32_t app_file_count = 0;
    std::uint32_t meta_block_count = 0;
    std::uint32_t meta_block_mirror = 0;
    std::uint32_t empty_files = 0;
};

// ProsperoFihBuilder.BuildHeaderBlockFromSuperblock, debug variant.
std::vector<std::uint8_t> fih_header(const FihFields& f) {
    std::vector<std::uint8_t> h(kHeaderRegion, 0);
    h[0] = 0x7F;
    h[1] = 'F';
    h[2] = 'I';
    h[3] = 'H';
    h[4] = 1;
    put_u16(&h[6], 3);
    put_u32(&h[8], 1);
    put_u64(&h[0x10], kHeaderRegion);
    put_u64(&h[0x18], f.pfs_image_size);
    put_u64(&h[0x28], kHeaderRegion);
    put_u64(&h[0x58], kHeaderRegion + f.pfs_image_size);
    put_u64(&h[0x60], kHeaderRegion);
    put_u64(&h[0x68], 0x800000000000ULL);
    if (f.meta_base_blocks > 0) put_u64(&h[0x50], f.meta_base_blocks);
    put_u64(&h[0x20], kHeaderRegion + f.superblock_offset);
    std::memcpy(&h[0x30], f.game_digest.data(), 32);
    std::memcpy(&h[0x70], f.game_digest.data(), 32);
    std::memcpy(&h[0xD0], f.game_digest.data(), 32);
    std::uint64_t sb_index = f.superblock_offset / kHeaderRegion, total = f.pfs_image_size / kHeaderRegion;
    if (sb_index >= 1 && f.superblock_offset % kHeaderRegion == 0 && f.pfs_image_size % kHeaderRegion == 0 && total > sb_index) {
        bool nwonly = f.inner_content_inodes > 0;
        std::uint64_t naps_blocks = f.nested_size > 0 ? div_round_up(f.nested_size, kHeaderRegion) : 1;
        auto inner_blocks = static_cast<std::uint32_t>(sb_index - naps_blocks);
        std::uint32_t meta_or_inodes = nwonly ? f.inner_content_inodes : static_cast<std::uint32_t>(total - inner_blocks);
        put_u32(&h[0x90], inner_blocks);
        put_u32(&h[0x94], f.meta_block_count > 0 ? f.meta_block_count : meta_or_inodes);
        put_u32(&h[0x98], f.meta_block_mirror > 0 ? f.meta_block_mirror : meta_or_inodes);
        put_u64(&h[0xA0], static_cast<std::uint64_t>(inner_blocks) * kHeaderRegion);
        if (f.content_version_hi) put_u32(&h[0x9C], f.content_version_hi);
        if (f.nested_size > 0) put_u64(&h[0xA8], f.nested_size);
        put_u32(&h[0xF0], nwonly && f.app_file_count > 0 ? f.app_file_count : 1);
        put_u32(&h[0xF4], 0); // sparse afid holes: the reference passes none
        put_u32(&h[0xF8], 2);
        put_u32(&h[0xFC], f.empty_files);
    }
    std::memcpy(&h[0xB0], f.nested_digest.data(), 32);
    return h;
}

void record(std::vector<std::uint8_t>& out, const char* tag, const std::vector<std::uint8_t>& payload, std::uint8_t version = 1) {
    out.push_back(static_cast<std::uint8_t>(tag[3]));
    out.push_back(static_cast<std::uint8_t>(tag[2]));
    out.push_back(static_cast<std::uint8_t>(tag[1]));
    out.push_back(static_cast<std::uint8_t>(tag[0]));
    out.push_back(version);
    out.insert(out.end(), 3, 0);
    std::uint8_t len[8];
    put_u64(len, payload.size());
    out.insert(out.end(), len, len + 8);
    out.insert(out.end(), payload.begin(), payload.end());
}

std::vector<std::uint8_t> meta300(std::uint64_t inner_image_size) {
    std::vector<std::uint8_t> r(48, 0);
    std::uint64_t data_region = inner_image_size - 0x10000;
    put_u64(&r[0x10], data_region);
    put_u64(&r[0x18], 0x3E9);
    put_u64(&r[0x20], data_region);
    put_u64(&r[0x28], 0x10000);
    return r;
}

struct Meta18Block {
    std::uint64_t co;
    std::uint32_t cs, ps, c0, c1, flag;
    bool hole;
    std::uint32_t owner_flag;
    std::uint64_t tail;
    crypto::Digest digest; // of the on-disk bytes (holes: of ps zero bytes)
};

// ProsperoNapsMeta.BuildMeta18WithMetadataEntry with the assembled inner image.
std::vector<std::uint8_t> meta18(const InnerImage& inner, std::uint64_t inner_image_size, std::uint64_t pfs_image_size,
                                 const crypto::Digest& superblock_digest, const crypto::Digest& image_digest) {
    std::vector<Meta18Block> blocks;
    for (std::size_t i = 0; i < inner.placements.size(); ++i) {
        const Placement& p = inner.placements[i];
        auto cs = static_cast<std::uint32_t>(p.on_disk_size);
        crypto::Digest d;
        std::memcpy(d.data(), p.on_disk_sha3.data(), 32);
        blocks.push_back({p.on_disk_offset, cs, static_cast<std::uint32_t>(p.uncompressed_size), cs, 0, 0x40090000u, false,
                          i == 0 ? 0u : 1u, 0, d});
    }
    if (inner.meta_base_logical > inner.data_end_logical) {
        std::uint64_t padding = inner.meta_base_logical - inner.data_end_logical;
        std::uint64_t n = div_round_up(padding, 0x40000);
        std::vector<std::pair<std::uint32_t, std::uint64_t>> hole_co;
        std::uint64_t cursor = inner.block_info_on_disk_offset;
        std::vector<std::uint8_t> zeros(0x40000, 0);
        for (std::uint64_t k = 0; k < n; ++k) {
            auto ps = static_cast<std::uint32_t>(std::min<std::uint64_t>(0x40000, padding - k * 0x40000));
            std::uint64_t co = 0;
            bool found = false;
            for (auto& [size, at] : hole_co)
                if (size == ps) {
                    co = at;
                    found = true;
                }
            if (!found) {
                co = cursor;
                hole_co.push_back({ps, co});
                cursor += 0x10;
            }
            blocks.push_back({co, 0x10, ps, 8, 8, 0x40110000u, true, 0, 0, crypto::sha3_256(Bytes(zeros.data(), ps))});
        }
    }
    std::uint32_t data_offset = inner.container.size() >= 0xB0 ? get_u32(&inner.container[0xAA]) : 0;
    std::uint64_t meta_cursor = inner.metadata_on_disk_offset + data_offset;
    for (const ContainerBlock& b : inner.metadata_blocks) {
        std::uint64_t in_container = meta_cursor - inner.metadata_on_disk_offset;
        crypto::Digest d = crypto::sha3_256(Bytes(inner.container.data() + in_container, b.compressed_size));
        blocks.push_back({meta_cursor, b.compressed_size, b.uncompressed_size, b.compressed_size, 0, 0x40050000u, false, 0,
                          0x3E9, d});
        meta_cursor += b.compressed_size;
    }
    int meta_first = -1;
    for (std::size_t i = 0; i < blocks.size(); ++i)
        if (blocks[i].tail == 0x3E9) {
            meta_first = static_cast<int>(i);
            break;
        }

    // Content files: inner files in afid order, then the metadata pseudo-file.
    std::vector<const MetaNode*> files;
    for (const MetaNode& n : inner.nodes)
        if (!n.is_directory && n.parent_inode >= 0) files.push_back(&n);
    std::stable_sort(files.begin(), files.end(), [](const MetaNode* a, const MetaNode* b) { return a->afid < b->afid; });
    std::vector<std::pair<std::string, std::uint64_t>> content;
    for (const MetaNode* n : files) content.push_back({n->full_path.substr(n->full_path[0] == '/' ? 1 : 0), n->size});
    content.push_back({"*PFSmetadata", pfs_image_size});

    std::vector<std::uint8_t> plain;
    {
        std::vector<std::uint8_t> b(0x18, 0);
        put_u32(&b[0], 1);
        put_u32(&b[4], 0x30);
        put_u32(&b[8], static_cast<std::uint32_t>(inner_image_size / 0x10000));
        put_u32(&b[0xC], static_cast<std::uint32_t>(inner_image_size));
        put_u32(&b[0x10], 1);
        put_u32(&b[0x14], 0x10000);
        record(plain, "phdr", b);
    }
    {
        std::vector<std::uint8_t> b(std::max<std::size_t>(content.size(), 1) * 0x18, 0);
        for (std::size_t i = 0; i < content.size(); ++i) {
            std::uint8_t* e = &b[i * 0x18];
            bool is_meta = content[i].first == "*PFSmetadata";
            put_u64(e, content[i].second);
            put_u32(e + 8, is_meta && meta_first >= 0 ? static_cast<std::uint32_t>(meta_first) : static_cast<std::uint32_t>(i));
            put_u32(e + 0xC, is_meta ? 3 : 1);
            put_u32(e + 0x10, is_meta ? 0x3E9 : 0);
            put_u32(e + 0x14, is_meta ? 0 : (i == 0 ? 0 : 1));
        }
        record(plain, "file", b, 2);
    }
    {
        std::vector<std::uint8_t> b(blocks.size());
        for (std::size_t i = 0; i < blocks.size(); ++i) b[i] = blocks[i].owner_flag == 1 ? 0x01 : 0x0F;
        record(plain, "ibcl", b);
    }
    {
        std::vector<std::uint8_t> b(blocks.size() * 0x28, 0);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            std::uint8_t* e = &b[i * 0x28];
            const Meta18Block& k = blocks[i];
            put_u64(e, k.co);
            put_u32(e + 8, k.cs);
            put_u32(e + 0xC, k.ps);
            put_u32(e + 0x10, k.c0);
            put_u32(e + 0x14, k.c1);
            put_u32(e + 0x18, static_cast<std::uint32_t>(k.co >> 16));
            put_u32(e + 0x20, 1);
            put_u32(e + 0x24, k.flag);
        }
        record(plain, "i2ob", b);
    }
    {
        std::vector<std::uint8_t> b(blocks.size() * 0x10, 0);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            put_u64(&b[i * 0x10], blocks[i].co);
            put_u64(&b[i * 0x10 + 8], blocks[i].co >> 16);
        }
        record(plain, "i2op", b);
    }
    {
        std::vector<std::uint8_t> b(blocks.size() * 0x30, 0);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            std::uint8_t* e = &b[i * 0x30];
            const Meta18Block& k = blocks[i];
            if (k.hole) put_u32(e + 4, k.ps);
            std::memcpy(e + 8, k.digest.data(), 32);
            if (!k.hole) put_u64(e + 0x28, k.tail);
        }
        record(plain, "ihsh", b);
    }
    {
        std::vector<std::uint8_t> b(0xB0, 0);
        std::memcpy(b.data(), superblock_digest.data(), 32);
        record(plain, "rhsh", b);
    }
    {
        std::vector<std::uint8_t> b;
        for (const auto& [path, size] : content) {
            b.insert(b.end(), path.begin(), path.end());
            b.push_back(0);
        }
        record(plain, "fstr", b);
    }
    {
        std::vector<std::uint8_t> b(0x14, 0);
        put_u32(&b[4], 4);
        record(plain, "twek", b);
    }
    {
        std::vector<std::uint8_t> b(0x80, 0);
        std::memcpy(b.data(), image_digest.data(), 32);
        record(plain, "obdg", b);
    }
    std::vector<std::uint8_t> desc = meta300(inner_image_size);
    record(plain, "pgpl", desc);
    record(plain, "pgil", desc);
    record(plain, "pgpi", desc);
    record(plain, "pgpu", desc);
    record(plain, "zero", std::vector<std::uint8_t>((16 - plain.size() % 16) % 16, 0));

    // AES-128-XTS with fixed keys and initial tweak 0x7D10BA3C.
    static const std::uint8_t kData[16] = {0x02, 0x2D, 0xCA, 0xF6, 0xD1, 0x11, 0xE5, 0x8F,
                                           0x25, 0x93, 0x6E, 0xF5, 0x46, 0x93, 0x45, 0xAB};
    static const std::uint8_t kTweak[16] = {0xAD, 0xAC, 0x16, 0x37, 0x60, 0xDA, 0x51, 0x46,
                                            0x98, 0xC2, 0x45, 0xAB, 0x4C, 0x9C, 0x42, 0x6C};
    crypto::Aes128 data_key(kData), tweak_key(kTweak);
    crypto::xts_encrypt(data_key, tweak_key, 0x7D10BA3C, plain);
    return plain;
}

// ProsperoSiArchive.WriteZip: stored members, no extra fields, fixed DOS date.
std::vector<std::uint8_t> write_zip(const std::vector<std::pair<std::string, std::vector<std::uint8_t>>>& members) {
    std::vector<std::uint8_t> out;
    auto u16 = [&](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    auto u32 = [&](std::uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    };
    std::vector<std::pair<std::uint32_t, std::uint32_t>> local; // crc, offset
    for (const auto& [name, data] : members) {
        auto crc = static_cast<std::uint32_t>(::crc32(0, data.data(), static_cast<uInt>(data.size())));
        local.push_back({crc, static_cast<std::uint32_t>(out.size())});
        u32(0x04034b50);
        u16(20);
        u16(0);
        u16(0);
        u16(0);
        u16(0x0021);
        u32(crc);
        u32(static_cast<std::uint32_t>(data.size()));
        u32(static_cast<std::uint32_t>(data.size()));
        u16(static_cast<std::uint16_t>(name.size()));
        u16(0);
        out.insert(out.end(), name.begin(), name.end());
        out.insert(out.end(), data.begin(), data.end());
    }
    auto cd_start = static_cast<std::uint32_t>(out.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& [name, data] = members[i];
        u32(0x02014b50);
        u16(0);
        u16(20);
        u16(0);
        u16(0);
        u16(0);
        u16(0x0021);
        u32(local[i].first);
        u32(static_cast<std::uint32_t>(data.size()));
        u32(static_cast<std::uint32_t>(data.size()));
        u16(static_cast<std::uint16_t>(name.size()));
        u16(0);
        u16(0);
        u16(0);
        u16(0);
        u32(0);
        u32(local[i].second);
        out.insert(out.end(), name.begin(), name.end());
    }
    auto cd_size = static_cast<std::uint32_t>(out.size()) - cd_start;
    u32(0x06054b50);
    u16(0);
    u16(0);
    u16(static_cast<std::uint16_t>(members.size()));
    u16(static_cast<std::uint16_t>(members.size()));
    u32(cd_size);
    u32(cd_start);
    u16(0);
    return out;
}

} // namespace

namespace {

// The checkpoint side file (.state): what the inner image had written and
// the open outer block. Little-endian fields after the magic "ALFS".
std::vector<std::uint8_t> encode_checkpoint(const InnerProgress& p, Bytes pending) {
    std::vector<std::uint8_t> out(4 + 8 * 5);
    std::memcpy(out.data(), "ALFS", 4);
    put_u64(&out[4], p.written);
    put_u64(&out[12], p.file);
    put_u64(&out[20], p.file_offset);
    put_u64(&out[28], p.source_block);
    put_u64(&out[36], pending.size());
    for (const crypto::Digest& d : p.finished) out.insert(out.end(), d.begin(), d.end());
    out.insert(out.end(), p.hash.begin(), p.hash.end());
    out.insert(out.end(), pending.begin(), pending.end());
    return out;
}

Status decode_checkpoint(const std::vector<std::uint8_t>& in, InnerProgress& p, std::vector<std::uint8_t>& pending) {
    auto bad = [] { return make_error(Errc::invalid_data, "the package checkpoint side file is damaged"); };
    if (in.size() < 44 || std::memcmp(in.data(), "ALFS", 4) != 0) return bad();
    p.written = get_u64(&in[4]);
    std::uint64_t files = get_u64(&in[12]);
    p.file = static_cast<std::size_t>(files);
    p.file_offset = get_u64(&in[20]);
    p.source_block = static_cast<std::uint32_t>(get_u64(&in[28]));
    std::uint64_t pending_size = get_u64(&in[36]);
    if (files > (1u << 24) || pending_size >= kInnerBlock ||
        in.size() != 44 + files * 32 + crypto::Sha3_256::kStateBytes + pending_size)
        return bad();
    const std::uint8_t* q = in.data() + 44;
    p.finished.resize(p.file);
    for (crypto::Digest& d : p.finished) {
        std::memcpy(d.data(), q, 32);
        q += 32;
    }
    std::memcpy(p.hash.data(), q, crypto::Sha3_256::kStateBytes);
    q += crypto::Sha3_256::kStateBytes;
    pending.assign(q, q + pending_size);
    return {};
}

Result<std::vector<std::uint8_t>> read_whole(const std::string& path, std::uint64_t length) {
    ALEMBIC_ASSIGN(File f, File::open(path, File::Mode::read));
    ALEMBIC_ASSIGN(std::uint64_t size, f.size());
    if (size < length) return make_error(Errc::invalid_data, "a package checkpoint side file is short: " + path);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    if (!data.empty()) ALEMBIC_TRY(f.read_at(0, data));
    return data;
}

} // namespace

Result<PackageResult> write_package(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                                    const PackageOptions& options, const ProgressFn& progress, resume::Session* session) {
    PackageResult result;
    if (options.passcode.size() != 32) return make_error(Errc::invalid_argument, "the passcode must be 32 characters");
    if (options.compression != Compression::stored)
        return make_error(Errc::unsupported, "Kraken-compressed packages are not supported yet (use stored)");
    ALEMBIC_ASSIGN(PackageKeys keys, load_package_keys(options.keys_folder));

    // ---- Inputs, as img_create collects them. ----
    std::vector<InnerFile> files;
    std::vector<std::pair<std::string, const tree::Node*>> sce_sys; // path relative to sce_sys
    std::function<Status(const tree::Node&, const std::string&)> walk = [&](const tree::Node& n, const std::string& prefix) -> Status {
        for (const tree::Node& c : n.children) {
            std::string rel = prefix + c.name;
            if (c.is_directory) {
                ALEMBIC_TRY(walk(c, rel + "/"));
                continue;
            }
            if (rel.rfind("sce_sys/", 0) == 0) sce_sys.push_back({rel.substr(8), &c});
            std::string l = lower(c.name);
            if (ends_with(l, ".gp4") || ends_with(l, ".gp5") || ends_with(l, ".pkg")) continue;
            if (rel.rfind("sce_sys/", 0) == 0 && is_outer_cnt_file(rel.substr(8))) continue;
            InnerFile f;
            f.path = "/" + rel;
            f.source = &source;
            f.node = &c;
            files.push_back(std::move(f));
        }
        return {};
    };
    ALEMBIC_TRY(walk(root, ""));
    auto sce_sys_file = [&](const std::string& name) -> const tree::Node* { // case-insensitive, as the reference
        for (const auto& [rel, node] : sce_sys)
            if (lower(rel) == lower(name)) return node;
        return nullptr;
    };
    const tree::Node* param_node = sce_sys_file("param.json");
    if (!param_node) return make_error(Errc::invalid_argument, "the folder has no sce_sys/param.json");
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> param_json, read_node(source, *param_node));
    ALEMBIC_ASSIGN(Param param, read_param(param_json));
    result.content_id = param.content_id;

    auto has_file = [&](const char* path) {
        return std::any_of(files.begin(), files.end(), [&](const InnerFile& f) { return f.path == path; });
    };
    // Modules the console maps must be SELFs: ProsperoPkgTool fake-signs raw ELFs; not ported yet.
    for (const InnerFile& f : files) {
        bool module = f.path == "/eboot.bin" || f.path.rfind("/sce_module/", 0) == 0 || f.path.rfind("/sce_sys/about/", 0) == 0;
        if (!module || f.length() < 4) continue;
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, source.open(*f.node));
        std::uint8_t magic[4];
        ALEMBIC_TRY(in->read_at(0, MutableBytes(magic, 4)));
        if (get_u32(magic) == 0x464C457F)
            return make_error(Errc::unsupported, f.path.substr(1) + " is a raw ELF; packages need signed (SELF) modules, and fake-signing is not supported yet");
    }
    if (!has_file("/sce_sys/pfs-version.dat")) {
        InnerFile f;
        f.path = "/sce_sys/pfs-version.dat";
        std::string v = param.content_version.empty() ? "01.000.000" : param.content_version;
        f.data.assign(v.begin(), v.end());
        files.push_back(std::move(f));
    }
    if (!has_file("/sce_sys/keystone"))
        return make_error(Errc::unsupported, "the game has no sce_sys/keystone (generating one is not supported)");
    std::uint32_t empty_files = 0;
    for (const InnerFile& f : files)
        if (f.length() == 0) ++empty_files;
    result.inner_files = files.size();

    // Media entries (ProsperoSceSysMedia.Collect), checked before the long write.
    std::vector<CntEntry> media;
    {
        std::vector<std::uint32_t> emitted;
        auto add = [&](std::uint32_t id, const std::string& name, std::vector<std::uint8_t> data) {
            CntProfile prof = cnt_entry_profile(id, name, param.drm_type);
            CntEntry e;
            e.id = id;
            e.name = prof.include_name ? name : std::string();
            e.data = std::move(data);
            e.flags1 = prof.flags1;
            e.flags2 = prof.flags2;
            media.push_back(std::move(e));
            emitted.push_back(id);
        };
        auto was = [&](std::uint32_t id) { return std::find(emitted.begin(), emitted.end(), id) != emitted.end(); };
        const std::pair<const char*, std::uint32_t> kMedia[] = {{"icon0.png", 0x1200}, {"pic0.png", 0x1220}, {"pic1.png", 0x1006},
                                                                {"pic2.png", 0x2040},  {"snd0.at9", 0x1240}, {"save_data.png", 0x100D}};
        for (const auto& [name, id] : kMedia)
            if (const tree::Node* n = sce_sys_file(name); n && !was(id)) {
                ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, read_node(source, *n));
                add(id, name, std::move(data));
            }
        const std::tuple<const char*, const char*, std::uint32_t> kDds[] = {{"icon0.png", "icon0.dds", 0x1280},
                                                                            {"pic0.png", "pic0.dds", 0x12A0},
                                                                            {"pic1.png", "pic1.dds", 0x12C0},
                                                                            {"pic2.png", "pic2.dds", 0x2060}};
        for (const auto& [png, dds, id] : kDds) {
            if (was(id)) continue;
            if (const tree::Node* n = sce_sys_file(dds)) {
                ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, read_node(source, *n));
                add(id, dds, std::move(data));
            } else if (sce_sys_file(png)) {
                return make_error(Errc::unsupported, std::string("sce_sys/") + png + " has no " + dds +
                                                         " next to it; generating DDS pictures is not supported yet");
            }
        }
        std::vector<std::pair<std::string, const tree::Node*>> rest = sce_sys;
        std::stable_sort(rest.begin(), rest.end(), [](const auto& a, const auto& b) { return lower(a.first) < lower(b.first); });
        for (const auto& [name, node] : rest) {
            if (ends_with(name, ".dds") || name == "param.sfo") continue;
            std::optional<std::uint32_t> id = sce_sys_entry_id(name);
            if (!id || was(*id)) continue;
            ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, read_node(source, *node));
            add(*id, name, std::move(data));
        }
    }
    PlayGoProject project = default_playgo_project();
    if (const tree::Node* n = sce_sys_file("playgo-chunk.dat")) {
        ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, read_node(source, *n));
        if (auto parsed = parse_playgo_chunk(data); parsed.ok()) project = std::move(*parsed);
        else result.warnings.push_back("sce_sys/playgo-chunk.dat could not be parsed (" + parsed.error().message + "); using the default PlayGo project");
    }

    // ---- A checkpoint to continue from. ----
    // State: format, written (inner bytes), blocks (outer blocks written),
    // seed (two halves), state_size and state_crc of the .state side file.
    std::optional<InnerProgress> resume_point;
    std::vector<crypto::Digest> resume_digests;
    std::vector<std::uint8_t> resume_pending;
    std::optional<std::array<std::uint8_t, 16>> resume_seed;
    if (session && session->from) {
        const resume::State& st = *session->from;
        auto invalid = [](const std::string& why) { return make_error(Errc::invalid_data, "cannot continue from the checkpoint: " + why); };
        if (resume::get(st, "format") != resume::kFpkgFormat) return invalid("it is not a package checkpoint");
        std::uint64_t blocks = resume::get(st, "blocks"), state_size = resume::get(st, "state_size");
        if (blocks > 0x7FFFFFFF || state_size > (1u << 30)) return invalid("bad values");
        auto blob = read_whole(resume::sidecar_path(*session, ".state"), state_size);
        if (!blob.ok()) return invalid(blob.error().message);
        if (crypto::crc32c(*blob) != resume::get(st, "state_crc")) return invalid("its side file does not match");
        InnerProgress p;
        if (Status s = decode_checkpoint(*blob, p, resume_pending); !s.ok()) return invalid(s.error().message);
        if (p.written != blocks * kInnerBlock + resume_pending.size() || p.written != resume::get(st, "written"))
            return invalid("inconsistent positions");
        auto digests = read_whole(resume::sidecar_path(*session, ".digests"), blocks * 32);
        if (!digests.ok()) return invalid(digests.error().message);
        resume_digests.resize(static_cast<std::size_t>(blocks));
        for (std::size_t i = 0; i < resume_digests.size(); ++i) std::memcpy(resume_digests[i].data(), digests->data() + i * 32, 32);
        std::array<std::uint8_t, 16> seed{};
        put_u64(seed.data(), resume::get(st, "seed_lo"));
        put_u64(seed.data() + 8, resume::get(st, "seed_hi"));
        resume_seed = seed;
        resume_point = std::move(p);
    }

    // ---- The outer PFS, with the inner image streamed through it. ----
    OuterOptions outer_options;
    outer_options.content_id = param.content_id;
    outer_options.passcode = options.passcode;
    if (resume_seed) outer_options.seed = *resume_seed;
    else if (options.seed) outer_options.seed = *options.seed;
    else {
        std::random_device rd;
        for (auto& b : outer_options.seed) b = static_cast<std::uint8_t>(rd());
    }
    outer_options.time_seconds = options.time_seconds;
    outer_options.time_nanoseconds = options.time_nanoseconds;
    outer_options.threads = options.threads;
    ALEMBIC_ASSIGN(AtomicOutput output, session ? AtomicOutput::create_partial(output_path, session->partial_path,
                                                                                session->from.has_value(), options.overwrite)
                                                : AtomicOutput::create(output_path, options.overwrite));
    File& file = output.file();
    const std::uint64_t outer_start = kHeaderRegion + resume_digests.size() * static_cast<std::uint64_t>(kInnerBlock);
    if (resume_point) ALEMBIC_TRY(file.truncate(outer_start)); // drop what came after the checkpoint
    FileSink outer_sink(file, outer_start);
    OuterPfsWriter outer(outer_sink, outer_options);
    ALEMBIC_TRY(outer.begin_file("pfs_image.dat", false));
    if (resume_point) ALEMBIC_TRY(outer.resume(std::move(resume_digests), resume_pending, resume_point->written));
    InnerOptions inner_options;
    inner_options.time_seconds = options.time_seconds;
    inner_options.time_nanoseconds = options.time_nanoseconds;
    inner_options.threads = options.threads;
    if (resume_point) inner_options.resume_from = &*resume_point;
    std::uint64_t digests_saved = resume_point ? resume_point->written / kInnerBlock : 0;
    if (session && session->save) {
        inner_options.checkpoint_interval = session->interval;
        inner_options.checkpoint = [&](const InnerProgress& p) -> Status {
            // Everything up to here on disk first, then the side files, then the State.
            ALEMBIC_TRY(outer.flush());
            ALEMBIC_TRY(file.sync());
            const std::vector<crypto::Digest>& digests = outer.digests();
            {
                const std::string path = resume::sidecar_path(*session, ".digests");
                auto f = digests_saved ? File::open(path, File::Mode::read_write) : File::open(path, File::Mode::create_new);
                if (!f.ok() && !digests_saved) {
                    ::unlink(path.c_str());
                    f = File::open(path, File::Mode::create_new);
                }
                if (!f.ok()) return f.error();
                std::vector<std::uint8_t> add;
                add.reserve((digests.size() - digests_saved) * 32);
                for (std::size_t i = digests_saved; i < digests.size(); ++i) add.insert(add.end(), digests[i].begin(), digests[i].end());
                ALEMBIC_TRY(f->write_at(digests_saved * 32, add));
                ALEMBIC_TRY(f->sync());
                digests_saved = digests.size();
            }
            std::vector<std::uint8_t> blob = encode_checkpoint(p, outer.pending());
            {
                ALEMBIC_ASSIGN(AtomicOutput side, AtomicOutput::create(resume::sidecar_path(*session, ".state"), true));
                ALEMBIC_TRY(side.file().write_at(0, blob));
                ALEMBIC_TRY(side.commit());
            }
            resume::State st;
            st["format"] = resume::kFpkgFormat;
            st["written"] = p.written;
            st["blocks"] = digests.size();
            st["seed_lo"] = get_u64(outer_options.seed.data());
            st["seed_hi"] = get_u64(outer_options.seed.data() + 8);
            st["state_size"] = blob.size();
            st["state_crc"] = crypto::crc32c(blob);
            return session->save(st);
        };
    }
    ALEMBIC_ASSIGN(InnerImage inner, write_inner_image(files, inner_options, outer, progress));
    ALEMBIC_TRY(outer.end_file(inner.ndblock * kInnerBlock));
    ALEMBIC_ASSIGN(Naps naps, build_naps(inner, Compression::stored));
    ALEMBIC_TRY(outer.begin_file("naps_pkg_layout.dat", true));
    ALEMBIC_TRY(outer.write(naps.blob));
    ALEMBIC_TRY(outer.end_file(naps.blob.size()));
    ALEMBIC_ASSIGN(OuterPfs pfs, outer.finish());
    const std::uint64_t outer_length = static_cast<std::uint64_t>(pfs.block_count) * kInnerBlock;
    const crypto::Digest game_digest = crypto::sha3_256(pfs.superblock_plaintext);

    // ---- The FIH header (its SHA3 goes into the CNT). ----
    FihFields fih;
    fih.pfs_image_size = outer_length;
    fih.superblock_offset = static_cast<std::uint64_t>(pfs.superblock_index) * kInnerBlock;
    fih.game_digest = game_digest;
    fih.nested_digest = crypto::sha3_256(naps.blob);
    fih.nested_size = naps.blob.size();
    fih.meta_base_blocks = inner.meta_base_logical / kInnerBlock;
    fih.content_version_hi = content_version_high(param.content_version);
    fih.inner_content_inodes = inner.inner_content_inodes;
    fih.app_file_count = inner.app_file_count;
    fih.meta_block_count = static_cast<std::uint32_t>(naps.file_count - 1);
    fih.meta_block_mirror = fih.meta_block_count + empty_files;
    fih.empty_files = empty_files;
    std::vector<std::uint8_t> header = fih_header(fih);

    // ---- PlayGo and the CNT. ----
    PlayGoFiles playgo = build_playgo(param.content_id, project, inner, kHeaderRegion, kHeaderRegion + outer_length);
    if (playgo.fell_back) result.warnings.push_back("the PlayGo project did not fit (" + playgo.warning + "); using the default one");
    std::vector<CntEntry> entries;
    auto add = [&](std::uint32_t id, std::string name, std::vector<std::uint8_t> data, std::uint32_t flags1) {
        CntEntry e;
        e.id = id;
        e.name = std::move(name);
        e.data = std::move(data);
        e.flags1 = flags1;
        entries.push_back(std::move(e));
    };
    crypto::Digest ekpfs = compute_keys(param.content_id, options.passcode, 1);
    add(0x0010, "", build_entry_keys(keys, param.content_id, options.passcode), 0x60000000);
    add(0x0020, "", build_image_key(keys, ekpfs), 0x60000000);
    add(0x0080, "", std::vector<std::uint8_t>(0x1E0, 0), 0x60000000);
    add(0x0100, "", {}, 0x60000000);
    add(0x0001, "", {}, 0x40000000);
    add(0x0200, "", std::vector<std::uint8_t>(1, 0), 0x40000000);
    add(0x2000, "param.json", param_json, 0);
    {
        std::vector<std::uint8_t> digs;
        digs.reserve(pfs.block_digests.size() * 32);
        for (const crypto::Digest& d : pfs.block_digests) digs.insert(digs.end(), d.rbegin(), d.rend()); // byte-reversed
        add(0x040A, "", std::move(digs), 0x08000000);
    }
    add(0x1001, "playgo-chunk.dat", playgo.chunk, 0x08000000);
    for (CntEntry& m : media)
        if (std::none_of(entries.begin(), entries.end(), [&](const CntEntry& e) { return e.id == m.id; })) entries.push_back(std::move(m));
    add(0x2010, "playgo-hash-table.dat", playgo.hash_table, 0x08000000);
    add(0x2011, "playgo-ficm.dat", playgo.ficm, 0x08000000);
    CntParameters cnt_parameters;
    cnt_parameters.content_id = param.content_id;
    cnt_parameters.passcode = options.passcode;
    cnt_parameters.pfs_image_size = outer_length;
    cnt_parameters.image_seed = outer_options.seed;
    cnt_parameters.pfs_image_digest = game_digest;
    cnt_parameters.pfs_signed_digest = crypto::sha3_256(header);
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> cnt, build_cnt(entries, cnt_parameters, keys));
    ALEMBIC_TRY(file.write_at(kHeaderRegion + outer_length, cnt));
    ALEMBIC_TRY(file.write_at(0, header));

    // ---- Read back [0, SI) for the whole-image digest and the per-block CRC32Cs. ----
    const std::uint64_t mount_length = kHeaderRegion + outer_length + cnt.size();
    crypto::Sha3_256 image_hash;
    std::vector<std::uint8_t> crc(static_cast<std::size_t>(div_round_up(mount_length, kInnerBlock) * 4));
    {
        // The SHA3 (the slow part) runs on a helper thread while the next block is read.
        crypto::BackgroundSha3 hasher(worker_count(options.threads) > 1, kInnerBlock);
        std::vector<std::uint8_t> buffer(kInnerBlock);
        for (std::uint64_t off = 0, i = 0; off < mount_length; off += kInnerBlock, ++i) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kInnerBlock, mount_length - off));
            ALEMBIC_TRY(file.read_at(off, MutableBytes(buffer.data(), n)));
            hasher.update(image_hash, Bytes(buffer.data(), n));
            put_u32(&crc[i * 4], crypto::crc32c(Bytes(buffer.data(), n)));
            if (progress && !progress({"Checking the package", off + n, mount_length})) return make_error(Errc::cancelled, "cancelled");
        }
        hasher.wait();
    }
    crypto::Digest image_digest = image_hash.finish();

    // ---- The SI segment. ----
    const std::uint64_t inner_image_size = get_u64(&header[0xA0]);
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> members;
    if (inner_image_size >= 0x10000 && mount_length >= 0x10000)
        members.push_back({"common/etc/naps_meta_18.dat", meta18(inner, inner_image_size, outer_length, game_digest, image_digest)});
    if (inner_image_size >= 0x10000)
        for (int id : {300, 301, 302, 308}) members.push_back({"common/etc/naps_meta_" + std::to_string(id) + ".dat", meta300(inner_image_size)});
    members.push_back({"common/etc/playgo-chunk.dat", playgo.chunk});
    members.push_back({"config/" + param.content_id + "/playgo-chunk.crc", crc});
    std::vector<std::uint8_t> si = write_zip(members);
    ALEMBIC_TRY(file.write_at(mount_length, si));
    result.package_length = mount_length + si.size();
    ALEMBIC_TRY(output.commit());
    if (session) for (const char* suffix : resume::kSidecarSuffixes) ::unlink(resume::sidecar_path(*session, suffix).c_str());
    return result;
}

} // namespace alembic::fpkg::build
