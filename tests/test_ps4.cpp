// SPDX-License-Identifier: GPL-3.0-or-later
// PS4 packages: key derivation (vectors from Python's hashlib/hmac), param.sfo,
// and a small synthetic package read end to end (CNT, signed and encrypted
// outer PFS, PFSC version 2, inner PFS). Real packages are checked by
// tools/pkg-oracle/check_samples.sh.
#include "alembic/bytes.hpp"
#include "alembic/convert.hpp"
#include "alembic/crypto.hpp"
#include "alembic/fpkg.hpp"
#include "alembic/pfs.hpp"
#include "alembic/pfsc.hpp"
#include "alembic/sfo.hpp"
#include "test.hpp"

#include <cstring>
#include <string>

using namespace alembic;

namespace {

const std::string kContentId = "UP0000-CUSA00000_00-0000000000000000";
const std::string kPasscode(32, '0');
constexpr std::uint64_t kBlock = 0x10000;

std::string to_hex(const std::uint8_t* p, std::size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

void put_be32(std::uint8_t* p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}
void put_be64(std::uint8_t* p, std::uint64_t v) {
    put_be32(p, static_cast<std::uint32_t>(v >> 32));
    put_be32(p + 4, static_cast<std::uint32_t>(v));
}

std::vector<std::uint8_t> pattern(std::size_t size, std::uint8_t seed) {
    std::vector<std::uint8_t> data(size);
    for (std::size_t i = 0; i < size; ++i) data[i] = static_cast<std::uint8_t>(seed + i * 7 + (i >> 10));
    return data;
}

// A param.sfo with UTF-8 text entries and one number.
std::vector<std::uint8_t> make_sfo(const std::vector<std::pair<std::string, std::string>>& text) {
    std::vector<std::uint8_t> keys, values;
    std::vector<std::uint8_t> rows;
    auto add = [&](const std::string& key, std::uint16_t format, const std::vector<std::uint8_t>& value) {
        std::uint8_t row[16] = {};
        put_u16(row, static_cast<std::uint16_t>(keys.size()));
        put_u16(row + 2, format);
        put_u32(row + 4, static_cast<std::uint32_t>(value.size()));
        put_u32(row + 8, static_cast<std::uint32_t>(value.size()));
        put_u32(row + 12, static_cast<std::uint32_t>(values.size()));
        rows.insert(rows.end(), row, row + 16);
        keys.insert(keys.end(), key.begin(), key.end());
        keys.push_back(0);
        values.insert(values.end(), value.begin(), value.end());
    };
    for (const auto& [k, v] : text) {
        std::vector<std::uint8_t> value(v.begin(), v.end());
        value.push_back(0);
        add(k, 0x0204, value);
    }
    add("APP_TYPE", 0x0404, {1, 0, 0, 0});
    while (keys.size() % 4) keys.push_back(0);
    std::vector<std::uint8_t> out(0x14);
    std::memcpy(out.data(), "\0PSF", 4);
    put_u32(&out[4], 0x101);
    put_u32(&out[8], static_cast<std::uint32_t>(0x14 + rows.size()));
    put_u32(&out[12], static_cast<std::uint32_t>(0x14 + rows.size() + keys.size()));
    put_u32(&out[16], static_cast<std::uint32_t>(rows.size() / 16));
    out.insert(out.end(), rows.begin(), rows.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), values.begin(), values.end());
    return out;
}

// One 0x2C8-byte signed inode with direct block pointers.
void put_inode(std::uint8_t* e, std::uint16_t mode, std::uint64_t size, const std::vector<std::uint32_t>& blocks) {
    put_u16(e, mode);
    put_u64(e + 8, size);
    put_u64(e + 16, size);
    put_u32(e + 0x60, static_cast<std::uint32_t>(blocks.size()));
    for (std::size_t k = 0; k < blocks.size(); ++k) put_u32(e + 0x64 + k * 36 + 32, blocks[k]);
}

std::size_t put_dirent(std::uint8_t* p, std::uint32_t ino, std::uint32_t type, const std::string& name) {
    std::size_t size = (16 + name.size() + 1 + 7) & ~std::size_t(7);
    put_u32(p, ino);
    put_u32(p + 4, type);
    put_u32(p + 8, static_cast<std::uint32_t>(name.size()));
    put_u32(p + 12, static_cast<std::uint32_t>(size));
    std::memcpy(p + 16, name.data(), name.size());
    return size;
}

// Builds a PS4 package holding `game` (a folder) at `path`. Returns its eboot.bin.
std::vector<std::uint8_t> make_package(const std::string& tmp, const std::string& path) {
    std::string game = tmp + "/game";
    test::make_dir(game);
    test::make_dir(game + "/sce_sys");
    std::vector<std::uint8_t> eboot = pattern(150000, 3);
    test::write_file(game + "/eboot.bin", eboot);
    test::write_file(game + "/sce_sys/keystone", pattern(96, 9));

    // Inner PFS, then PFSC with the PS4 version field.
    auto folder = tree::open_folder(game);
    if (!folder.ok()) return {};
    pfs::BuildOptions pfs_options;
    pfs_options.timestamp = 1600000000;
    if (!pfs::write_image(**folder, (*folder)->root(), tmp + "/inner.pfs", pfs_options, true).ok()) return {};
    {
        auto in = File::open(tmp + "/inner.pfs", File::Mode::read);
        auto out = File::open(tmp + "/inner.pfsc", File::Mode::create_new);
        if (!in.ok() || !out.ok() || !pfsc::encode_file(*in, *out, 0).ok()) return {};
    }
    std::vector<std::uint8_t> image = test::read_file(tmp + "/inner.pfsc");
    put_u32(&image[8], pfsc::kVersionFieldPs4);
    std::size_t image_blocks = (image.size() + kBlock - 1) / kBlock;
    if (image_blocks > 12) return {}; // direct pointers only

    // Outer PFS: superblock, inode table, super root, uroot, pfs_image.dat.
    std::uint32_t block_count = static_cast<std::uint32_t>(4 + image_blocks);
    std::vector<std::uint8_t> pfs(block_count * kBlock);
    std::uint8_t* sb = pfs.data();
    put_u64(sb, 1);
    put_u64(sb + 8, 20130315);
    put_u16(sb + 0x1C, 0x0D); // signed | encrypted | case-insensitive
    put_u32(sb + 0x20, kBlock);
    put_u64(sb + 0x28, 1);
    put_u64(sb + 0x30, 4);
    put_u64(sb + 0x38, block_count);
    put_u64(sb + 0x40, 1);
    for (int i = 0; i < 16; ++i) sb[0x370 + i] = static_cast<std::uint8_t>(0xA0 + i);
    std::uint8_t* inodes = pfs.data() + kBlock;
    std::vector<std::uint32_t> data_blocks;
    for (std::uint32_t i = 0; i < image_blocks; ++i) data_blocks.push_back(4 + i);
    std::uint8_t* uroot = pfs.data() + 3 * kBlock;
    std::size_t at = put_dirent(uroot, 2, 4, ".");
    at += put_dirent(uroot + at, 0, 5, "..");
    at += put_dirent(uroot + at, 3, 2, "pfs_image.dat");
    put_inode(inodes, 0x416D, kBlock, {2});
    put_inode(inodes + 0x2C8, 0x816D, 0, {});
    put_inode(inodes + 2 * 0x2C8, 0x416D, at, {3});
    put_inode(inodes + 3 * 0x2C8, 0x816D, image.size(), data_blocks);
    std::memcpy(pfs.data() + 4 * kBlock, image.data(), image.size());

    crypto::Digest ekpfs = fpkg::compute_keys_ps4(kContentId, kPasscode, 1);
    std::uint8_t message[20] = {1, 0, 0, 0};
    std::memcpy(message + 4, sb + 0x370, 16);
    crypto::Digest keys = crypto::hmac_sha256(Bytes(ekpfs.data(), 32), Bytes(message, 20));
    message[0] = 2;
    crypto::Digest sign = crypto::hmac_sha256(Bytes(ekpfs.data(), 32), Bytes(message, 20));
    crypto::Digest icv = crypto::hmac_sha256(Bytes(sign.data(), 32), Bytes(sb, 0x5A0));
    std::memcpy(sb + 0x380, icv.data(), 32);
    crypto::Aes128 tweak(keys.data()), data(keys.data() + 16);
    for (std::uint64_t sector = kBlock / 0x1000; sector < pfs.size() / 0x1000; ++sector)
        crypto::xts_encrypt(data, tweak, sector, MutableBytes(pfs.data() + sector * 0x1000, 0x1000));

    // CNT: header, entry table at 0x800, entry data from 0x1000, PFS at 0x10000.
    std::vector<std::uint8_t> sfo = make_sfo({{"TITLE", "Synthetic"}, {"TITLE_ID", "CUSA00000"},
                                              {"CONTENT_ID", kContentId}, {"APP_VER", "01.00"}});
    const std::string names("\0param.sfo\0", 11);
    std::vector<std::uint8_t> pkg(kBlock);
    std::memcpy(pkg.data(), "\x7F" "CNT", 4);
    put_be32(&pkg[0x10], 2);
    put_be32(&pkg[0x18], 0x800);
    std::memcpy(&pkg[0x40], kContentId.data(), kContentId.size());
    put_be32(&pkg[0x400], 1);
    put_be64(&pkg[0x410], kBlock);
    put_be64(&pkg[0x418], pfs.size());
    put_be32(&pkg[0x438], kBlock);
    crypto::Digest head = crypto::sha256(Bytes(pfs.data(), kBlock));
    std::memcpy(&pkg[0x460], head.data(), 32);
    auto entry = [&](int row, std::uint32_t id, std::uint32_t name, std::uint32_t offset, std::size_t size) {
        std::uint8_t* r = &pkg[0x800 + row * 0x20];
        put_be32(r, id);
        put_be32(r + 4, name);
        put_be32(r + 16, offset);
        put_be32(r + 20, static_cast<std::uint32_t>(size));
    };
    entry(0, 0x200, 0, 0x1000, names.size());
    entry(1, 0x1000, 1, 0x1100, sfo.size());
    std::memcpy(&pkg[0x1000], names.data(), names.size());
    std::memcpy(&pkg[0x1100], sfo.data(), sfo.size());
    pkg.insert(pkg.end(), pfs.begin(), pfs.end());
    test::write_file(path, pkg);
    return eboot;
}

std::vector<std::uint8_t> read_all(const tree::Source& source, const tree::Node& file) {
    auto in = source.open(file);
    if (!in.ok()) return {};
    std::vector<std::uint8_t> data(static_cast<std::size_t>(file.size));
    if (!data.empty() && !(*in)->read_at(0, data).ok()) return {};
    return data;
}

} // namespace

TEST_CASE(ps4_key_derivation) {
    // SHA-256(SHA-256(BE32(i)) || SHA-256(content id padded to 48) || passcode), from hashlib.
    crypto::Digest ekpfs = fpkg::compute_keys_ps4(kContentId, kPasscode, 1);
    CHECK(to_hex(ekpfs.data(), 32) == "ffa6791ad3fd28c095d099116856cafcaac99119156a81e58f8d0c0d644af14a");
    crypto::Digest key0 = fpkg::compute_keys_ps4(kContentId, kPasscode, 0);
    CHECK(to_hex(key0.data(), 32) == "296df5c438c42227da647834a9c7cdebfea077b2d8df5e2ef864bd4e4a38b6ad");
    // The PFS keys for seed 00..0f, from hmac.
    std::uint8_t message[20] = {1, 0, 0, 0};
    for (int i = 0; i < 16; ++i) message[4 + i] = static_cast<std::uint8_t>(i);
    crypto::Digest keys = crypto::hmac_sha256(Bytes(ekpfs.data(), 32), Bytes(message, 20));
    CHECK(to_hex(keys.data(), 32) == "daaec9b37fb66eabbb4e87028f36fa669c1fb13a6344a0aed8ae0b3688c0e97c");
}

TEST_CASE(sfo_parses_text_and_numbers) {
    auto data = make_sfo({{"TITLE", "A game"}, {"TITLE_ID", "CUSA12345"}});
    auto param = sfo::parse(data);
    REQUIRE_OK(param);
    CHECK(param->get("TITLE") == std::optional<std::string>("A game"));
    CHECK(param->get("TITLE_ID") == std::optional<std::string>("CUSA12345"));
    CHECK_EQ(param->numbers.at("APP_TYPE"), 1u);
    CHECK(!param->get("MISSING"));
    data.resize(0x20);
    CHECK(!sfo::parse(data).ok());
    CHECK(!sfo::parse(std::vector<std::uint8_t>{'{', '}'}).ok());
}

TEST_CASE(ps4_package_reads_end_to_end) {
    std::string tmp = test::make_temp_dir();
    std::string pkg_path = tmp + "/game.pkg";
    std::vector<std::uint8_t> eboot = make_package(tmp, pkg_path);
    CHECK(!eboot.empty());
    if (eboot.empty()) return;

    auto opened = File::open(pkg_path, File::Mode::read);
    REQUIRE_OK(opened);
    std::shared_ptr<const RandomAccess> file = std::make_shared<File>(std::move(*opened));
    auto package = fpkg::read_package(*file);
    REQUIRE_OK(package);
    CHECK(package->kind == fpkg::Kind::ps4_package);
    CHECK_EQ(package->pfs_offset, kBlock);
    auto digests = fpkg::check_digests(*file, *package);
    REQUIRE_OK(digests);
    CHECK(digests->fih_digest_ok);
    auto outer = fpkg::OuterPfs::open(file, *package);
    REQUIRE_OK(outer);
    CHECK((*outer)->superblock_icv_ok());
    CHECK_EQ((*outer)->files().size(), std::size_t(1));
    // A wrong passcode fails the signature and the root folder.
    auto wrong = fpkg::OuterPfs::open(file, *package, std::string(32, '1'));
    CHECK(!wrong.ok() || !(*wrong)->superblock_icv_ok());

    auto input = convert::open(pkg_path);
    REQUIRE_OK(input);
    const tree::Node& root = input->tree->root();
    const tree::Node* e = tree::find(root, "eboot.bin");
    const tree::Node* sfo = tree::find(root, "sce_sys/param.sfo");
    CHECK(e && sfo && tree::find(root, "sce_sys/keystone"));
    if (e) CHECK(read_all(*input->tree, *e) == eboot);
    CHECK(ffpfsc::is_ps4_game(root));
    CHECK(ffpfsc::read_title_id(*input->tree, root) == std::optional<std::string>("CUSA00000"));

    // Any image out; no PS5 package.
    convert::Options options;
    options.ffpfsc.build_timestamp = 1700000000;
    auto report = convert::run(pkg_path, tmp + "/out.exfat", convert::Format::exfat, options);
    REQUIRE_OK(report);
    CHECK(report->title_id == "CUSA00000");
    CHECK(!report->ampr_emulator_added);
    auto back = convert::compare(pkg_path, tmp + "/out.exfat");
    REQUIRE_OK(back);
    CHECK(back->same());
    CHECK(!convert::run(pkg_path, tmp + "/out.pkg", convert::Format::fpkg, options).ok());
}
