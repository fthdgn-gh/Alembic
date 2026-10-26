// SPDX-License-Identifier: GPL-3.0-or-later
// A PS4 fake package from a game tree, byte for byte as maxton/LibOrbisPkg
// (LGPL-3.0, `6434772`) pkg_build makes it from the project pkg_makegp4
// extracts: ported from PKG/PkgBuilder.cs, PKG/PkgWriter.cs, PKG/Pkg.cs,
// PKG/Entry.cs, Rif/LicenseDat.cs, Rif/LicenseInfo.cs, PlayGo/ChunkDat.cs,
// PlayGo/Manifest.cs and GP4/Gp4Creator.cs (what an extracted project keeps).
// Differences: a licence the game already has is kept instead of generated,
// and the outer PFS is streamed (fpkg_ps4_pfs.cpp).
#include "alembic/fpkg_ps4.hpp"

#include "fpkg_ps4_internal.hpp"

#include "alembic/bytes.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/rsa.hpp"
#include "alembic/sfo.hpp"
#include "alembic/sha256.hpp"
#include "alembic/thread_pool.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <ctime>
#include <mutex>

namespace alembic::fpkg::ps4 {

namespace {

using detail::FsTree;
using detail::kBlock;

constexpr std::uint64_t kBodyOffset = 0x2000;
constexpr std::uint32_t kContentTypeGd = 0x1A;
constexpr std::uint32_t kDrmTypePs4 = 0xF;

std::uint64_t align(std::uint64_t value, std::uint64_t to) { return value % to ? value + to - value % to : value; }

void put_be16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}
void put_be32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
void put_be64(std::uint8_t* p, std::uint64_t v) {
    put_be32(p, static_cast<std::uint32_t>(v >> 32));
    put_be32(p + 4, static_cast<std::uint32_t>(v));
}

// Header fields that LibOrbisPkg's PkgWriter.WriteHeader writes (big-endian).
struct Header {
    std::uint32_t entry_count = 0;
    std::uint16_t sc_entry_count = 6;
    std::uint32_t main_ent_data_size = 0;
    std::uint64_t body_size = 0;
    std::string content_id;
    std::uint32_t promote_size = 0;
    crypto::Digest sc_entries1{}, sc_entries2{}, digest_table{}, body_digest{};
    std::uint64_t pfs_offset = 0, pfs_size = 0, package_size = 0, mount_image_size = 0;
    crypto::Digest pfs_image_digest{}, pfs_signed_digest{};

    void write(std::uint8_t* h) const {
        std::memcpy(h, "\x7F" "CNT", 4);
        put_be32(h + 0x04, 1); // PKGFlags.Unknown
        put_be32(h + 0x0C, 0xF);
        put_be32(h + 0x10, entry_count);
        put_be16(h + 0x14, sc_entry_count);
        put_be16(h + 0x16, static_cast<std::uint16_t>(entry_count));
        put_be32(h + 0x18, 0x2A80);
        put_be32(h + 0x1C, main_ent_data_size);
        put_be64(h + 0x20, kBodyOffset);
        put_be64(h + 0x28, body_size);
        std::memcpy(h + 0x40, content_id.data(), std::min<std::size_t>(content_id.size(), 36));
        put_be32(h + 0x70, kDrmTypePs4);
        put_be32(h + 0x74, kContentTypeGd);
        put_be32(h + 0x78, 0x0A000000); // Unk_x8000000 | GD_AC
        put_be32(h + 0x7C, promote_size);
        put_be32(h + 0x80, 0x20161020);
        put_be32(h + 0x84, 0x1738551);
        put_be32(h + 0x9C, 1); // ekc_version
        std::memcpy(h + 0x100, sc_entries1.data(), 32);
        std::memcpy(h + 0x120, sc_entries2.data(), 32);
        std::memcpy(h + 0x140, digest_table.data(), 32);
        std::memcpy(h + 0x160, body_digest.data(), 32);
        put_be32(h + 0x400, 1);
        put_be32(h + 0x404, 1); // pfs_image_count
        put_be64(h + 0x408, 0x80000000000003CCull);
        put_be64(h + 0x410, pfs_offset);
        put_be64(h + 0x418, pfs_size);
        put_be64(h + 0x428, mount_image_size);
        put_be64(h + 0x430, package_size);
        put_be32(h + 0x438, 0x10000); // pfs_signed_size
        put_be32(h + 0x43C, 0xD0000); // pfs_cache_size
        std::memcpy(h + 0x440, pfs_image_digest.data(), 32);
        std::memcpy(h + 0x460, pfs_signed_digest.data(), 32);
    }
};

// One CNT entry: its id, name (none for most generated ones) and bytes.
struct Entry {
    std::uint32_t id = 0;
    bool named = false;
    std::string name;
    std::vector<std::uint8_t> data;
    std::uint32_t flags1 = 0, flags2 = 0;
    std::uint32_t name_offset = 0, offset = 0, size = 0;
    bool encrypted() const { return (flags1 & 0x80000000u) != 0; }
    std::uint32_t key_index() const { return (flags2 >> 12) & 0xF; }
    void meta(std::uint8_t* row) const {
        std::memset(row, 0, 32);
        put_be32(row, id);
        put_be32(row + 4, name_offset);
        put_be32(row + 8, flags1);
        put_be32(row + 12, flags2);
        put_be32(row + 16, offset);
        put_be32(row + 20, size);
    }
};

// Entry keys (KeysEntry): the content ID's digest, then each passcode key's
// digest and RSA wrap; key 0 wraps the passcode itself.
std::vector<std::uint8_t> entry_keys(const Keys& keys, const std::string& content_id, const std::string& passcode) {
    std::uint8_t cid[48] = {};
    std::memcpy(cid, content_id.data(), std::min<std::size_t>(content_id.size(), 48));
    crypto::Digest seed = crypto::sha256(Bytes(cid, 48));
    std::vector<std::uint8_t> out(seed.begin(), seed.end());
    std::vector<std::vector<std::uint8_t>> wraps;
    for (std::uint32_t i = 0; i < 7; ++i) {
        crypto::Digest key = compute_keys_ps4(content_id, passcode, i);
        crypto::Digest d = crypto::sha256(Bytes(key.data(), 32));
        for (int j = 0; j < 32; ++j) d[j] ^= key[j];
        out.insert(out.end(), d.begin(), d.end());
        wraps.push_back(i == 0 ? rsa::mt_wrap(keys.package_public[0], Bytes(reinterpret_cast<const std::uint8_t*>(passcode.data()), passcode.size()))
                               : rsa::mt_wrap(keys.package_public[i], Bytes(key.data(), 32)));
    }
    for (const auto& w : wraps) out.insert(out.end(), w.begin(), w.end());
    return out;
}

// A debug licence for a game application (LicenseDat with SkuFlag 3).
std::vector<std::uint8_t> license_dat(const Keys& keys, const std::string& content_id) {
    std::vector<std::uint8_t> d(0x400, 0);
    put_be32(&d[0x00], 0x52494600);
    put_be16(&d[0x04], 1);
    put_be16(&d[0x06], 0xFFFF);
    put_be64(&d[0x10], 1364222275ull);
    put_be64(&d[0x18], 0x7FFFFFFFFFFFFFFFull);
    std::memcpy(&d[0x20], content_id.data(), std::min<std::size_t>(content_id.size(), 36));
    put_be16(&d[0x50], 0x200); // Debug_0
    put_be16(&d[0x52], static_cast<std::uint16_t>(kDrmTypePs4));
    put_be16(&d[0x54], static_cast<std::uint16_t>(kContentTypeGd));
    put_be16(&d[0x56], 3); // SkuFlag
    put_be32(&d[0x64], 1);
    std::uint8_t cid[48] = {};
    std::memcpy(cid, content_id.data(), std::min<std::size_t>(content_id.size(), 36));
    crypto::Digest h = crypto::sha256(Bytes(cid, 48));
    std::uint8_t* iv = &d[0x260];
    std::uint8_t* secret = &d[0x270]; // 144 bytes
    std::memcpy(iv, h.data(), 16);
    std::memcpy(secret, h.data() + 16, 16);
    crypto::Aes128 key(keys.rif_debug_key.data());
    crypto::cbc_encrypt(key, iv, MutableBytes(secret, 144));
    crypto::Digest signed_part = crypto::sha256(Bytes(d.data(), 0x300));
    std::vector<std::uint8_t> sig = rsa::sign_sha256(keys.rif_modulus, keys.rif_private_exponent, signed_part);
    std::memcpy(&d[0x300], sig.data(), 256);
    return d;
}

std::vector<std::uint8_t> license_info(const std::string& content_id) {
    std::vector<std::uint8_t> d(0x200, 0);
    std::memcpy(&d[0], content_id.data(), std::min<std::size_t>(content_id.size(), 36));
    put_be32(&d[0x44], kContentTypeGd);
    put_be32(&d[0x4C], 1);
    return d;
}

// playgo-chunk.dat for one chunk and one scenario (ChunkDat.FromProject).
std::vector<std::uint8_t> chunk_dat(const std::string& content_id, std::uint64_t package_size, std::uint64_t inner_size) {
    std::vector<std::uint8_t> d(416, 0);
    put_u32(&d[0x00], 0x6f676c70);
    put_u16(&d[0x08], 1);
    put_u16(&d[0x0A], 1);
    put_u16(&d[0x0C], 1);
    put_u16(&d[0x0E], 1);
    put_u32(&d[0x10], 416);
    put_u16(&d[0x16], 1); // attrib
    std::memset(&d[0x20], 0xFF, 32);
    std::memcpy(&d[0x40], content_id.data(), std::min<std::size_t>(content_id.size(), 36));
    const std::uint32_t table[16] = {256, 32, 288, 2, 304, 9, 320, 16, 352, 32, 384, 2, 400, 12, 336, 16};
    for (int i = 0; i < 16; ++i) put_u32(&d[static_cast<std::size_t>(0xC0 + 4 * i)], table[i]);
    d[0x100] = 0x80;               // chunk flag
    d[0x102] = 3;                  // req_locus
    put_u16(&d[0x10E], 1);         // mchunk_count
    put_u64(&d[0x110], ~0ull);     // language_mask
    std::memcpy(&d[0x130], "Chunk #0", 8);
    put_u64(&d[0x148], package_size);
    put_u64(&d[0x158], inner_size);
    d[0x160] = 1;                  // scenario type
    put_u16(&d[0x174], 1);         // initial_chunk_count
    put_u16(&d[0x176], 1);         // chunk_count
    std::memcpy(&d[0x190], "Scenario #0", 11);
    return d;
}

const char kManifest[] =
    "\xEF\xBB\xBF<?xml version=\"1.0\" encoding=\"utf-8\" standalone=\"yes\"?>\r\n"
    "<psproject fmt=\"playgo-manifest\" version=\"0990\">\r\n"
    "  <volume>\r\n"
    "    <chunk_info chunk_count=\"1\" scenario_count=\"1\">\r\n"
    "      <scenarios default_id=\"0\">\r\n"
    "        <scenario id=\"0\" type=\"sp\" initial_chunk_count=\"1\" label=\"Scenario #0\">0</scenario>\r\n"
    "      </scenarios>\r\n"
    "    </chunk_info>\r\n"
    "  </volume>\r\n"
    "</psproject>\r\n";

Result<std::vector<std::uint8_t>> read_node(const tree::Source& source, const tree::Node& node, std::size_t max) {
    if (node.size > max) return make_error(Errc::too_large, node.name + " is too large");
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, source.open(node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node.size));
    if (!data.empty()) ALEMBIC_TRY(in->read_at(0, data));
    return data;
}

// The header bytes LibOrbisPkg folds into its header digest: [0, 64) and [0x400, 0x480).
crypto::Digest header_digest(const Header& header) {
    std::vector<std::uint8_t> h(0x1000, 0);
    header.write(h.data());
    std::vector<std::uint8_t> part(h.begin(), h.begin() + 64);
    part.insert(part.end(), h.begin() + 0x400, h.begin() + 0x480);
    return crypto::sha256(Bytes(part.data(), part.size()));
}

class MemoryFile final : public RandomAccess {
public:
    explicit MemoryFile(std::vector<std::uint8_t> data) : data_(std::move(data)) {}
    Result<std::uint64_t> size() const override { return data_.size(); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > data_.size() || out.size() > data_.size() - offset)
            return make_error(Errc::invalid_argument, "read past the end");
        std::memcpy(out.data(), data_.data() + offset, out.size());
        return {};
    }

private:
    std::vector<std::uint8_t> data_;
};

// pfs_image.dat: the PFSC header, then the inner image.
class PfscFile final : public RandomAccess {
public:
    PfscFile(std::vector<std::uint8_t> header, std::shared_ptr<const RandomAccess> image, std::uint64_t image_size)
        : header_(std::move(header)), image_(std::move(image)), image_size_(image_size) {}
    Result<std::uint64_t> size() const override { return header_.size() + image_size_; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        std::size_t done = 0;
        if (offset < header_.size()) {
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), header_.size() - offset));
            std::memcpy(out.data(), header_.data() + offset, n);
            done = n;
        }
        if (done < out.size()) ALEMBIC_TRY(image_->read_at(offset + done - header_.size(), out.subspan(done)));
        return {};
    }

private:
    std::vector<std::uint8_t> header_;
    std::shared_ptr<const RandomAccess> image_;
    std::uint64_t image_size_;
};

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm).
void civil_from_days(std::int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    std::int64_t yy = static_cast<std::int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

bool all_digits(const std::string& s, std::size_t n) {
    return s.size() == n && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

Result<PackageResult> write_package(const tree::Source& source, const tree::Node& search_root, const std::string& output_path,
                                    const PackageOptions& options, const ProgressFn& progress) {
    PackageResult result;
    if (options.passcode.size() != 32) return make_error(Errc::invalid_argument, "the passcode must be 32 characters");
    Keys keys;
    if (options.keys) {
        keys = *options.keys;
    } else {
        ALEMBIC_ASSIGN(keys, load_keys(options.keys_file));
    }
    ALEMBIC_ASSIGN(const tree::Node* root, ffpfsc::resolve_game_root(search_root));

    // param.sfo: the content ID, the kind of package and the creation date.
    const tree::Node* sce_sys = tree::find_child_icase(*root, "sce_sys");
    const tree::Node* sfo_node = sce_sys && sce_sys->is_directory ? tree::find(*sce_sys, "param.sfo") : nullptr;
    if (!sfo_node || sfo_node->is_directory) return make_error(Errc::invalid_argument, "the game has no sce_sys/param.sfo");
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> sfo_bytes, read_node(source, *sfo_node, 1024 * 1024));
    ALEMBIC_ASSIGN(sfo::Document sfo, sfo::Document::read(sfo_bytes));
    std::string category = sfo.display("CATEGORY");
    if (category != "gd")
        return make_error(Errc::unsupported, "only game applications (param.sfo CATEGORY gd) can be packaged yet; this is '" +
                                                 category + "'");
    std::string content_id = sfo.display("CONTENT_ID");
    if (content_id.size() != 36) return make_error(Errc::invalid_data, "param.sfo has no valid CONTENT_ID");
    result.content_id = content_id;
    // GP4/Gp4Creator.cs keeps c_date and c_time from PUBTOOLINFO.
    std::string c_date, c_time;
    for (std::size_t start = 0;;) {
        std::string info = sfo.display("PUBTOOLINFO");
        std::size_t comma = info.find(',', start);
        std::string item = info.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        if (item.rfind("c_date=", 0) == 0) c_date = item.substr(7);
        if (item.rfind("c_time=", 0) == 0) c_time = item.substr(7);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    if (!all_digits(c_date, 8)) {
        c_date.clear();
        c_time.clear();
    }
    if (!c_time.empty() && !all_digits(c_time, 6)) c_time.clear();
    const bool sfo_dated = !c_date.empty();
    if (c_date.empty()) {
        if (options.creation_date) {
            c_date = *options.creation_date;
            c_time = options.creation_time.value_or("");
        } else {
            int y;
            unsigned m, d;
            civil_from_days(static_cast<std::int64_t>(std::time(nullptr)) / 86400, y, m, d);
            char text[16];
            std::snprintf(text, sizeof text, "%04d%02u%02u", y, m, d);
            c_date = text;
        }
        if (!all_digits(c_date, 8) || (!c_time.empty() && !all_digits(c_time, 6)))
            return make_error(Errc::invalid_argument, "the creation date must be YYYYMMDD and the time HHMMSS");
    }
    // The PFS time: given, else the creation date in param.sfo (it survives
    // every format, so a package rebuilt from any image of it is the same),
    // else the newest file's.
    std::int64_t newest = 0;
    struct Newest {
        static void walk(const tree::Node& n, std::int64_t& t) {
            if (!n.is_directory) t = std::max(t, n.mtime);
            for (const tree::Node& c : n.children) walk(c, t);
        }
    };
    Newest::walk(*root, newest);
    auto days_from_civil = [](int y, unsigned m, unsigned d) {
        y -= m <= 2;
        std::int64_t era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = static_cast<unsigned>(y - era * 400);
        unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
    };
    auto digits = [](const std::string& s, std::size_t at, std::size_t n) { return std::stoi(s.substr(at, n)); };
    std::int64_t dated = 0;
    if (sfo_dated) {
        dated = days_from_civil(digits(c_date, 0, 4), static_cast<unsigned>(digits(c_date, 4, 2)),
                                static_cast<unsigned>(digits(c_date, 6, 2))) * 86400;
        if (!c_time.empty()) dated += digits(c_time, 0, 2) * 3600 + digits(c_time, 2, 2) * 60 + digits(c_time, 4, 2);
    }
    const std::int64_t time = options.timestamp ? *options.timestamp
                            : sfo_dated         ? dated
                            : newest            ? newest
                                                : static_cast<std::int64_t>(std::time(nullptr));

    // The project: the game's tree (each folder's sub-folders and files in
    // name order), less what the package generates again.
    FsTree tree;
    std::vector<std::uint8_t> kept_license, kept_license_info;
    int sce_sys_dir = -1;
    // The order pkg_makegp4 gives a project, whatever the game came from:
    // sce_sys first at the top; in each folder, sub-folders that hold CNT
    // entry files first (by their lowest entry id), then the others by name;
    // entry files first (by id), then the other files by name (the order of
    // a PS4 image's own folders).
    struct Walk {
        const tree::Source& source;
        FsTree& tree;
        std::vector<std::uint8_t>& license;
        std::vector<std::uint8_t>& license_info;
        int& sce_sys_dir;
        // Made again by the package; param.json is what Alembic adds to a PS4
        // game's images (ffpfsc::with_ps4_param_json), never part of a package.
        static bool generated(const std::string& rel) {
            return rel == "license.dat" || rel == "license.info" || rel == "psreserved.dat" ||
                   rel == "playgo-chunk.dat" || rel == "playgo-chunk.sha" || rel == "playgo-manifest.xml" ||
                   rel == "param.json";
        }
        // A file's entry id ("rel" is relative to sce_sys; empty outside it).
        static std::optional<std::uint32_t> entry_of(const std::string& rel) {
            if (rel.empty() || generated(rel)) return std::nullopt;
            return entry_id_for_path(rel);
        }
        static std::uint64_t lowest_entry(const tree::Node& node, const std::string& rel) {
            std::uint64_t lowest = UINT64_MAX;
            for (const tree::Node& c : node.children) {
                std::string r = rel.empty() ? std::string() : rel + c.name;
                if (c.is_directory) lowest = std::min(lowest, lowest_entry(c, r.empty() ? r : r + "/"));
                else if (auto id = entry_of(r)) lowest = std::min<std::uint64_t>(lowest, *id);
            }
            return lowest;
        }
        // `rel`: the folder's path inside sce_sys with a trailing '/' ("" for sce_sys itself).
        Status add(const tree::Node& node, int dir, const std::string& rel, bool in_sys) {
            std::vector<const tree::Node*> dirs, files;
            for (const tree::Node& c : node.children) (c.is_directory ? dirs : files).push_back(&c);
            auto path_of = [&](const tree::Node* n) { return in_sys ? rel + n->name : std::string(); };
            std::sort(dirs.begin(), dirs.end(), [&](const tree::Node* a, const tree::Node* b) {
                bool sa = dir == 0 && a->name == "sce_sys", sb = dir == 0 && b->name == "sce_sys";
                if (sa != sb) return sa;
                std::uint64_t la = in_sys ? lowest_entry(*a, path_of(a) + "/") : UINT64_MAX;
                std::uint64_t lb = in_sys ? lowest_entry(*b, path_of(b) + "/") : UINT64_MAX;
                if (la != lb) return la < lb;
                return a->name < b->name;
            });
            std::sort(files.begin(), files.end(), [&](const tree::Node* a, const tree::Node* b) {
                std::uint64_t ia = entry_of(path_of(a)).value_or(UINT64_MAX);
                std::uint64_t ib = entry_of(path_of(b)).value_or(UINT64_MAX);
                if (ia != ib) return ia < ib;
                return a->name < b->name;
            });
            for (const tree::Node* d : dirs) {
                int index = tree.add_dir(dir, d->name);
                bool is_sce_sys = dir == 0 && d->name == "sce_sys";
                if (is_sce_sys) sce_sys_dir = index;
                ALEMBIC_TRY(add(*d, index, is_sce_sys ? std::string() : in_sys ? rel + d->name + "/" : std::string(),
                                is_sce_sys || in_sys));
            }
            for (const tree::Node* f : files) {
                if (in_sys && rel.empty()) {
                    if (f->name == "license.dat" || f->name == "license.info") {
                        ALEMBIC_ASSIGN(auto data, read_node(source, *f, 0x10000));
                        (f->name == "license.dat" ? license : license_info) = std::move(data);
                        continue;
                    }
                    if (generated(f->name)) continue;
                }
                FsTree::File file;
                file.name = f->name;
                file.size = file.compressed_size = f->size;
                const tree::Source* s = &source;
                file.open = [s, f]() { return s->open(*f); };
                tree.add_file(dir, std::move(file));
            }
            return {};
        }
    } walk{source, tree, kept_license, kept_license_info, sce_sys_dir};
    ALEMBIC_TRY(walk.add(*root, 0, std::string(), false));
    if (sce_sys_dir < 0) return make_error(Errc::invalid_argument, "the game has no sce_sys folder");
    // A game application needs a keystone; one is made from the passcode when missing.
    bool has_keystone = false;
    for (int f : tree.dirs[static_cast<std::size_t>(sce_sys_dir)].files)
        if (tree.files[static_cast<std::size_t>(f)].name == "keystone") has_keystone = true;
    if (!has_keystone) {
        auto keystone = std::make_shared<std::vector<std::uint8_t>>(create_keystone(keys, options.passcode));
        FsTree::File file;
        file.name = "keystone";
        file.size = file.compressed_size = keystone->size();
        file.open = [keystone]() -> Result<std::shared_ptr<const RandomAccess>> {
            return std::shared_ptr<const RandomAccess>(std::make_shared<MemoryFile>(*keystone));
        };
        tree.add_file(sce_sys_dir, std::move(file));
        result.generated_keystone = true;
    }
    if (kept_license.size() != 0x400 || kept_license_info.size() != 0x200) {
        if (!kept_license.empty() || !kept_license_info.empty())
            result.warnings.push_back("the game's licence files have unexpected sizes; a new licence was generated");
        kept_license = license_dat(keys, content_id);
        kept_license_info = license_info(content_id);
        result.generated_license = true;
    }
    // sce_sys files that become entries, in the folder's order (not param.sfo).
    struct FileEntry {
        std::uint32_t id;
        std::string name;
        std::vector<std::uint8_t> data;
    };
    std::vector<FileEntry> file_entries;
    for (int f : tree.dirs[static_cast<std::size_t>(sce_sys_dir)].files) {
        const FsTree::File& file = tree.files[static_cast<std::size_t>(f)];
        if (file.name == "param.sfo") continue;
        auto id = entry_id_for_name(file.name);
        if (!id) continue;
        if (file.size > 256u * 1024 * 1024) return make_error(Errc::too_large, "sce_sys/" + file.name + " is too large");
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, file.open());
        std::vector<std::uint8_t> data(static_cast<std::size_t>(file.size));
        if (!data.empty()) ALEMBIC_TRY(in->read_at(0, data));
        file_entries.push_back({*id, file.name, std::move(data)});
    }

    // The inner PFS, and the signed, encrypted outer one holding it.
    const crypto::Digest ekpfs = compute_keys_ps4(content_id, options.passcode, 1);
    detail::PfsProperties inner_p;
    inner_p.min_blocks = 0x55;
    inner_p.time = time;
    ALEMBIC_ASSIGN(std::shared_ptr<detail::PfsImage> inner, detail::PfsImage::plan(std::move(tree), inner_p));
    result.inner_files = inner->file_count();
    const std::uint64_t inner_size = inner->size();
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> inner_file, inner->open());
    std::vector<std::uint8_t> pfsc = detail::pfsc_header(inner_size);
    const std::uint64_t pfsc_size = pfsc.size() + inner_size;
    auto pfs_image = std::make_shared<PfscFile>(std::move(pfsc), inner_file, inner_size);
    FsTree outer_tree;
    {
        FsTree::File file;
        file.name = "pfs_image.dat";
        file.size = pfsc_size;
        file.compressed_size = inner_size;
        file.compress = true;
        file.open = [pfs_image]() -> Result<std::shared_ptr<const RandomAccess>> {
            return std::shared_ptr<const RandomAccess>(pfs_image);
        };
        outer_tree.add_file(0, std::move(file));
    }
    detail::PfsProperties outer_p;
    outer_p.sign = outer_p.encrypt = true;
    outer_p.time = time;
    outer_p.ekpfs = ekpfs;
    ALEMBIC_ASSIGN(std::shared_ptr<detail::PfsImage> outer, detail::PfsImage::plan(std::move(outer_tree), outer_p));
    const std::uint64_t pfs_size = outer->size();

    Header header;
    header.content_id = content_id;

    // param.sfo as the package stores it.
    {
        std::uint64_t first_package_size = 0x80000 + pfs_size; // what BuildPkg has at this point
        std::string info = "c_date=" + c_date + (c_time.empty() ? "" : ",c_time=" + c_time) +
                           ",img0_l0_size=" + std::to_string((first_package_size + 0xFFFFF) / (1024 * 1024)) +
                           ",img0_l1_size=0,img0_sc_ksize=512,img0_pc_ksize=832";
        sfo.set_text("PUBTOOLINFO", info, 0x200);
        sfo.set_number("PUBTOOLVER", 0x02890000);
    }
    const std::vector<std::uint8_t> sfo_out = sfo.serialize();

    // The entries, in body order.
    enum Slot { kKeys, kImageKey, kGeneral, kMetas, kDigests, kNames, kChunkDat, kChunkSha, kChunkXml };
    std::vector<Entry> entries;
    auto add = [&](std::uint32_t id, const char* name, std::vector<std::uint8_t> data) {
        Entry e;
        e.id = id;
        if (name) {
            e.named = true;
            e.name = name;
        }
        e.data = std::move(data);
        entries.push_back(std::move(e));
    };
    add(0x10, nullptr, entry_keys(keys, content_id, options.passcode));
    add(0x20, nullptr, rsa::mt_wrap(keys.fake_modulus, Bytes(ekpfs.data(), 32)));
    add(0x80, nullptr, std::vector<std::uint8_t>(0x180, 0));
    add(0x100, nullptr, {});
    add(0x1, nullptr, {});
    add(0x200, nullptr, {});
    add(0x1001, "playgo-chunk.dat", std::vector<std::uint8_t>(416, 0));
    add(0x1002, "playgo-chunk.sha", {});
    add(0x1003, "playgo-manifest.xml", std::vector<std::uint8_t>(kManifest, kManifest + sizeof kManifest - 1));
    add(0x400, nullptr, kept_license);
    add(0x401, nullptr, kept_license_info);
    add(0x1000, "param.sfo", sfo_out);
    add(0x409, nullptr, std::vector<std::uint8_t>(0x2000, 0));
    for (FileEntry& f : file_entries) add(f.id, f.name.c_str(), std::move(f.data));
    const std::size_t count = entries.size();
    entries[kDigests].data.assign(count * 32, 0);

    // Names: "" first, then each named entry in .NET string order.
    std::vector<std::uint8_t> names(1, 0);
    {
        std::vector<std::size_t> order(count);
        for (std::size_t i = 0; i < count; ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const Entry& x = entries[a];
            const Entry& y = entries[b];
            if (!x.named || !y.named) return !x.named && y.named;
            return unicode::dotnet_compare(x.name, y.name) < 0;
        });
        std::vector<std::pair<std::string, std::uint32_t>> table;
        for (std::size_t i : order) {
            Entry& e = entries[i];
            if (!e.named || e.name.empty()) continue;
            auto it = std::find_if(table.begin(), table.end(), [&](const auto& t) { return t.first == e.name; });
            if (it != table.end()) {
                e.name_offset = it->second;
                continue;
            }
            e.name_offset = static_cast<std::uint32_t>(names.size());
            table.push_back({e.name, e.name_offset});
            names.insert(names.end(), e.name.begin(), e.name.end());
            names.push_back(0);
        }
    }
    entries[kNames].data = names;

    // Offsets and sizes (BuildPkg's second pass). LibOrbisPkg reserves the
    // PlayGo chunk digests' room from an estimate made before the entries are
    // laid out, and gives up when the package outgrows it (large sce_sys
    // files can push the body past another 512 KiB). There, and only there,
    // Alembic reserves what is needed and lays the entries out again.
    std::uint64_t chunk_room = 0; // 0: LibOrbisPkg's estimate
    for (;;) {
        std::uint64_t offset = kBodyOffset;
        for (std::size_t i = 0; i < count; ++i) {
            Entry& e = entries[i];
            switch (e.id) {
            case 0x1: e.flags1 = 0x40000000; break;
            case 0x10: e.flags1 = 0x60000000; break;
            case 0x20: e.flags1 = 0xE0000000; e.flags2 = 3u << 12; break;
            case 0x80: e.flags1 = 0x60000000; break;
            case 0x100: e.flags1 = 0x60000000; break;
            case 0x200: e.flags1 = 0x40000000; break;
            case 0x400: e.flags1 = 0x80000000; e.flags2 = 3u << 12; break;
            case 0x401: e.flags1 = 0x80000000; e.flags2 = 2u << 12; break;
            default: break;
            }
            e.offset = static_cast<std::uint32_t>(offset);
            if (i == kMetas) {
                e.size = static_cast<std::uint32_t>(count * 32);
            } else if (i == kChunkSha) {
                // Room for the chunk digests, estimated from the entries so far
                // (metas: one per entry up to this one; the digests themselves: none).
                std::uint64_t sum = 0;
                for (std::size_t j = 0; j < count; ++j) {
                    std::uint64_t length = j == kMetas ? (i + 1) * 32 : j == kChunkSha ? 0 : entries[j].data.size();
                    sum += align(length, 16);
                }
                std::uint64_t estimate = align(kBodyOffset + sum, 0x80000) + pfs_size;
                estimate += ((estimate + 16) / 0x10000) * 4;
                e.size = static_cast<std::uint32_t>(chunk_room ? chunk_room : (estimate / 0x10000) * 4);
            } else {
                e.size = static_cast<std::uint32_t>(e.data.size());
            }
            offset = align(offset + e.size, 16);
        }
        std::uint64_t body = offset - kBodyOffset;
        header.entry_count = static_cast<std::uint32_t>(count);
        header.body_size = align(kBodyOffset + body, 0x80000) - kBodyOffset;
        header.main_ent_data_size = static_cast<std::uint32_t>(2048 + 256 + 0x180 + count * 32 + count * 32);
        header.pfs_offset = kBodyOffset + header.body_size;
        header.pfs_size = pfs_size;
        header.package_size = header.mount_image_size = header.body_size + kBodyOffset + pfs_size;
        std::uint64_t chunk_sha = 4 * (header.package_size / 0x10000);
        if (chunk_sha > entries[kChunkSha].size) {
            if (chunk_room >= chunk_sha) return make_error(Errc::invalid_data, "the PlayGo chunk digests do not fit");
            chunk_room = chunk_sha;
            continue;
        }
        entries[kChunkSha].size = static_cast<std::uint32_t>(chunk_sha);
        entries[kChunkSha].data.assign(static_cast<std::size_t>(chunk_sha), 0);
        entries[kChunkDat].data = chunk_dat(content_id, header.package_size, inner_size);
        header.promote_size = static_cast<std::uint32_t>(header.body_size + kBodyOffset);
        break;
    }
    result.package_length = header.package_size;

    // The outer PFS, straight into the package.
    ALEMBIC_ASSIGN(AtomicOutput output, AtomicOutput::create(output_path, options.overwrite));
    File& out = output.file();
    ALEMBIC_TRY(out.truncate(header.package_size));
    const unsigned threads = worker_count(options.threads);
    if (progress && !progress({"Writing the package", 0, pfsc_size})) return make_error(Errc::cancelled, "cancelled");
    ALEMBIC_TRY(outer->write(out, header.pfs_offset, threads, [&](std::uint64_t done) {
        return !progress || progress({"Writing the package", done, pfsc_size});
    }));

    // Read back: the PFS digests and the PlayGo chunk digests.
    {
        Sha256 whole;
        const std::uint64_t batch = 64;
        std::vector<std::uint8_t> buffer(static_cast<std::size_t>(batch * kBlock));
        std::vector<std::uint8_t>& chunks = entries[kChunkSha].data;
        Pool pool(threads);
        for (std::uint64_t done = 0; done < pfs_size;) {
            std::uint64_t n = std::min<std::uint64_t>(buffer.size(), pfs_size - done);
            ALEMBIC_TRY(out.read_at(header.pfs_offset + done, MutableBytes(buffer.data(), static_cast<std::size_t>(n))));
            if (done == 0) header.pfs_signed_digest = crypto::sha256(Bytes(buffer.data(), kBlock));
            std::uint64_t first_chunk = (header.pfs_offset + done) / kBlock;
            std::size_t jobs = static_cast<std::size_t>(n / kBlock);
            std::mutex mutex;
            std::condition_variable cv;
            std::size_t left = jobs;
            for (std::size_t k = 0; k < jobs; ++k)
                pool.submit([&, k](unsigned) {
                    crypto::Digest d = crypto::sha256(Bytes(buffer.data() + k * kBlock, kBlock));
                    std::memcpy(&chunks[static_cast<std::size_t>((first_chunk + k) * 4)], d.data(), 4);
                    std::lock_guard<std::mutex> lock(mutex);
                    if (--left == 0) cv.notify_all();
                });
            whole.update(Bytes(buffer.data(), static_cast<std::size_t>(n)));
            {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait(lock, [&] { return left == 0; });
            }
            done += n;
            if (progress && !progress({"Hashing the package", done, pfs_size})) return make_error(Errc::cancelled, "cancelled");
        }
        header.pfs_image_digest = whole.finish();
    }

    // FinishPkg: general digests, body, body digests, header, its digest and signature.
    {
        std::string major = "ATTRIBUTE" + sfo.display("ATTRIBUTE");
        if (sfo.find("ATTRIBUTE2")) major += "ATTRIBUTE2" + sfo.display("ATTRIBUTE2");
        major += "CATEGORY" + sfo.display("CATEGORY") + "FORMAT" + sfo.display("FORMAT") + "PUBTOOLVER" +
                 sfo.display("PUBTOOLVER");
        crypto::Digest major_digest = crypto::sha256(Bytes(reinterpret_cast<const std::uint8_t*>(major.data()), major.size()));
        std::vector<std::uint8_t> content(36 + 12 + 8, 0);
        std::memcpy(content.data(), content_id.data(), 36);
        put_be32(&content[48], kDrmTypePs4);
        put_be32(&content[52], kContentTypeGd);
        content.insert(content.end(), header.pfs_image_digest.begin(), header.pfs_image_digest.end());
        content.insert(content.end(), major_digest.begin(), major_digest.end());
        crypto::Digest content_digest = crypto::sha256(Bytes(content.data(), content.size()));
        crypto::Digest param_digest = crypto::sha256(Bytes(sfo_out.data(), sfo_out.size()));
        crypto::Digest head = header_digest(header);
        std::vector<std::uint8_t>& g = entries[kGeneral].data;
        put_be16(&g[0], 0xD256);
        put_be16(&g[2], 0x100);
        put_be32(&g[0x1C], 0x2 | 0x4 | 0x8 | 0x20 | 0x40);
        std::memcpy(&g[0x20], content_digest.data(), 32);
        std::memcpy(&g[0x40], header.pfs_image_digest.data(), 32);
        std::memcpy(&g[0x60], head.data(), 32);
        std::memcpy(&g[0xA0], major_digest.data(), 32);
        std::memcpy(&g[0xC0], param_digest.data(), 32);
    }
    std::vector<std::uint8_t> body(static_cast<std::size_t>(header.pfs_offset), 0);
    std::vector<std::size_t> sorted(count);
    for (std::size_t i = 0; i < count; ++i) sorted[i] = i;
    std::stable_sort(sorted.begin(), sorted.end(), [&](std::size_t a, std::size_t b) { return entries[a].id < entries[b].id; });
    {
        std::vector<std::uint8_t>& metas = entries[kMetas].data;
        metas.assign(count * 32, 0);
        for (std::size_t i = 0; i < count; ++i) entries[sorted[i]].meta(&metas[i * 32]);
    }
    for (Entry& e : entries) {
        std::vector<std::uint8_t> data = e.data;
        data.resize(e.size, 0);
        if (e.encrypted()) {
            if (data.size() % 16) return make_error(Errc::invalid_data, "an encrypted entry is not a multiple of 16 bytes");
            std::uint8_t seed_input[64];
            e.meta(seed_input);
            crypto::Digest key = compute_keys_ps4(content_id, options.passcode, e.key_index());
            std::memcpy(seed_input + 32, key.data(), 32);
            crypto::Digest seed = crypto::sha256(Bytes(seed_input, 64));
            crypto::Aes128 aes(seed.data() + 16);
            crypto::cbc_encrypt(aes, seed.data(), data);
        }
        if (e.offset + data.size() > body.size()) return make_error(Errc::invalid_data, "a CNT entry runs into the PFS");
        if (!data.empty()) std::memcpy(&body[e.offset], data.data(), data.size());
    }
    {
        Entry& digests = entries[kDigests];
        for (std::size_t i = 1; i < count; ++i) {
            const Entry& e = entries[sorted[i]];
            crypto::Digest d = crypto::sha256(Bytes(&body[e.offset], e.size));
            std::memcpy(&digests.data[i * 32], d.data(), 32);
            std::memcpy(&body[digests.offset + i * 32], d.data(), 32);
        }
        header.body_digest = crypto::sha256(Bytes(&body[kBodyOffset], static_cast<std::size_t>(header.body_size)));
        header.digest_table = crypto::sha256(Bytes(digests.data.data(), digests.data.size()));
        std::vector<std::uint8_t> sc;
        for (Slot slot : {kKeys, kImageKey, kGeneral, kMetas, kDigests}) {
            const Entry& e = entries[slot];
            sc.insert(sc.end(), body.begin() + e.offset, body.begin() + e.offset + e.size);
        }
        header.sc_entries1 = crypto::sha256(Bytes(sc.data(), sc.size()));
        sc.clear();
        for (Slot slot : {kKeys, kImageKey, kGeneral, kMetas}) {
            const Entry& e = entries[slot];
            std::size_t size = slot == kMetas ? header.sc_entry_count * 0x20u : e.size;
            sc.insert(sc.end(), body.begin() + e.offset, body.begin() + e.offset + static_cast<std::ptrdiff_t>(size));
        }
        header.sc_entries2 = crypto::sha256(Bytes(sc.data(), sc.size()));
    }
    header.write(body.data());
    crypto::Digest head = crypto::sha256(Bytes(body.data(), 0xFE0));
    std::memcpy(&body[0xFE0], head.data(), 32);
    crypto::Digest whole_head = crypto::sha256(Bytes(body.data(), 0x1000));
    std::vector<std::uint8_t> signature = rsa::mt_wrap(keys.package_public[3], Bytes(whole_head.data(), 32));
    std::memcpy(&body[0x1000], signature.data(), 256);
    ALEMBIC_TRY(out.write_at(0, Bytes(body.data(), body.size())));
    ALEMBIC_TRY(output.commit());
    return result;
}

} // namespace alembic::fpkg::ps4
