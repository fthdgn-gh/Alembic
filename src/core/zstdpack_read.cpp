// SPDX-License-Identifier: GPL-3.0-or-later
// Reads zstd packs: verification and a tree that serves the packed files
// again. The checks are the ones quer3q/ps5-dump-forge's reader.rs and
// ampr-emu-ng's runtime make (GPL-3.0): the index, then each volume header
// against it (id, count, archive ID, size), then every chunk.
#include "zstdpack_internal.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

namespace alembic::zstdpack {

namespace {

struct PackSet {
    Index index;
    std::vector<std::shared_ptr<const RandomAccess>> volumes;
    std::set<std::string> artifact_paths; // the index and the volumes, as named in the tree
};

Result<PackSet> open_set(const tree::Source& source) {
    const tree::Node& root = source.root();
    const tree::Node* node = tree::find(root, kIndexName);
    if (!node || node->is_directory) return make_error(Errc::invalid_argument, "the tree has no ampr_zstd.index");
    PackSet set;
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(*node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node->size));
    ALEMBIC_TRY(file->read_at(0, data));
    ALEMBIC_ASSIGN(set.index, parse_index(data));
    set.artifact_paths.insert(kIndexName);
    const std::uint32_t count = static_cast<std::uint32_t>(set.index.volume_sizes.size());
    for (std::uint32_t id = 0; id < count; ++id) {
        std::string name = volume_name(id);
        const tree::Node* v = tree::find(root, name);
        if (!v || v->is_directory) return make_error(Errc::invalid_data, "zstd pack volume " + name + " is missing");
        if (v->size != set.index.volume_sizes[id]) return make_error(Errc::invalid_data, "zstd pack volume size mismatch: " + name);
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> vf, source.open(*v));
        std::uint8_t header[kVolumeHeaderSize];
        ALEMBIC_TRY(vf->read_at(0, MutableBytes(header, kVolumeHeaderSize)));
        ALEMBIC_ASSIGN(VolumeHeader h, parse_volume_header(Bytes(header, kVolumeHeaderSize)));
        if (h.id != id || h.count != count || h.archive_id != set.index.archive_id ||
            h.payload_bytes + kVolumeHeaderSize != v->size)
            return make_error(Errc::invalid_data, "zstd pack volume " + name + " belongs to another archive or place");
        set.volumes.push_back(std::move(vf));
        set.artifact_paths.insert(name);
    }
    return set;
}

// Reads and decodes chunk `k` of file `f` into `raw`.
Status read_chunk(const PackSet& set, const FileRecord& f, std::uint64_t k, Decoder& decoder,
                  std::vector<std::uint8_t>& stored, std::vector<std::uint8_t>& raw) {
    const Chunk& c = set.index.chunks[f.first_chunk + k];
    stored.resize(c.stored_size);
    ALEMBIC_TRY(set.volumes[c.volume]->read_at(c.offset, stored));
    raw.resize(static_cast<std::size_t>(chunk_raw_size(f.size, k)));
    Status s = decoder.decode(c.codec, stored, raw);
    if (!s.ok()) return make_error(Errc::invalid_data, f.path + " chunk " + std::to_string(k) + ": " + s.error().message);
    return {};
}

} // namespace

bool is_packed(const tree::Node& root) {
    const tree::Node* index = tree::find_child_icase(root, kIndexName);
    return index && !index->is_directory;
}

Result<VerifyResult> verify(const tree::Source& packed, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(PackSet set, open_set(packed));
    VerifyResult result;
    result.volumes = static_cast<std::uint32_t>(set.volumes.size());
    std::uint64_t total = 0, done = 0;
    for (const FileRecord& f : set.index.files) total += f.size;
    Decoder decoder;
    std::vector<std::uint8_t> stored, raw;
    for (const FileRecord& f : set.index.files) {
        const std::uint64_t count = chunk_count(f.size);
        for (std::uint64_t k = 0; k < count; ++k) {
            ALEMBIC_TRY(read_chunk(set, f, k, decoder, stored, raw));
            const Chunk& c = set.index.chunks[f.first_chunk + k];
            ++result.chunks;
            if (c.codec == Codec::zstd) ++result.chunks_zstd;
            result.stored_bytes += c.stored_size;
            result.raw_bytes += raw.size();
            done += raw.size();
            if (progress && !progress({"Verifying zstd packs", done, total})) return make_error(Errc::cancelled, "cancelled");
        }
        ++result.files;
    }
    return result;
}

namespace {

// One packed file, decoded on demand (last chunk cached).
class PackedFile final : public RandomAccess {
public:
    PackedFile(std::shared_ptr<const PackSet> set, std::size_t file) : set_(std::move(set)), file_(file) {}
    Result<std::uint64_t> size() const override { return set_->index.files[file_].size; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const FileRecord& f = set_->index.files[file_];
        if (offset > f.size || out.size() > f.size - offset)
            return make_error(Errc::invalid_argument, "read past the end of a packed file");
        std::size_t done = 0;
        while (done < out.size()) {
            std::uint64_t at = offset + done;
            std::uint64_t k = at >> kChunkShift;
            if (k != cached_) {
                ALEMBIC_TRY(read_chunk(*set_, f, k, decoder_, stored_, raw_));
                cached_ = k;
            }
            std::uint64_t within = at - (k << kChunkShift);
            std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(raw_.size() - within, out.size() - done));
            std::memcpy(out.data() + done, raw_.data() + within, n);
            done += n;
        }
        return {};
    }

private:
    std::shared_ptr<const PackSet> set_;
    std::size_t file_;
    mutable std::mutex mutex_;
    mutable std::uint64_t cached_ = UINT64_MAX;
    mutable Decoder decoder_;
    mutable std::vector<std::uint8_t> stored_, raw_;
};

class UnpackedTree final : public tree::Source {
public:
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return packed_->label(); }
    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory || file.id >= entries_.size()) return make_error(Errc::invalid_argument, "not a file of this tree");
        const Entry& e = entries_[file.id];
        if (e.packed_node) return packed_->open(*e.packed_node);
        return std::shared_ptr<const RandomAccess>(std::make_shared<PackedFile>(set_, e.index_file));
    }

    Status build(const tree::Source& packed) {
        packed_ = &packed;
        ALEMBIC_ASSIGN(PackSet set, open_set(packed));
        set_ = std::make_shared<const PackSet>(std::move(set));
        root_.is_directory = true;
        root_.mtime = packed.root().mtime;
        copy(packed.root(), root_, "");
        // The index keeps no times: packed files take the index's.
        const tree::Node* index_node = tree::find(packed.root(), kIndexName);
        const std::int64_t mtime = index_node ? index_node->mtime : root_.mtime;
        const Index& ix = set_->index;
        for (std::size_t i = 0; i < ix.files.size(); ++i) {
            const FileRecord& f = ix.files[i];
            tree::Node* dir = &root_;
            std::size_t at = 0;
            for (;;) {
                std::size_t slash = f.path.find('/', at);
                if (slash == std::string::npos) break;
                std::string part = f.path.substr(at, slash - at);
                tree::Node* next = nullptr;
                for (tree::Node& c : dir->children)
                    if (c.is_directory && c.name == part) next = &c;
                if (!next) {
                    tree::Node d;
                    d.name = part;
                    d.is_directory = true;
                    d.mtime = mtime;
                    dir->children.push_back(std::move(d));
                    next = &dir->children.back();
                }
                dir = next;
                at = slash + 1;
            }
            tree::Node node;
            node.name = f.path.substr(at);
            node.size = f.size;
            node.mtime = mtime;
            node.id = entries_.size();
            entries_.push_back({nullptr, i});
            if (std::any_of(dir->children.begin(), dir->children.end(), [&](const tree::Node& n) { return n.name == node.name; }))
                return make_error(Errc::invalid_data, "a packed file is also a loose one: " + f.path);
            dir->children.push_back(std::move(node));
        }
        return {};
    }

private:
    struct Entry {
        const tree::Node* packed_node;
        std::size_t index_file;
    };
    void copy(const tree::Node& in, tree::Node& out, const std::string& prefix) {
        for (const tree::Node& child : in.children) {
            std::string rel = prefix.empty() ? child.name : prefix + "/" + child.name;
            if (!child.is_directory && set_->artifact_paths.count(rel)) continue;
            tree::Node node;
            node.name = child.name;
            node.is_directory = child.is_directory;
            node.size = child.size;
            node.mtime = child.mtime;
            if (child.is_directory) {
                copy(child, node, rel);
            } else {
                node.id = entries_.size();
                entries_.push_back({&child, 0});
            }
            out.children.push_back(std::move(node));
        }
    }

    const tree::Source* packed_ = nullptr;
    std::shared_ptr<const PackSet> set_;
    tree::Node root_;
    std::vector<Entry> entries_;
};

} // namespace

Result<std::unique_ptr<tree::Source>> unpack(const tree::Source& packed) {
    auto tree = std::make_unique<UnpackedTree>();
    ALEMBIC_TRY(tree->build(packed));
    return std::unique_ptr<tree::Source>(std::move(tree));
}

} // namespace alembic::zstdpack
