// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/ampr_emu.hpp"

#include "alembic/exfat.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <vector>

namespace alembic::ampr_emu {

namespace {

const tree::Node* child_dir(const tree::Node& dir, const char* name) {
    const tree::Node* n = tree::find_child_icase(dir, name);
    return n && n->is_directory ? n : nullptr;
}

// Whether `file` contains `needle`, reading it in large pieces.
Result<bool> contains(const RandomAccess& file, std::string_view needle, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(std::uint64_t size, file.size());
    constexpr std::size_t kChunk = 4 << 20;
    const std::size_t keep = needle.size() - 1; // bytes carried over so a match across pieces is found
    std::vector<std::uint8_t> buffer(kChunk + keep);
    std::size_t carried = 0;
    for (std::uint64_t at = 0; at < size;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(kChunk, size - at));
        ALEMBIC_TRY(file.read_at(at, MutableBytes(buffer.data() + carried, n)));
        at += n;
        std::size_t have = carried + n;
        auto end = buffer.begin() + static_cast<std::ptrdiff_t>(have);
        if (std::search(buffer.begin(), end, needle.begin(), needle.end()) != end) return true;
        carried = std::min(keep, have);
        std::memmove(buffer.data(), buffer.data() + have - carried, carried);
        if (progress && !progress({"Checking eboot.bin", at, size})) return make_error(Errc::cancelled, "cancelled");
    }
    return false;
}

class Memory final : public RandomAccess {
public:
    explicit Memory(const std::vector<std::uint8_t>& data) : data_(data) {}
    Result<std::uint64_t> size() const override { return static_cast<std::uint64_t>(data_.size()); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > data_.size() || out.size() > data_.size() - offset)
            return make_error(Errc::invalid_argument, "read past the end of ampr_emu.index");
        if (!out.empty()) std::memcpy(out.data(), data_.data() + offset, out.size());
        return {};
    }

private:
    const std::vector<std::uint8_t>& data_;
};

// The input tree plus the emulator, and ampr_emu.index when it has none.
class WithEmulator final : public tree::Source {
public:
    WithEmulator(const tree::Source& base, std::shared_ptr<const RandomAccess> emulator)
        : base_(base), emulator_(std::move(emulator)) {}

    Status build(const Check& check, std::int64_t mtime) {
        ALEMBIC_ASSIGN(std::uint64_t size, emulator_->size());
        root_ = copy(base_.root());
        tree::Node* dir = nullptr;
        for (tree::Node& c : root_.children)
            if (c.is_directory && unicode::ascii_iequals(c.name, check.fakelib)) dir = &c;
        if (!dir) {
            tree::Node d;
            d.name = check.fakelib;
            d.is_directory = true;
            d.mtime = mtime;
            root_.children.push_back(std::move(d));
            dir = &root_.children.back();
        }
        std::string name = kFileName;
        for (const tree::Node& c : dir->children)
            if (!c.is_directory && unicode::ascii_iequals(c.name, kFileName)) name = c.name;
        std::erase_if(dir->children, [](const tree::Node& n) { return !n.is_directory && unicode::ascii_iequals(n.name, kFileName); });
        tree::Node file;
        file.name = name;
        file.size = size;
        file.mtime = mtime;
        file.id = kEmulatorId;
        dir->children.push_back(std::move(file));

        // The emulator cannot save an index it builds itself into a read-only
        // image, so one comes with it, as the image writers would make it.
        // An LZ4-packed game keeps the index its packs refer to.
        auto root_file = [&](const char* n) {
            const tree::Node* f = tree::find_child_icase(root_, n);
            return f && !f->is_directory;
        };
        if (!root_file("ampr_emu.index") && !root_file("ampr_assets.index")) {
            ALEMBIC_ASSIGN(index_, exfat::build_ampr_index(*this, root_, ampr::Scheme::ampr_emu));
            if (!index_.empty()) {
                tree::Node node;
                node.name = "ampr_emu.index";
                node.size = index_.size();
                node.mtime = mtime;
                node.id = kIndexId;
                root_.children.push_back(std::move(node));
            }
        }
        return {};
    }

    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return base_.label(); }
    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory) return make_error(Errc::invalid_argument, "not a file");
        if (file.id == kEmulatorId) return emulator_;
        if (file.id == kIndexId)
            return std::shared_ptr<const RandomAccess>(std::make_shared<Memory>(index_));
        if (file.id >= nodes_.size()) return make_error(Errc::invalid_argument, "not a file of this tree");
        return base_.open(*nodes_[file.id]);
    }

private:
    static constexpr std::uint64_t kEmulatorId = UINT64_MAX;
    static constexpr std::uint64_t kIndexId = UINT64_MAX - 1;

    // Our ids index nodes_, which point into the base tree.
    tree::Node copy(const tree::Node& in) {
        tree::Node out;
        out.name = in.name;
        out.is_directory = in.is_directory;
        out.size = in.size;
        out.mtime = in.mtime;
        if (in.is_directory) {
            out.children.reserve(in.children.size() + 1);
            for (const tree::Node& c : in.children) out.children.push_back(copy(c));
        } else {
            nodes_.push_back(&in);
            out.id = nodes_.size() - 1;
        }
        return out;
    }

    const tree::Source& base_;
    std::shared_ptr<const RandomAccess> emulator_;
    tree::Node root_;
    std::vector<const tree::Node*> nodes_;
    std::vector<std::uint8_t> index_;
};

} // namespace

Result<Check> check(const tree::Source& source, const ProgressFn& progress) {
    Check result;
    const tree::Node& root = source.root();
    const tree::Node* dir = child_dir(root, "fakelib2");
    if (!dir) dir = child_dir(root, "fakelib");
    if (dir) {
        result.fakelib = dir->name;
        result.has_fakelib = true;
        const tree::Node* sprx = tree::find_child_icase(*dir, kFileName);
        result.ships_emulator = sprx && !sprx->is_directory;
    }
    const tree::Node* eboot = tree::find_child_icase(root, "eboot.bin");
    if (eboot && !eboot->is_directory) {
        // The library name sits in the executable's dynamic string table,
        // which neither a fake-signed SELF nor a plain ELF encrypts.
        ALEMBIC_ASSIGN(auto file, source.open(*eboot));
        ALEMBIC_ASSIGN(result.imports_ampr, contains(*file, "libSceAmpr", progress));
    }
    return result;
}

Result<std::unique_ptr<tree::Source>> add(const tree::Source& source, const Check& check,
                                          std::shared_ptr<const RandomAccess> emulator, std::int64_t mtime) {
    if (!emulator) return make_error(Errc::invalid_argument, "no emulator file");
    auto tree = std::make_unique<WithEmulator>(source, std::move(emulator));
    ALEMBIC_TRY(tree->build(check, mtime));
    return std::unique_ptr<tree::Source>(std::move(tree));
}

namespace {

// Writes `data` to a new file at `path` (never over an existing one).
Status write_copy(const RandomAccess& data, const std::string& path) {
    ALEMBIC_ASSIGN(std::uint64_t size, data.size());
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, false));
    std::vector<std::uint8_t> buffer(1 << 20);
    for (std::uint64_t at = 0; at < size;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), size - at));
        ALEMBIC_TRY(data.read_at(at, MutableBytes(buffer.data(), n)));
        ALEMBIC_TRY(out.file().write_at(at, Bytes(buffer.data(), n)));
        at += n;
    }
    return out.commit();
}

Status write_bytes(const std::vector<std::uint8_t>& data, const std::string& path) {
    ALEMBIC_ASSIGN(AtomicOutput out, AtomicOutput::create(path, false));
    ALEMBIC_TRY(out.file().write_at(0, Bytes(data.data(), data.size())));
    return out.commit();
}

// A non-owning view, for add().
class Borrowed final : public RandomAccess {
public:
    explicit Borrowed(const RandomAccess& base) : base_(base) {}
    Result<std::uint64_t> size() const override { return base_.size(); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override { return base_.read_at(offset, out); }

private:
    const RandomAccess& base_;
};

bool has_root_file(const tree::Node& root, const char* name) {
    const tree::Node* n = tree::find_child_icase(root, name);
    return n && !n->is_directory;
}

// ampr_emu.index for `game` as it will look with the emulator in place.
Result<std::vector<std::uint8_t>> index_with_emulator(const tree::Source& game, const Check& check,
                                                       const RandomAccess& emulator) {
    ALEMBIC_ASSIGN(auto with, add(game, check, std::make_shared<Borrowed>(emulator), game.root().mtime));
    const tree::Node* node = tree::find_child_icase(with->root(), "ampr_emu.index");
    if (!node || node->is_directory) return make_error(Errc::invalid_argument, "the game has no files to index");
    ALEMBIC_ASSIGN(auto file, with->open(*node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node->size));
    ALEMBIC_TRY(file->read_at(0, MutableBytes(data.data(), data.size())));
    return data;
}

} // namespace

namespace {

Status refuse_ps4(const tree::Source& game) {
    auto root = ffpfsc::resolve_game_root(game.root());
    if (root.ok() && ffpfsc::is_ps4_game(game, **root))
        return make_error(Errc::invalid_argument, "this is a PS4 game: PS4 games do not use APR and need no emulator");
    return {};
}

} // namespace

Result<AttachResult> attach_to_folder(const std::string& folder, const RandomAccess& emulator,
                                      const ProgressFn& progress) {
    ALEMBIC_ASSIGN(auto game, tree::open_folder(folder));
    ALEMBIC_TRY(refuse_ps4(*game));
    AttachResult r;
    ALEMBIC_ASSIGN(r.check, check(*game, progress));
    r.folder = folder;
    if (r.check.ships_emulator) {
        r.already_there = true;
        r.notes.push_back("the game already has " + r.check.fakelib + "/" + kFileName);
        return r;
    }
    if (!r.check.imports_ampr) r.notes.push_back("eboot.bin does not name libSceAmpr; the emulator was added anyway");
    if (progress && !progress({"Writing the emulator", 0, 0})) return make_error(Errc::cancelled, "cancelled");
    std::string rel = r.check.fakelib + "/" + kFileName;
    if (!has_root_file(game->root(), "ampr_emu.index")) {
        ALEMBIC_ASSIGN(std::vector<std::uint8_t> index, index_with_emulator(*game, r.check, emulator));
        ALEMBIC_TRY(write_bytes(index, folder + "/ampr_emu.index"));
        r.written.push_back("ampr_emu.index");
    }
    ALEMBIC_TRY(write_copy(emulator, folder + "/" + rel));
    r.written.push_back(rel);
    return r;
}

Result<AttachResult> attach_backport(const tree::Source& game, const std::string& backports_root,
                                     const RandomAccess& emulator, const ProgressFn& progress) {
    ALEMBIC_TRY(refuse_ps4(game));
    AttachResult r;
    ALEMBIC_ASSIGN(r.check, check(game, progress));
    if (r.check.ships_emulator) {
        r.already_there = true;
        r.notes.push_back("the game already has " + r.check.fakelib + "/" + kFileName);
        return r;
    }
    std::optional<std::string> title_id = ffpfsc::read_title_id(game, game.root());
    if (!title_id) return make_error(Errc::invalid_argument, "the game has no title ID in sce_sys/param.json or param.sfo");
    r.folder = backports_root + "/" + *title_id;
    if (!r.check.imports_ampr) r.notes.push_back("eboot.bin does not name libSceAmpr; the emulator was added anyway");
    if (progress && !progress({"Writing the emulator", 0, 0})) return make_error(Errc::cancelled, "cancelled");

    // An existing backport keeps its own fakelib; the emulator joins it.
    std::string target;
    bool fresh = true;
    for (const char* name : {"fakelib2", "fakelib"})
        if (target.empty() && is_directory(r.folder + "/" + name)) {
            target = name;
            fresh = false;
        }
    if (target.empty()) target = r.check.fakelib;
    std::string rel = target + "/" + kFileName;
    if (path_exists(r.folder + "/" + rel)) {
        r.already_there = true;
        r.notes.push_back("the backport folder already has " + rel);
        return r;
    }
    if (!fresh) {
        r.notes.push_back("added to the existing backport folder's " + target + "/");
    } else if (r.check.has_fakelib) {
        const tree::Node* own = tree::find_child_icase(game.root(), r.check.fakelib);
        for (const tree::Node& f : own->children) {
            if (f.is_directory) {
                r.notes.push_back("skipped the folder " + r.check.fakelib + "/" + f.name);
                continue;
            }
            if (unicode::ascii_iequals(f.name, "libkernel.sprx") || unicode::ascii_iequals(f.name, kFileName) ||
                tree::is_os_junk(f.name))
                continue;
            ALEMBIC_ASSIGN(auto data, game.open(f));
            ALEMBIC_TRY(write_copy(*data, r.folder + "/" + target + "/" + f.name));
            r.written.push_back(target + "/" + f.name);
        }
    }
    if (!has_root_file(game.root(), "ampr_emu.index") && !path_exists(r.folder + "/ampr_emu.index")) {
        ALEMBIC_ASSIGN(std::vector<std::uint8_t> index, index_with_emulator(game, r.check, emulator));
        ALEMBIC_TRY(write_bytes(index, r.folder + "/ampr_emu.index"));
        r.written.push_back("ampr_emu.index");
    }
    ALEMBIC_TRY(write_copy(emulator, r.folder + "/" + rel));
    r.written.push_back(rel);
    return r;
}

} // namespace alembic::ampr_emu
