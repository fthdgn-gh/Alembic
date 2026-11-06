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

// ---- ProsperoPkgTool's flavour (packages). The digests are ProsperoPkgTool
// fself's output; tools/pkg-oracle/compare.sh checks whole packages.

namespace {

// tools/fself-oracle/make_elfs.py's elf(): filler bytes, the header, program headers.
struct Ph {
    std::uint32_t type;
    std::uint64_t offset, size;
};
std::vector<std::uint8_t> py_elf(std::uint16_t type, std::size_t size, std::vector<Ph> phdrs, unsigned fill) {
    std::vector<std::uint8_t> f(size);
    for (std::size_t i = 0; i < size; ++i) f[i] = static_cast<std::uint8_t>(i * fill + 3);
    const std::uint8_t ident[16] = {0x7F, 'E', 'L', 'F', 2, 1, 1, 9, 0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x55};
    std::memcpy(f.data(), ident, 16);
    std::uint8_t* p = f.data();
    put_u16(p + 0x10, type);
    put_u16(p + 0x12, 0x3E);
    put_u32(p + 0x14, 1);
    put_u64(p + 0x18, 0x1234);
    put_u64(p + 0x20, 0x40);
    put_u64(p + 0x28, 0);
    put_u32(p + 0x30, 0);
    const std::uint16_t half[6] = {0x40, 0x38, static_cast<std::uint16_t>(phdrs.size()), 0x40, 0, 2};
    for (int i = 0; i < 6; ++i) put_u16(p + 0x34 + 2 * i, half[i]);
    for (std::size_t i = 0; i < phdrs.size(); ++i) {
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
    return f;
}

std::vector<std::uint8_t> sceversion(const std::string& name, const std::vector<std::uint8_t>& v) {
    std::string n = name + ":";
    std::vector<std::uint8_t> r(5 + n.size());
    put_u16(r.data() + 2, static_cast<std::uint16_t>(1 + n.size() + 16));
    r[4] = 8;
    std::memcpy(r.data() + 5, n.data(), n.size());
    r.insert(r.end(), v.begin(), v.end());
    r.insert(r.end(), v.begin(), v.end());
    return r;
}

// make_elfs.py's with_sections() with one .sceversion section.
std::vector<std::uint8_t> with_sceversion(std::vector<std::uint8_t> f, const std::vector<std::uint8_t>& records, std::uint8_t ex_info) {
    f[0x3F00] = ex_info;
    const char names[] = "\0.shstrtab\0.sceversion\0";
    const std::uint64_t names_at = f.size();
    f.insert(f.end(), names, names + sizeof names - 1);
    const std::uint64_t records_at = f.size();
    f.insert(f.end(), records.begin(), records.end());
    while (f.size() % 8) f.push_back(0);
    const std::uint64_t shoff = f.size();
    f.resize(f.size() + 3 * 0x40, 0);
    auto header = [&](int i, std::uint32_t name, std::uint32_t type, std::uint64_t offset, std::uint64_t size) {
        std::uint8_t* h = f.data() + shoff + i * 0x40;
        put_u32(h, name);
        put_u32(h + 4, type);
        put_u64(h + 0x18, offset);
        put_u64(h + 0x20, size);
        put_u64(h + 0x30, 1);
    };
    header(1, 1, 3, names_at, sizeof names - 1);
    header(2, 11, 1, records_at, records.size());
    put_u64(f.data() + 0x28, shoff);
    put_u16(f.data() + 0x3A, 0x40);
    put_u16(f.data() + 0x3C, 3);
    put_u16(f.data() + 0x3E, 1);
    return f;
}

class Mem final : public RandomAccess {
public:
    explicit Mem(const std::vector<std::uint8_t>& d) : d_(d) {}
    Result<std::uint64_t> size() const override { return d_.size(); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > d_.size() || out.size() > d_.size() - offset) return make_error(Errc::invalid_argument, "past the end");
        std::memcpy(out.data(), d_.data() + offset, out.size());
        return {};
    }

private:
    const std::vector<std::uint8_t>& d_;
};

} // namespace

TEST_CASE(fself_prospero_matches_prosperopkgtool) {
    constexpr std::uint32_t kLoad = 1, kDynlib = 0x61000000, kRelro = 0x61000010, kComment = 0x6FFFFF00, kVersion = 0x6FFFFF01;
    auto module = fself::make_prospero_fself(py_elf(kDynamic, 0x9000,
        {{kLoad, 0x400, 0x3001}, {kDynlib, 0x3500, 0x777}, {kComment, 0x4000, 0x10}, {kLoad, 0x4100, 0},
         {kVersion, 0x4200, 0x30}, {kRelro, 0x4400, 0x200}, {kVersion, 0x4700, 0x18}}, 13));
    REQUIRE_OK(module);
    CHECK_EQ(module->size(), 17008u);
    CHECK(sha256_hex(*module) == "935c3770d79cfd0688f978ce526b04bbbbab68503743f81fbe64ade8c564c935");
    CHECK_EQ(get_u32(module->data()), 0xEEF51454u);

    const std::vector<std::uint8_t> v = {0x04, 0x50, 0x00, 0x31, 0x00, 0x00, 0x04, 0x11};
    std::vector<std::uint8_t> records = sceversion("eboot", v), libc = sceversion("libc", v);
    records.insert(records.end(), libc.begin(), libc.end());
    const std::vector<std::uint8_t> eboot = with_sceversion(
        py_elf(kExecAslr, 0x5000, {{kLoad, 0x1000, 0x2345}, {kRelro, 0x3400, 0x300}, {kDynlib, 0x3800, 0x5A0}}, 17), records, 0x40);
    auto signed_eboot = fself::make_prospero_fself(eboot); // its own .sceversion at the end; authority ...1101
    REQUIRE_OK(signed_eboot);
    CHECK_EQ(signed_eboot->size(), 13077u);
    CHECK(sha256_hex(*signed_eboot) == "7d58783b5e1e05d3089eecce9915ca96ba52f830d962309bd1b4165b1d34a616");

    // The application's record, from the ELF and from its SELF alike.
    auto record = fself::sceversion_record(Mem(eboot));
    REQUIRE_OK(record);
    REQUIRE_OK(*record ? Status{} : Status(make_error(Errc::invalid_data, "no record")));
    CHECK(std::equal(v.begin(), v.end(), (*record)->begin()));
    auto from_self = fself::sceversion_record(Mem(*signed_eboot));
    REQUIRE_OK(from_self);
    CHECK(*from_self == *record);

    // A stripped module: as it is, and with a record made from eboot.bin's.
    const std::vector<std::uint8_t> stripped = py_elf(kDynamic, 0x2000, {{kLoad, 0x400, 0x1234}, {kComment, 0x1800, 0x40}}, 19);
    auto plain = fself::make_prospero_fself(stripped);
    REQUIRE_OK(plain);
    CHECK(sha256_hex(*plain) == "8b5077dac5a0aefd4dc0a74fad17c1788d2015c434cc788dcd55ee56454f75d1");
    auto none = fself::sceversion_record(Mem(stripped));
    REQUIRE_OK(none);
    CHECK(!*none);
    fself::ProsperoOptions o;
    o.sceversion_name = "libstripped";
    o.sceversion_record = *record;
    auto made = fself::make_prospero_fself(stripped, o);
    REQUIRE_OK(made);
    CHECK_EQ(made->size(), 6225u);
    CHECK(sha256_hex(*made) == "41956dc088a7396963eeeba08c347922a1fba83c9325c2d44a7dc0ea743f87d1");

    // Which files img_create signs.
    for (const char* p : {"/eboot.bin", "/EBOOT.BIN", "/x/eboot.bin", "/sce_module/a.prx", "/fakelib/b.SPRX", "/c.elf"})
        CHECK(fself::is_prospero_target(p));
    for (const char* p : {"/eboot2.bin", "/a.self", "/data/level.bin", "/sce_module/a.so"}) CHECK(!fself::is_prospero_target(p));
}
