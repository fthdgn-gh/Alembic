// SPDX-License-Identifier: GPL-3.0-or-later
// Fake-signed SELFs. Byte parity with make_fself.py on more shapes is for
// tools/fself-oracle/compare.sh; the digests here are its output.
#include "alembic/ampr.hpp"
#include "alembic/bytes.hpp"
#include "alembic/convert.hpp"
#include "alembic/fself.hpp"
#include "alembic/sha256.hpp"
#include "test.hpp"

#include <cstring>
#include <string>

using namespace alembic;

namespace {

constexpr std::uint16_t kExecAslr = 0xFE10, kDynamic = 0xFE18;

// ps5-dump-forge's test ELF (fself.rs): a PT_LOAD of 0x4321 bytes with the
// process param, a RELRO, a PT_DYNAMIC, a PT_SCE_VERSION, section headers at
// the end. make_fself.py --ptype fake makes an 18917-byte SELF of it.
std::vector<std::uint8_t> forge_elf(std::uint16_t type) {
    std::vector<std::uint8_t> f(0x58C0);
    for (std::size_t i = 0; i < f.size(); ++i) f[i] = static_cast<std::uint8_t>(i * 7 + 3);
    const std::uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 9, 0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x55};
    std::memcpy(f.data(), ident, 16);
    std::uint8_t* p = f.data();
    put_u16(p + 0x10, type);
    put_u16(p + 0x12, 0x3E);
    put_u32(p + 0x14, 1);
    put_u64(p + 0x18, 0x1234);
    put_u64(p + 0x20, 0x40);
    put_u64(p + 0x28, 0x5800);
    put_u32(p + 0x30, 0);
    const std::uint16_t half[6] = {0x40, 0x38, 5, 0x40, 3, 2};
    for (int i = 0; i < 6; ++i) put_u16(p + 0x34 + 2 * i, half[i]);
    const struct {
        std::uint32_t type;
        std::uint64_t offset, size;
    } phdrs[5] = {{1, 0x1000, 0x4321}, {0x61000001, 0x1200, 0x40}, {0x61000010, 0x5400, 0x123}, {2, 0x5600, 0x80},
                  {0x6FFFFF01, 0x5700, 0x25}};
    for (int i = 0; i < 5; ++i) {
        std::uint8_t* h = p + 0x40 + i * 0x38;
        put_u32(h, phdrs[i].type);
        put_u32(h + 4, 4);
        put_u64(h + 8, phdrs[i].offset);
        put_u64(h + 0x10, 0x100000 + phdrs[i].offset);
        put_u64(h + 0x18, 0x100000 + phdrs[i].offset);
        put_u64(h + 0x20, phdrs[i].size);
        put_u64(h + 0x28, phdrs[i].size + 0x100);
        put_u64(h + 0x30, 0x4000);
    }
    put_u32(p + 0x1200, 0x4942524F);
    put_u32(p + 0x120C, 0x04500031);
    return f;
}

std::string sha256_hex(const std::vector<std::uint8_t>& data) { return Sha256::hex(Sha256::portable(data)); }

// A file of `source`, or nothing when it cannot be read (the CHECK then fails).
std::vector<std::uint8_t> read(const tree::Source& source, const std::string& path, std::uint64_t at = 0,
                               std::optional<std::size_t> length = std::nullopt) {
    const tree::Node* node = tree::find(source.root(), path);
    if (!node) return {};
    auto file = source.open(*node);
    if (!file.ok()) return {};
    std::vector<std::uint8_t> out(length.value_or(static_cast<std::size_t>(node->size - at)));
    if (!(*file)->read_at(at, out).ok()) return {};
    return out;
}

} // namespace

TEST_CASE(fself_matches_make_fself) {
    auto self = fself::make_fself(forge_elf(kExecAslr));
    REQUIRE_OK(self);
    CHECK_EQ(self->size(), 18917u);
    CHECK(sha256_hex(*self) == "947b58989f257f1d0990fa4cb3e0eef81ca8757d32634f56236983d4fb10ea9c");
    // The fake SELF magic, its type and authority ID.
    CHECK_EQ(get_u32(self->data()), 0x1D3D154Fu);
    // Not an unsigned ELF any more; neither is a PS4-style ET_EXEC.
    CHECK(!fself::is_unsigned_elf(*self));
    std::vector<std::uint8_t> exec = forge_elf(2);
    CHECK(!fself::is_unsigned_elf(exec));
    CHECK(fself::is_unsigned_elf(forge_elf(kDynamic)));
}

TEST_CASE(fself_refuses_what_make_fself_refuses) {
    std::vector<std::uint8_t> elf = forge_elf(kExecAslr);
    put_u16(elf.data() + 0x12, 0xB7); // not x86-64
    CHECK(!fself::make_fself(elf).ok());
    elf = forge_elf(kExecAslr);
    put_u64(elf.data() + 0x40 + 0x20, 0x9000); // the PT_LOAD runs past the end
    CHECK(!fself::make_fself(elf).ok());
    elf = forge_elf(kExecAslr);
    elf[4] = 1; // 32-bit
    CHECK(!fself::make_fself(elf).ok());
}

TEST_CASE(fself_signs_a_tree_as_it_is_read) {
    auto dir = test::make_temp_dir();
    std::string root = dir + "/game";
    for (const char* d : {"", "/sce_module", "/fakelib", "/data"}) test::make_dir(root + d);
    const std::vector<std::uint8_t> elf = forge_elf(kExecAslr);
    auto want = fself::make_fself(elf);
    REQUIRE_OK(want);
    test::write_file(root + "/eboot.bin", elf);
    test::write_file(root + "/sce_module/libfoo.prx", *want);           // already a SELF
    test::write_file(root + "/fakelib/libSceFoo.sprx", forge_elf(kDynamic)); // the loader loads these too
    test::write_file(root + "/data/level.bin", forge_elf(kDynamic));    // a data .bin is never opened
    test::write_file(root + "/data/elf.dat", forge_elf(kDynamic));
    test::write_file(root + "/data/a.bin", std::vector<std::uint8_t>(5, 1));
    // The game's own path index, its eboot record in another case.
    auto index = ampr::build({{"EBOOT.BIN", elf.size(), 7}, {"data/a.bin", 5, 7}});
    REQUIRE_OK(index);
    test::write_file(root + "/ampr_emu.index", *index);
    auto folder = tree::open_folder(root);
    REQUIRE_OK(folder);

    auto found = fself::find_unsigned(**folder);
    REQUIRE_OK(found);
    CHECK_EQ(found->size(), 2u);
    auto s = fself::sign(**folder);
    REQUIRE_OK(s);
    REQUIRE_OK(s->tree ? Status{} : Status(make_error(Errc::invalid_data, "nothing signed")));
    CHECK_EQ(s->signed_paths.size(), 2u);
    CHECK(s->refused.empty());
    CHECK_EQ(s->index_records_resized, 1u);
    const tree::Source& t = *s->tree;
    CHECK_EQ(tree::find(t.root(), "eboot.bin")->size, want->size());
    CHECK(read(t, "eboot.bin") == *want);
    CHECK(read(t, "sce_module/libfoo.prx") == *want);
    CHECK(read(t, "data/level.bin") == forge_elf(kDynamic));
    auto module = fself::make_fself(forge_elf(kDynamic));
    REQUIRE_OK(module);
    CHECK(read(t, "fakelib/libSceFoo.sprx") == *module);
    // Windows across the digest, head/zeros/segment, segment/pad/meta/RELRO,
    // each from a fresh tree (the digest is made on the first read reaching it).
    for (auto [at, length] : {std::pair<std::uint64_t, std::size_t>{0x230, 7}, {0x4F0, 0x60}, {0x4860, 0x40}, {0x49B0, 0x35}}) {
        auto fresh = fself::sign(**folder);
        REQUIRE_OK(fresh);
        CHECK(read(*fresh->tree, "eboot.bin", at, length) ==
              std::vector<std::uint8_t>(want->begin() + static_cast<std::ptrdiff_t>(at),
                                        want->begin() + static_cast<std::ptrdiff_t>(at + length)));
    }
    // Past the end: refused, as any file.
    CHECK(read(t, "eboot.bin", want->size() - 4, 9).empty());
    // ampr_emu.index: the eboot record at its SELF size, nothing else changed.
    auto served = read(t, "ampr_emu.index");
    CHECK_EQ(served.size(), index->size());
    auto rebuilt = ampr::build({{"EBOOT.BIN", want->size(), 7}, {"data/a.bin", 5, 7}});
    REQUIRE_OK(rebuilt);
    CHECK(served == *rebuilt);
}

TEST_CASE(fself_in_conversions) {
    auto dir = test::make_temp_dir();
    std::string root = dir + "/game";
    for (const char* d : {"", "/sce_sys"}) test::make_dir(root + d);
    const char* param = "{\"titleId\":\"ABCD99999\",\"contentVersion\":\"01.000.000\"}";
    test::write_file(root + "/sce_sys/param.json", std::vector<std::uint8_t>(param, param + std::strlen(param)));
    std::vector<std::uint8_t> bad = forge_elf(kDynamic);
    put_u16(bad.data() + 0x12, 0xB7);
    test::write_file(root + "/eboot.bin", forge_elf(kExecAslr));
    test::write_file(root + "/odd.prx", bad);
    auto want = fself::make_fself(forge_elf(kExecAslr));
    REQUIRE_OK(want);

    auto report = convert::run(root, dir + "/game.exfat", convert::Format::exfat);
    REQUIRE_OK(report);
    CHECK_EQ(report->fake_signed.size(), 1u);
    CHECK(report->method.rfind("fake-signed the executables and ", 0) == 0);
    CHECK_EQ(report->warnings.size(), 1u); // odd.prx cannot be signed: left as it is
    auto image = convert::open(dir + "/game.exfat");
    REQUIRE_OK(image);
    CHECK(read(*image->tree, "eboot.bin") == *want);
    CHECK(read(*image->tree, "odd.prx") == bad);

    // Off: the files as they are.
    convert::Options off;
    off.fake_sign = false;
    auto plain = convert::run(root, dir + "/plain.exfat", convert::Format::exfat, off);
    REQUIRE_OK(plain);
    CHECK(plain->fake_signed.empty());
    auto plain_image = convert::open(dir + "/plain.exfat");
    REQUIRE_OK(plain_image);
    CHECK(read(*plain_image->tree, "eboot.bin") == forge_elf(kExecAslr));

    // An image of a signed game is copied as it is: nothing left to sign.
    auto again = convert::run(dir + "/game.exfat", dir + "/again.ffpfsc", convert::Format::ffpfsc);
    REQUIRE_OK(again);
    CHECK(again->fake_signed.empty());
}
