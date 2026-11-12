// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from ps5-payload-dev/ftpsrv self.c (GPL-3.0, John Törnblom; derived
// from idlesauce/ps5-self-pager). See self_extract.hpp.
#include "alembic/self_extract.hpp"

#include "alembic/bytes.hpp"
#include "alembic/fself.hpp"
#include "alembic/sha256.hpp"

#include <algorithm>
#include <cstring>
#include <map>

namespace alembic::self_extract {

namespace {

constexpr std::uint32_t kSelfPs4Magic = 0x1D3D154F;
constexpr std::uint32_t kSelfPs5Magic = 0xEEF51454;
constexpr std::size_t kHeaderSize = 0x20;  // self_head_t
constexpr std::size_t kEntrySize = 0x20;   // self_entry_t
constexpr std::size_t kEhdrSize = 0x40;    // Elf64_Ehdr
constexpr std::size_t kPhdrSize = 0x38;    // Elf64_Phdr
constexpr std::size_t kExinfoSize = 0x40;  // self_exinfo_t (digest at +0x20)
constexpr std::uint32_t kPtSceVersion = 0x6FFFFF01;
constexpr std::uint64_t kMaxElf = 512ull << 20; // a single executable

Error bad(const std::string& why) { return make_error(Errc::invalid_data, "SELF: " + why); }

// A SELF segment entry (0x20 bytes): a packed little-endian bitfield word,
// then offset, enc_size, dec_size. Only these fields are needed.
struct Entry {
    std::uint32_t segment_index;
    bool has_blocks;
    bool is_encrypted;
    bool is_compressed;
    std::uint64_t offset;
};
Entry read_entry(const std::uint8_t* p) {
    const std::uint64_t props = get_u64(p);
    Entry e;
    e.is_encrypted = (props >> 1) & 1;
    e.is_compressed = (props >> 3) & 1;
    e.has_blocks = (props >> 11) & 1;
    e.segment_index = static_cast<std::uint32_t>((props >> 20) & 0xFFFF);
    e.offset = get_u64(p + 8);
    return e;
}

Status read_exact(const RandomAccess& file, std::uint64_t at, std::uint8_t* out, std::size_t n) {
    return file.read_at(at, MutableBytes(out, n));
}

} // namespace

bool is_self(Bytes head) {
    if (head.size() < 4) return false;
    const std::uint32_t magic = get_u32(head.data());
    return magic == kSelfPs4Magic || magic == kSelfPs5Magic;
}

Result<std::vector<std::uint8_t>> extract_elf(const RandomAccess& self, const std::string& self_path, const Decryptor& decrypt) {
    ALEMBIC_ASSIGN(std::uint64_t self_size, self.size());
    if (self_size < kHeaderSize) return bad("file is too small");
    std::uint8_t head[kHeaderSize];
    ALEMBIC_TRY(read_exact(self, 0, head, sizeof head));
    if (!is_self(Bytes(head, sizeof head))) return bad("bad magic");
    const std::uint64_t self_file_size = get_u64(head + 0x10); // where PT_SCE_VERSION bytes sit
    const std::uint16_t num_entries = get_u16(head + 0x18);
    const std::uint64_t entries_at = kHeaderSize;
    const std::uint64_t elf_off = entries_at + std::uint64_t{num_entries} * kEntrySize;
    if (num_entries > 256 || elf_off + kEhdrSize > self_size) return bad("entry table out of range");

    std::vector<std::uint8_t> entry_bytes(std::size_t{num_entries} * kEntrySize);
    ALEMBIC_TRY(read_exact(self, entries_at, entry_bytes.data(), entry_bytes.size()));

    std::uint8_t ehdr[kEhdrSize];
    ALEMBIC_TRY(read_exact(self, elf_off, ehdr, sizeof ehdr));
    if (std::memcmp(ehdr, "\x7F""ELF", 4) != 0) return bad("embedded file is not an ELF");
    const std::uint64_t phoff = get_u64(ehdr + 0x20);
    const std::uint16_t phentsize = get_u16(ehdr + 0x36);
    const std::uint16_t phnum = get_u16(ehdr + 0x38);
    if (phentsize != kPhdrSize || phnum == 0 || phnum > 256) return bad("bad program header table");
    if (elf_off + phoff + std::uint64_t{phnum} * kPhdrSize > self_size) return bad("program headers out of range");

    // Read every program header and work out the ELF's size.
    std::vector<std::uint8_t> phdrs(std::size_t{phnum} * kPhdrSize);
    ALEMBIC_TRY(read_exact(self, elf_off + phoff, phdrs.data(), phdrs.size()));
    std::uint64_t elf_size = kEhdrSize;
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::uint8_t* ph = phdrs.data() + std::size_t{i} * kPhdrSize;
        const std::uint64_t end = get_u64(ph + 0x08) + get_u64(ph + 0x20); // p_offset + p_filesz
        elf_size = std::max(elf_size, std::max(end, phoff + std::uint64_t{phnum} * kPhdrSize));
    }
    if (elf_size > kMaxElf) return bad("embedded ELF is too large");

    std::vector<std::uint8_t> out(static_cast<std::size_t>(elf_size), 0);
    std::memcpy(out.data(), ehdr, kEhdrSize);
    // The program header table (a PT_LOAD usually also covers it; writing it
    // here keeps the ELF correct when none does, and matches the bytes a
    // covering segment would carry, so the digest still checks out).
    std::memcpy(out.data() + phoff, phdrs.data(), phdrs.size());
    // The ex-info digest covers the ELF only up to the end of the last segment
    // that has stored data (a block entry or PT_SCE_VERSION); a trailing
    // segment with no data of its own, such as a PT_NOTE after the version
    // segment, is left zero and not part of the hash.
    std::uint64_t data_end = phoff + std::uint64_t{phnum} * kPhdrSize;

    // Copy or decrypt each segment into the output ELF.
    for (std::uint16_t i = 0; i < phnum; ++i) {
        const std::uint8_t* ph = phdrs.data() + std::size_t{i} * kPhdrSize;
        const std::uint32_t p_type = get_u32(ph + 0x00);
        const std::uint64_t p_offset = get_u64(ph + 0x08);
        const std::uint64_t p_vaddr = get_u64(ph + 0x10);
        const std::uint64_t p_filesz = get_u64(ph + 0x20);
        const std::uint64_t p_align = get_u64(ph + 0x30);
        if (p_filesz == 0) continue;
        if (p_offset + p_filesz > out.size()) return bad("a segment runs past the ELF");

        if (p_type == kPtSceVersion) {
            // Its bytes are appended at the end of the SELF file.
            if (self_file_size + p_filesz > self_size) continue; // some fake SELFs omit it
            ALEMBIC_TRY(read_exact(self, self_file_size, out.data() + p_offset, static_cast<std::size_t>(p_filesz)));
            data_end = std::max(data_end, p_offset + p_filesz);
            continue;
        }
        // The entry that describes this segment's stored data.
        const Entry* seg = nullptr;
        Entry held;
        for (std::uint16_t j = 0; j < num_entries; ++j) {
            Entry e = read_entry(entry_bytes.data() + std::size_t{j} * kEntrySize);
            if (e.segment_index == i && e.has_blocks) { held = e; seg = &held; break; }
        }
        if (!seg) continue; // no stored data for this header
        data_end = std::max(data_end, p_offset + p_filesz);
        if (seg->is_encrypted || seg->is_compressed) {
            if (!decrypt) return make_error(Errc::unsupported, "this SELF is encrypted; decryption is only available on the PS5");
            Segment req{i, p_filesz, p_vaddr, p_align};
            ALEMBIC_ASSIGN(std::vector<std::uint8_t> plain, decrypt(self_path, req));
            if (plain.size() != p_filesz) return bad("the decryptor returned the wrong number of bytes");
            std::memcpy(out.data() + p_offset, plain.data(), static_cast<std::size_t>(p_filesz));
        } else {
            if (seg->offset + p_filesz > self_size) return bad("a stored segment runs past the file");
            ALEMBIC_TRY(read_exact(self, seg->offset, out.data() + p_offset, static_cast<std::size_t>(p_filesz)));
        }
    }

    // The ex-info (0x10-aligned, right after the program headers) holds the
    // expected SHA-256 of the whole ELF.
    const std::uint64_t exinfo_at = (elf_off + phoff + std::uint64_t{phnum} * kPhdrSize + 0xF) & ~std::uint64_t{0xF};
    if (exinfo_at + kExinfoSize <= self_size) {
        std::uint8_t exinfo[kExinfoSize];
        ALEMBIC_TRY(read_exact(self, exinfo_at, exinfo, sizeof exinfo));
        Sha256 sha;
        sha.update(Bytes(out.data(), static_cast<std::size_t>(std::min<std::uint64_t>(data_end, out.size()))));
        Sha256::Digest digest = sha.finish();
        if (std::memcmp(digest.data(), exinfo + 0x20, 32) != 0)
            return bad("the decrypted ELF does not match the SELF's digest");
    }
    return out;
}

namespace {

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

// `base` with some files' bytes replaced (tree paths, '/'-separated).
class DecryptedTree final : public tree::Source {
public:
    using Contents = std::map<std::string, std::shared_ptr<const std::vector<std::uint8_t>>>;
    DecryptedTree(const tree::Source& base, Contents contents) : base_(base), contents_(std::move(contents)) {
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

void walk(const tree::Node& dir, const std::string& prefix, const std::function<void(const tree::Node&, const std::string&)>& fn) {
    for (const tree::Node& c : dir.children) {
        std::string rel = prefix.empty() ? c.name : prefix + "/" + c.name;
        if (c.is_directory) walk(c, rel, fn);
        else fn(c, rel);
    }
}

} // namespace

Result<Decrypted> decrypt_executables(const tree::Source& source, const Decryptor& decrypt, bool game_sign, const ProgressFn& progress) {
    Decrypted result;
    std::vector<std::pair<const tree::Node*, std::string>> executables;
    walk(source.root(), "", [&](const tree::Node& n, const std::string& rel) {
        if (n.size >= kHeaderSize && fself::is_executable_name(rel)) executables.emplace_back(&n, rel);
    });

    DecryptedTree::Contents contents;
    for (const auto& [node, rel] : executables) {
        if (progress && !progress({"Decrypting executables", 0, 0})) return make_error(Errc::cancelled, "cancelled");
        std::string path = source.real_path(*node);
        if (path.empty()) { result.skipped.emplace_back(rel, "only a folder's files can be decrypted"); continue; }
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(*node));
        std::uint8_t head[4];
        if (!file->read_at(0, MutableBytes(head, sizeof head)).ok()) continue;
        if (!is_self(Bytes(head, sizeof head))) continue; // already a plain ELF or not one
        auto elf = extract_elf(*file, path, decrypt);
        if (!elf.ok()) { result.skipped.emplace_back(rel, elf.error().message); continue; }
        std::vector<std::uint8_t> bytes = std::move(*elf);
        if (game_sign) {
            // Fake-sign it so the console will launch it, with the PS5 SELF
            // magic (0xEEF51454) a game process needs -- make_fself.py's fake
            // magic (0x1D3D154F) is for standalone homebrew and the console
            // does not start a game from it. Same format otherwise, as
            // ps5-app-dumper's elf2fself writes it.
            auto signed_elf = fself::make_fself(Bytes(bytes.data(), bytes.size()), /*ps5_magic=*/true);
            if (!signed_elf.ok()) { result.skipped.emplace_back(rel, "fake-sign: " + signed_elf.error().message); continue; }
            bytes = std::move(*signed_elf);
        }
        contents[rel] = std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes));
        result.paths.push_back(rel);
    }
    if (contents.empty()) return result;
    result.tree = std::make_unique<DecryptedTree>(source, std::move(contents));
    return result;
}

} // namespace alembic::self_extract
