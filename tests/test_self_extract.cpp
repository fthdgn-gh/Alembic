// SPDX-License-Identifier: GPL-3.0-or-later
// The portable SELF -> ELF assembler (self_extract.hpp). The kernel segment
// decryptor is PS5-only; here a stand-in callback supplies the plaintext.
#include "alembic/bytes.hpp"
#include "alembic/self_extract.hpp"
#include "alembic/sha256.hpp"
#include "alembic/tree.hpp"
#include "test.hpp"

#include <cstring>
#include <string>

using namespace alembic;

namespace {

constexpr std::uint32_t kSelfPs5Magic = 0xEEF51454;
constexpr std::uint32_t kPtLoad = 1, kPtSceVersion = 0x6FFFFF01;

void put_entry(std::vector<std::uint8_t>& self, std::size_t at, std::uint32_t index, bool has_blocks, bool encrypted,
               std::uint64_t offset) {
    std::uint64_t props = (std::uint64_t{index} << 20) | (std::uint64_t{has_blocks} << 11) | (std::uint64_t{encrypted} << 1);
    put_u64(self.data() + at, props);
    put_u64(self.data() + at + 8, offset);
}

void put_phdr(std::vector<std::uint8_t>& elf, std::size_t at, std::uint32_t type, std::uint64_t offset, std::uint64_t filesz) {
    put_u32(elf.data() + at + 0x00, type);
    put_u64(elf.data() + at + 0x08, offset);  // p_offset
    put_u64(elf.data() + at + 0x10, 0x4000 + offset); // p_vaddr
    put_u64(elf.data() + at + 0x20, filesz);  // p_filesz
    put_u64(elf.data() + at + 0x30, 0x4000);  // p_align
}

// A synthetic SELF with one encrypted segment (bytes from the callback), one
// plaintext segment, and a PT_SCE_VERSION tail. Returns the SELF and the ELF
// it should yield. `good_digest` false stores a wrong digest.
struct Built {
    std::vector<std::uint8_t> self;
    std::vector<std::uint8_t> elf;
    std::vector<std::uint8_t> enc; // the plaintext the callback must return
};
// `trailing_note` adds a PT_NOTE after the version segment with no stored data
// (the eboot shape): it is left zero and must NOT be part of the digest.
Built build_self(bool good_digest = true, bool trailing_note = false) {
    const std::uint16_t num_entries = 3;
    const std::uint16_t phnum = trailing_note ? 4 : 3;
    const std::size_t elf_off = 0x20 + std::size_t{num_entries} * 0x20;
    const std::size_t phoff = 0x40;
    const std::size_t version_end = 0x310;

    // The ELF the SELF should produce.
    std::vector<std::uint8_t> elf(trailing_note ? 0x330 : 0x310, 0);
    std::memcpy(elf.data(), "\x7F""ELF\x02\x01\x01\x09", 8);
    put_u64(elf.data() + 0x20, phoff);
    put_u16(elf.data() + 0x36, 0x38); // e_phentsize
    put_u16(elf.data() + 0x38, phnum);
    put_phdr(elf, phoff + 0 * 0x38, kPtLoad, 0x140, 0x30);       // encrypted (past the phdr table)
    put_phdr(elf, phoff + 1 * 0x38, kPtLoad, 0x200, 0x20);       // plaintext
    put_phdr(elf, phoff + 2 * 0x38, kPtSceVersion, 0x300, 0x10); // version tail
    if (trailing_note) put_phdr(elf, phoff + 3 * 0x38, 0x4 /*PT_NOTE*/, 0x320, 0x10); // no entry: stays zero
    std::vector<std::uint8_t> enc(0x30), plain(0x20), version(0x10);
    for (std::size_t i = 0; i < enc.size(); ++i) enc[i] = static_cast<std::uint8_t>(0xA0 + i);
    for (std::size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<std::uint8_t>(0x50 + i);
    for (std::size_t i = 0; i < version.size(); ++i) version[i] = static_cast<std::uint8_t>(0x10 + i);
    std::memcpy(elf.data() + 0x140, enc.data(), enc.size());
    std::memcpy(elf.data() + 0x200, plain.data(), plain.size());
    std::memcpy(elf.data() + 0x300, version.data(), version.size());

    // The digest covers only up to the end of the last data-bearing segment
    // (the version tail), not the trailing note.
    Sha256 sha;
    sha.update(Bytes(elf.data(), version_end));
    Sha256::Digest digest = sha.finish();

    // Lay out the SELF: header, entries, ELF (ehdr+phdrs), ex-info, stored
    // plaintext, version tail.
    const std::size_t exinfo_at = (elf_off + phoff + std::size_t{phnum} * 0x38 + 0xF) & ~std::size_t{0xF};
    const std::size_t stored_at = exinfo_at + 0x40;
    const std::size_t version_at = stored_at + plain.size();
    std::vector<std::uint8_t> self(version_at + version.size(), 0);
    put_u32(self.data(), kSelfPs5Magic);
    put_u64(self.data() + 0x10, version_at);   // file_size -> version bytes
    put_u16(self.data() + 0x18, num_entries);
    put_entry(self, 0x20 + 0 * 0x20, 0, true, true, 0);          // encrypted, no stored offset
    put_entry(self, 0x20 + 1 * 0x20, 1, true, false, stored_at); // plaintext at stored_at
    put_entry(self, 0x20 + 2 * 0x20, 2, false, false, 0);        // unused (version handled separately)
    std::memcpy(self.data() + elf_off, elf.data(), 0x40 + std::size_t{phnum} * 0x38); // ehdr + phdr table
    std::memcpy(self.data() + exinfo_at + 0x20, digest.data(), 32);
    if (!good_digest) self[exinfo_at + 0x20] ^= 0xFF;
    std::memcpy(self.data() + stored_at, plain.data(), plain.size());
    std::memcpy(self.data() + version_at, version.data(), version.size());

    return {std::move(self), std::move(elf), std::move(enc)};
}

class MemFile final : public RandomAccess {
public:
    explicit MemFile(std::vector<std::uint8_t> d) : d_(std::move(d)) {}
    Result<std::uint64_t> size() const override { return d_.size(); }
    Status read_at(std::uint64_t off, MutableBytes out) const override {
        if (off > d_.size() || out.size() > d_.size() - off) return make_error(Errc::invalid_argument, "past end");
        std::memcpy(out.data(), d_.data() + off, out.size());
        return {};
    }

private:
    std::vector<std::uint8_t> d_;
};

std::vector<std::uint8_t> read_all(const tree::Source& t, const std::string& path) {
    const tree::Node* n = tree::find(t.root(), path);
    if (!n) return {};
    auto f = t.open(*n);
    if (!f.ok()) return {};
    std::vector<std::uint8_t> b(static_cast<std::size_t>(n->size));
    if (!(*f)->read_at(0, b).ok()) return {};
    return b;
}

} // namespace

TEST_CASE(self_extract_assembles_the_elf) {
    Built b = build_self();
    MemFile self(b.self);
    bool asked = false;
    auto decrypt = [&](const std::string&, const self_extract::Segment& seg) -> Result<std::vector<std::uint8_t>> {
        asked = true;
        CHECK_EQ(seg.index, 0u);
        CHECK_EQ(seg.file_size, std::uint64_t{0x30});
        return b.enc;
    };
    auto elf = self_extract::extract_elf(self, "/x/eboot.bin", decrypt);
    REQUIRE_OK(elf);
    CHECK(asked);
    CHECK(*elf == b.elf);
    CHECK(self_extract::is_self(Bytes(b.self.data(), 8)));
    CHECK(!self_extract::is_self(Bytes(b.elf.data(), 8))); // a plain ELF is not a SELF
}

TEST_CASE(self_extract_ignores_a_trailing_no_data_segment) {
    // The eboot shape: a PT_NOTE after the version segment, left zero and not
    // part of the digest. The hash must stop at the version-segment end.
    Built b = build_self(true, true);
    MemFile self(b.self);
    auto decrypt = [&](const std::string&, const self_extract::Segment&) -> Result<std::vector<std::uint8_t>> { return b.enc; };
    auto elf = self_extract::extract_elf(self, "/x/eboot.bin", decrypt);
    REQUIRE_OK(elf);
    CHECK_EQ(elf->size(), b.elf.size()); // full size, note region kept but zero
    CHECK(*elf == b.elf);
    bool tail_zero = true;
    for (std::size_t i = 0x320; i < 0x330; ++i) tail_zero = tail_zero && (*elf)[i] == 0;
    CHECK(tail_zero);
}

TEST_CASE(self_extract_rejects_a_bad_digest) {
    Built b = build_self(false);
    MemFile self(b.self);
    auto decrypt = [&](const std::string&, const self_extract::Segment&) -> Result<std::vector<std::uint8_t>> { return b.enc; };
    CHECK(!self_extract::extract_elf(self, "/x/eboot.bin", decrypt).ok());
    // A decryptor that lies about the size is refused.
    auto wrong = [&](const std::string&, const self_extract::Segment&) -> Result<std::vector<std::uint8_t>> {
        return std::vector<std::uint8_t>(1, 0);
    };
    Built ok = build_self();
    MemFile self2(ok.self);
    CHECK(!self_extract::extract_elf(self2, "/x/eboot.bin", wrong).ok());
    // No decryptor at all: an encrypted SELF cannot be opened.
    CHECK(!self_extract::extract_elf(self2, "/x/eboot.bin", {}).ok());
}

TEST_CASE(self_extract_over_a_folder_tree) {
    auto dir = test::make_temp_dir();
    std::string root = dir + "/game";
    for (const char* d : {"", "/sce_module", "/sce_sys"}) test::make_dir(root + d);
    Built b = build_self();
    test::write_file(root + "/eboot.bin", b.self);                 // an encrypted SELF
    test::write_file(root + "/sce_module/libc.prx", b.self);       // another
    std::vector<std::uint8_t> plain_elf = b.elf;
    test::write_file(root + "/already.elf", plain_elf);            // a plain ELF: left as it is
    const char* j = "{\"titleId\":\"ABCD99999\"}";
    test::write_file(root + "/sce_sys/param.json", std::vector<std::uint8_t>(j, j + std::strlen(j)));

    auto folder = tree::open_folder(root);
    REQUIRE_OK(folder);
    int calls = 0;
    auto decrypt = [&](const std::string& path, const self_extract::Segment&) -> Result<std::vector<std::uint8_t>> {
        ++calls;
        CHECK(!path.empty()); // the real on-disk path
        return b.enc;
    };
    auto d = self_extract::decrypt_executables(**folder, decrypt, false);
    REQUIRE_OK(d);
    REQUIRE_OK(d->tree ? Status{} : Status(make_error(Errc::invalid_data, "nothing decrypted")));
    CHECK_EQ(d->paths.size(), 2u);       // eboot.bin and libc.prx
    CHECK_EQ(calls, 2);                  // one encrypted segment each
    CHECK(read_all(*d->tree, "eboot.bin") == b.elf);
    CHECK(read_all(*d->tree, "sce_module/libc.prx") == b.elf);
    CHECK(read_all(*d->tree, "already.elf") == plain_elf); // unchanged
}
