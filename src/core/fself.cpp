// SPDX-License-Identifier: GPL-3.0-or-later
// Fake-signed SELFs (fself.hpp). Ported from quer3q/ps5-dump-forge's
// ps5-dump-forge-core/src/fself.rs (which executables, the plan from the
// headers, ampr_emu.index's sizes) and the script it matches, ampr-emu-ng's
// tools/make_fself.py (the layout); both GPL-3.0.
#include "alembic/fself.hpp"

#include "alembic/ampr.hpp"
#include "alembic/bytes.hpp"
#include "alembic/sha256.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace alembic::fself {

namespace {

constexpr std::uint32_t kElfMagic = 0x464C457F; // "\x7FELF"
constexpr std::uint16_t kExecAslr = 0xFE10;
constexpr std::uint16_t kDynamic = 0xFE18;
constexpr std::uint32_t kPtLoad = 1;
constexpr std::uint32_t kPtDynlibData = 0x61000000;
constexpr std::uint32_t kPtRelro = 0x61000010;
constexpr std::uint32_t kPtComment = 0x6FFFFF00;
constexpr std::uint32_t kPtVersion = 0x6FFFFF01;
constexpr std::uint64_t kPhdr = 0x38;
constexpr std::uint64_t kBlock = 0x4000;              // a data entry's block, one zero digest each
constexpr std::uint64_t kPaid = 0x3100000000000002ull; // make_fself's program authority ID
constexpr std::uint64_t kPtypeFake = 1;
constexpr std::uint64_t kHead = 0x4000;   // the first read: the ELF header and, as built, its program headers
constexpr std::size_t kMaxExecutables = 4096;
constexpr std::uint64_t kMaxIndex = 64ull << 20;

bool is_copied(std::uint32_t type) {
    return type == kPtLoad || type == kPtRelro || type == kPtDynlibData || type == kPtComment;
}

std::uint64_t align16(std::uint64_t x) { return (x + 15) & ~std::uint64_t{15}; }

Error refuse(std::string why) { return make_error(Errc::unsupported, std::move(why)); }

// One SELF: `head` holds bytes [0, head.size()) (headers, meta, signature),
// `copies` the ELF ranges placed after it, zeros everywhere else.
struct Plan {
    struct Copy {
        std::uint64_t at, from, length; // SELF offset, ELF offset, bytes
    };
    std::uint64_t elf_size = 0;
    std::uint64_t size = 0;
    std::vector<std::uint8_t> head;
    std::size_t digest_at = 0; // the ex-info digest's place in head
    std::vector<Copy> copies;
    std::mutex mutex;          // guards head until the digest is in
    bool digest_done = false;
};

Status read_exact(const RandomAccess& file, std::uint64_t offset, MutableBytes out, std::uint64_t file_size) {
    if (offset > file_size || out.size() > file_size - offset) return make_error(Errc::invalid_data, "the ELF changed while it was signed");
    return file.read_at(offset, out);
}

Result<std::unique_ptr<Plan>> make_plan(const RandomAccess& elf, std::uint64_t elf_size) {
    std::vector<std::uint8_t> first(static_cast<std::size_t>(std::min(elf_size, kHead)));
    ALEMBIC_TRY(read_exact(elf, 0, first, elf_size));
    if (first.size() < 0x40 || !is_unsigned_elf(first)) return refuse("not a whole PS5 ELF header");
    const std::uint8_t* h = first.data();
    // make_fself's own refusals.
    if (h[4] != 2 || h[5] != 1) return refuse("not a 64-bit little-endian ELF");
    if (get_u16(h + 0x12) != 0x3E || get_u32(h + 0x14) != 1) return refuse("not an x86-64 ELF version 1");
    const std::uint16_t shentsize = get_u16(h + 0x3A);
    if (get_u16(h + 0x36) != kPhdr || !(shentsize == 0 || shentsize == 0x40)) return refuse("unsupported header entry size");
    const std::uint64_t phoff = get_u64(h + 0x20);
    const std::uint64_t ehsize = get_u16(h + 0x34);
    const std::uint64_t phnum = get_u16(h + 0x38);
    if (phoff > UINT64_MAX - phnum * kPhdr) return refuse("headers too large for a SELF");
    std::vector<std::uint8_t> table(static_cast<std::size_t>(phnum * kPhdr));
    if (phnum) {
        if (phoff + table.size() <= first.size()) std::memcpy(table.data(), h + phoff, table.size());
        else if (phoff > elf_size || table.size() > elf_size - phoff) return refuse("program headers past the end of the file");
        else ALEMBIC_TRY(elf.read_at(phoff, table));
    }
    struct Phdr {
        std::uint64_t index;
        std::uint32_t type;
        std::uint64_t offset, filesz;
    };
    std::vector<Phdr> copied;
    std::optional<Phdr> version;
    for (std::uint64_t i = 0; i < phnum; ++i) {
        const std::uint8_t* p = table.data() + i * kPhdr;
        Phdr ph{i, get_u32(p), get_u64(p + 8), get_u64(p + 0x20)};
        if (is_copied(ph.type)) copied.push_back(ph);
        if (ph.type == kPtVersion) version = ph; // the last one, as the script keeps
    }
    auto past_end = [&](const Phdr& p) { return p.filesz > 0 && (p.offset > elf_size || p.filesz > elf_size - p.offset); };
    for (const Phdr& p : copied)
        if (past_end(p)) return refuse("segment " + std::to_string(p.index) + " runs past the end of the file");
    if (version && past_end(*version)) return refuse("segment " + std::to_string(version->index) + " runs past the end of the file");

    const std::uint64_t n = copied.size() * 2;
    const std::uint64_t span = std::max(phoff + phnum * kPhdr, ehsize);
    if (span > 0xFFFF) return refuse("headers too large for a SELF");
    const std::uint64_t elf_at = 0x20 + 0x20 * n;
    const std::uint64_t elf_hs = align16(span);
    const std::uint64_t header_size = elf_hs + elf_at + 0x40 + 0x30;
    const std::uint64_t meta_size = 0x50 * n + 0x50 + 0x100;
    if (n > 0xFFFF || header_size > 0xFFFF || meta_size > 0xFFFF) return refuse("headers too large for a SELF");

    auto plan = std::make_unique<Plan>();
    plan->elf_size = elf_size;
    plan->head.assign(static_cast<std::size_t>(header_size + meta_size), 0);
    std::uint8_t* out = plan->head.data();

    // Entries: a meta entry of zero digests, then the segment's bytes, each 16-aligned.
    std::uint64_t offset = header_size + meta_size;
    for (std::size_t j = 0; j < copied.size(); ++j) {
        const Phdr& p = copied[j];
        const std::uint64_t k = 2 * j;
        const std::uint64_t digests = div_round_up(p.filesz, kBlock) * 0x20;
        const std::uint64_t meta = (1u << 2) | (1u << 16) | ((k + 1) << 20);
        const std::uint64_t data = (1u << 2) | (1u << 11) | (2u << 12) | (p.index << 20);
        auto entry = [&](std::uint64_t at, std::uint64_t props, std::uint64_t where, std::uint64_t length) {
            put_u64(out + at, props);
            put_u64(out + at + 8, where);
            put_u64(out + at + 16, length);
            put_u64(out + at + 24, length);
        };
        entry(0x20 + 0x20 * k, meta, offset, digests);
        offset = align16(offset + digests);
        entry(0x20 + 0x20 * (k + 1), data, offset, p.filesz);
        plan->copies.push_back({offset, p.offset, p.filesz});
        offset = align16(offset + p.filesz);
    }
    const std::uint64_t file_size = offset;
    // Common and extended header.
    const std::uint8_t common[8] = {0x4F, 0x15, 0x3D, 0x1D, 0x00, 0x01, 0x01, 0x12};
    std::memcpy(out, common, sizeof common);
    put_u32(out + 8, 0x101);
    put_u16(out + 0xC, static_cast<std::uint16_t>(header_size));
    put_u16(out + 0xE, static_cast<std::uint16_t>(meta_size));
    put_u64(out + 0x10, file_size);
    put_u16(out + 0x18, static_cast<std::uint16_t>(n));
    put_u16(out + 0x1A, 0x22);
    // The ELF header as the script saves it: e_ident[6] is e_version,
    // [9, 15) zero, e_shnum 0 (section headers ignored); then each program
    // header at its offset (written in this order, as the script does).
    std::uint8_t ehdr[0x40];
    std::memcpy(ehdr, h, sizeof ehdr);
    ehdr[6] = 1;
    std::memset(ehdr + 9, 0, 6);
    ehdr[0x3C] = ehdr[0x3D] = 0;
    std::memcpy(out + elf_at, ehdr, sizeof ehdr);
    if (phnum) std::memcpy(out + elf_at + phoff, table.data(), table.size());
    // Ex-info (the digest later), NPDRM block type 3; meta blocks, footer, signature.
    const std::uint64_t info = elf_at + elf_hs;
    put_u64(out + info, kPaid);
    put_u64(out + info + 8, kPtypeFake);
    put_u64(out + info + 16, 0);
    put_u64(out + info + 24, 0);
    put_u16(out + info + 0x40, 3);
    const std::uint64_t footer = info + 0x70 + 0x50 * n;
    put_u32(out + footer + 48, 0x10000);

    plan->digest_at = static_cast<std::size_t>(info + 0x20);
    plan->size = file_size;
    if (version) {
        plan->copies.push_back({file_size, version->offset, version->filesz});
        plan->size = file_size + version->filesz;
    }
    return plan;
}

// SHA-256 of the whole ELF, for the ex-info.
Status fill_digest(Plan& plan, const RandomAccess& elf) {
    Sha256 hash;
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(std::min<std::uint64_t>(plan.elf_size, 8u << 20)));
    for (std::uint64_t at = 0; at < plan.elf_size;) {
        const std::size_t length = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), plan.elf_size - at));
        ALEMBIC_TRY(read_exact(elf, at, MutableBytes(buffer.data(), length), plan.elf_size));
        hash.update(Bytes(buffer.data(), length));
        at += length;
    }
    Sha256::Digest d = hash.finish();
    std::memcpy(plan.head.data() + plan.digest_at, d.data(), d.size());
    plan.digest_done = true;
    return {};
}

class SelfFile final : public RandomAccess {
public:
    SelfFile(std::shared_ptr<Plan> plan, std::shared_ptr<const RandomAccess> elf) : plan_(std::move(plan)), elf_(std::move(elf)) {}
    Result<std::uint64_t> size() const override { return plan_->size; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        Plan& p = *plan_;
        if (offset > p.size || out.size() > p.size - offset) return make_error(Errc::invalid_argument, "read past the end of a SELF");
        const std::uint64_t start = offset, end = offset + out.size();
        std::memset(out.data(), 0, out.size());
        const std::uint64_t head = p.head.size();
        if (start < head) {
            std::lock_guard<std::mutex> lock(p.mutex);
            if (!p.digest_done && start < p.digest_at + 32 && p.digest_at < end) ALEMBIC_TRY(fill_digest(p, *elf_));
            const std::uint64_t to = std::min(end, head);
            std::memcpy(out.data(), p.head.data() + start, static_cast<std::size_t>(to - start));
        }
        for (const Plan::Copy& c : p.copies) {
            const std::uint64_t s = std::max(start, c.at), e = std::min(end, c.at + c.length);
            if (s < e)
                ALEMBIC_TRY(read_exact(*elf_, c.from + (s - c.at),
                                       MutableBytes(out.data() + (s - start), static_cast<std::size_t>(e - s)), p.elf_size));
        }
        return {};
    }

private:
    std::shared_ptr<Plan> plan_;
    std::shared_ptr<const RandomAccess> elf_;
};

class MemoryFile final : public RandomAccess {
public:
    explicit MemoryFile(Bytes data) : data_(data) {}
    explicit MemoryFile(std::shared_ptr<const std::vector<std::uint8_t>> owned)
        : owned_(std::move(owned)), data_(owned_->data(), owned_->size()) {}
    Result<std::uint64_t> size() const override { return static_cast<std::uint64_t>(data_.size()); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > data_.size() || out.size() > data_.size() - offset) return make_error(Errc::invalid_argument, "read past the end");
        if (!out.empty()) std::memcpy(out.data(), data_.data() + offset, out.size());
        return {};
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> owned_;
    Bytes data_;
};

std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }

void walk(const tree::Node& dir, const std::string& prefix, const std::function<void(const tree::Node&, const std::string&)>& fn) {
    for (const tree::Node& c : dir.children) {
        std::string rel = join(prefix, c.name);
        if (c.is_directory) walk(c, rel, fn);
        else fn(c, rel);
    }
}

// The input tree with signed files at their SELF sizes and, when it lists
// any of them, ampr_emu.index with those records resized.
class SignedTree final : public tree::Source {
public:
    SignedTree(const tree::Source& base, std::map<std::string, std::shared_ptr<Plan>> plans,
               std::shared_ptr<const std::vector<std::uint8_t>> index, const tree::Node* index_node)
        : base_(base), plans_(std::move(plans)), index_(std::move(index)), index_node_(index_node) {
        root_ = copy(base_.root(), "");
    }
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return base_.label(); }
    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory || file.id >= entries_.size()) return make_error(Errc::invalid_argument, "not a file of this tree");
        const Entry& e = entries_[file.id];
        if (e.node == index_node_ && index_) return std::shared_ptr<const RandomAccess>(std::make_shared<MemoryFile>(index_));
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> raw, base_.open(*e.node));
        if (!e.plan) return raw;
        return std::shared_ptr<const RandomAccess>(std::make_shared<SelfFile>(e.plan, std::move(raw)));
    }

private:
    struct Entry {
        const tree::Node* node;
        std::shared_ptr<Plan> plan;
    };
    tree::Node copy(const tree::Node& in, const std::string& rel) {
        tree::Node out;
        out.name = in.name;
        out.is_directory = in.is_directory;
        out.size = in.size;
        out.mtime = in.mtime;
        if (in.is_directory) {
            out.children.reserve(in.children.size());
            for (const tree::Node& c : in.children) out.children.push_back(copy(c, join(rel, c.name)));
            return out;
        }
        Entry e{&in, nullptr};
        if (auto it = plans_.find(rel); it != plans_.end()) {
            e.plan = it->second;
            out.size = e.plan->size;
        }
        if (&in == index_node_ && index_) out.size = index_->size();
        entries_.push_back(std::move(e));
        out.id = entries_.size() - 1;
        return out;
    }

    const tree::Source& base_;
    std::map<std::string, std::shared_ptr<Plan>> plans_;
    std::shared_ptr<const std::vector<std::uint8_t>> index_;
    const tree::Node* index_node_;
    tree::Node root_;
    std::vector<Entry> entries_;
};

// ampr_emu.index with each record of a signed file that lists its ELF size
// set to its SELF size (a plain u64 at +8, no checksum), and the count; no
// bytes when it does not read or lists none of them.
Result<std::pair<std::vector<std::uint8_t>, std::size_t>> resized_index(const tree::Source& source, const tree::Node& node,
                                                                        const std::map<std::string, std::shared_ptr<Plan>>& plans) {
    std::pair<std::vector<std::uint8_t>, std::size_t> none;
    if (node.size > kMaxIndex || plans.empty()) return none;
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(node));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(node.size));
    ALEMBIC_TRY(file->read_at(0, bytes));
    auto info = ampr::inspect(bytes);
    if (!info.ok()) return none;
    const std::size_t paths = 48 + std::size_t{info->record_count} * 24;
    std::size_t resized = 0;
    for (std::size_t i = 0; i < info->record_count; ++i) {
        std::uint8_t* record = bytes.data() + 48 + i * 24;
        std::string_view path(reinterpret_cast<const char*>(bytes.data() + paths + get_u32(record)), get_u32(record + 4));
        if (path.substr(0, 6) != "/app0/") continue;
        path.remove_prefix(6);
        // The runtime folds ASCII case.
        for (const auto& [rel, plan] : plans)
            if (unicode::ascii_iequals(rel, path)) {
                if (get_u64(record + 8) == plan->elf_size) {
                    put_u64(record + 8, plan->size);
                    ++resized;
                }
                break;
            }
    }
    if (!resized) return none;
    return std::pair{std::move(bytes), resized};
}

} // namespace

bool is_executable_name(std::string_view path) {
    std::size_t slash = path.rfind('/');
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    std::size_t dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    std::string_view ext = name.substr(dot + 1);
    for (std::string_view e : {"elf", "self", "prx", "sprx"})
        if (unicode::ascii_iequals(ext, e)) return true;
    return unicode::ascii_iequals(ext, "bin") && name.size() >= 5 && unicode::ascii_iequals(name.substr(0, 5), "eboot");
}

bool is_unsigned_elf(Bytes head) {
    if (head.size() < 0x12 || get_u32(head.data()) != kElfMagic) return false;
    const std::uint16_t type = get_u16(head.data() + 0x10);
    return type == kExecAslr || type == kDynamic;
}

Result<std::vector<std::string>> find_unsigned(const tree::Source& source) {
    std::vector<std::pair<const tree::Node*, std::string>> candidates;
    walk(source.root(), "", [&](const tree::Node& n, const std::string& rel) {
        if (n.size >= 0x40 && is_executable_name(rel)) candidates.emplace_back(&n, rel);
    });
    std::vector<std::string> found;
    for (const auto& [node, rel] : candidates) {
        if (found.size() >= kMaxExecutables) break;
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(*node));
        std::uint8_t head[0x12];
        if (!file->read_at(0, MutableBytes(head, sizeof head)).ok()) continue;
        if (is_unsigned_elf(Bytes(head, sizeof head))) found.push_back(rel);
    }
    return found;
}

Result<std::vector<std::uint8_t>> make_fself(Bytes elf) {
    auto file = std::make_shared<MemoryFile>(elf);
    ALEMBIC_ASSIGN(std::unique_ptr<Plan> plan, make_plan(*file, elf.size()));
    std::shared_ptr<Plan> shared(std::move(plan));
    SelfFile self(shared, file);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(shared->size));
    ALEMBIC_TRY(self.read_at(0, out));
    return out;
}

Result<Signed> sign(const tree::Source& source) {
    Signed result;
    ALEMBIC_ASSIGN(std::vector<std::string> paths, find_unsigned(source));
    std::map<std::string, std::shared_ptr<Plan>> plans;
    for (const std::string& rel : paths) {
        const tree::Node* node = tree::find(source.root(), rel);
        if (!node) continue;
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(*node));
        auto plan = make_plan(*file, node->size);
        if (!plan.ok()) {
            if (plan.error().code != Errc::unsupported) return plan.error();
            result.refused.emplace_back(rel, plan.error().message);
            continue;
        }
        plans.emplace(rel, std::shared_ptr<Plan>(std::move(*plan)));
        result.signed_paths.push_back(rel);
    }
    if (plans.empty()) return result;
    std::shared_ptr<const std::vector<std::uint8_t>> index;
    const tree::Node* index_node = tree::find(source.root(), "ampr_emu.index");
    if (index_node && !index_node->is_directory) {
        ALEMBIC_ASSIGN(auto resized, resized_index(source, *index_node, plans));
        if (resized.second) {
            index = std::make_shared<const std::vector<std::uint8_t>>(std::move(resized.first));
            result.index_records_resized = resized.second;
        }
    }
    result.tree = std::make_unique<SignedTree>(source, std::move(plans), std::move(index), index_node);
    return result;
}

} // namespace alembic::fself
