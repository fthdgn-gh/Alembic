// SPDX-License-Identifier: GPL-3.0-or-later
// Reads LZ4 pack sets: verification (ampr_pack.py verify) and a tree that
// serves the packed files again (ampr_pack.py unpack, streamed). Ported from
// drakmor/ampr_emu's tools/ampr_pack.py (PackReader, verify_packs; GPL-3.0).
#include "lz4pack_internal.hpp"

#include "lz4.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace alembic::lz4pack {

namespace {

using detail::crc;

Result<std::vector<std::uint8_t>> read_all(const tree::Source& source, const tree::Node& node) {
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(node));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(node.size));
    ALEMBIC_TRY(file->read_at(0, data));
    return data;
}

// An opened pack set: manifest, CRCs and volume handles.
struct PackSet {
    Manifest manifest;
    std::vector<std::uint32_t> crcs;
    std::vector<std::shared_ptr<const RandomAccess>> volumes;
    std::set<std::string> artifact_paths; // relative paths of the manifest, sidecars, volumes

    // Reads and decodes chunk `index`, checking size and CRC.
    Status read_chunk(std::size_t index, std::vector<std::uint8_t>& stored, std::vector<std::uint8_t>& raw) const {
        const ChunkRecord& c = manifest.chunks[index];
        stored.resize(c.stored_size);
        ALEMBIC_TRY(volumes[c.pack_id]->read_at(c.offset, stored));
        raw.resize(c.raw_size);
        if (c.codec == kCodecRaw) {
            raw = stored;
        } else {
            int n = LZ4_decompress_safe(reinterpret_cast<const char*>(stored.data()), reinterpret_cast<char*>(raw.data()),
                                        static_cast<int>(stored.size()), static_cast<int>(raw.size()));
            if (n != static_cast<int>(c.raw_size))
                return make_error(Errc::invalid_data, "LZ4 chunk " + std::to_string(index) + " does not decode");
        }
        if (crc(raw) != crcs[index])
            return make_error(Errc::invalid_data, "chunk " + std::to_string(index) + " fails its CRC check");
        return {};
    }
};

Result<PackSet> open_set(const tree::Source& source) {
    const tree::Node& root = source.root();
    const tree::Node* index = tree::find(root, kIndexName);
    if (!index || index->is_directory) return make_error(Errc::invalid_argument, "the tree has no ampr_assets.index");
    PackSet set;
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> data, read_all(source, *index));
    ALEMBIC_ASSIGN(set.manifest, parse_manifest(data));
    set.artifact_paths.insert(kIndexName);
    const std::string crc_name = std::string(kIndexName) + ".crc";
    const tree::Node* crc_node = tree::find(root, crc_name);
    if (!crc_node || crc_node->is_directory)
        return make_error(Errc::invalid_data, "the chunk CRC sidecar " + crc_name + " is missing");
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> crc_data, read_all(source, *crc_node));
    ALEMBIC_ASSIGN(set.crcs, parse_chunk_crcs(crc_data, set.manifest.build_id, set.manifest.chunks.size()));
    set.artifact_paths.insert(crc_name);
    const std::string runtime_name = std::string(kIndexName) + ".runtime";
    if (const tree::Node* rt = tree::find(root, runtime_name); rt && !rt->is_directory) {
        ALEMBIC_ASSIGN(std::vector<std::uint8_t> rt_data, read_all(source, *rt));
        auto runtime = parse_runtime(rt_data, set.manifest.build_id);
        if (!runtime.ok()) return runtime.take_error();
        set.artifact_paths.insert(runtime_name);
    }
    for (std::size_t id = 0; id < set.manifest.packs.size(); ++id) {
        std::string name = set.manifest.pack_name(id);
        const tree::Node* node = tree::find(root, name);
        if (!node || node->is_directory) return make_error(Errc::invalid_data, "pack volume " + name + " is missing");
        const PackRecord& record = set.manifest.packs[id];
        if (node->size != record.file_size) return make_error(Errc::invalid_data, "pack size mismatch: " + name);
        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source.open(*node));
        std::uint8_t header[kDataHeaderSize];
        ALEMBIC_TRY(file->read_at(0, MutableBytes(header, kDataHeaderSize)));
        ALEMBIC_ASSIGN(DataHeader h, parse_data_header(Bytes(header, kDataHeaderSize)));
        if (h.pack_id != id || h.build_id != set.manifest.build_id || h.flags != record.flags)
            return make_error(Errc::invalid_data, "pack volume " + name + " belongs to another build or pack");
        if (h.payload_bytes != record.payload_bytes || h.payload_offset + h.payload_bytes != node->size)
            return make_error(Errc::invalid_data, "pack payload range mismatch: " + name);
        set.volumes.push_back(std::move(file));
        set.artifact_paths.insert(name);
    }
    return set;
}

} // namespace

bool is_packed(const tree::Node& root) {
    const tree::Node* index = tree::find_child_icase(root, kIndexName);
    return index && !index->is_directory;
}

Result<VerifyResult> verify(const tree::Source& packed, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(PackSet set, open_set(packed));
    const Manifest& m = set.manifest;
    VerifyResult result;
    std::map<std::tuple<std::uint16_t, std::uint64_t, std::uint32_t>, std::tuple<std::uint8_t, std::uint32_t, std::uint32_t>>
        seen;
    std::vector<std::uint8_t> stored, raw;
    std::uint64_t total = 0, done = 0;
    for (const FileRecord& f : m.files)
        if (f.flags & kFilePacked) total += f.logical_size;
    for (const FileRecord& f : m.files) {
        if (!(f.flags & kFilePacked)) continue;
        std::uint64_t sum = 0;
        for (std::uint32_t local = 0; local < f.chunk_count; ++local) {
            std::size_t index = static_cast<std::size_t>(f.first_chunk) + local;
            const ChunkRecord& c = m.chunks[index];
            auto key = std::make_tuple(c.pack_id, c.offset, c.stored_size);
            auto descriptor = std::make_tuple(c.codec, c.raw_size, set.crcs[index]);
            auto prior = seen.find(key);
            if (prior != seen.end()) {
                if (prior->second != descriptor) return make_error(Errc::invalid_data, "one physical chunk has conflicting metadata");
            } else {
                ALEMBIC_TRY(set.read_chunk(index, stored, raw));
                seen.emplace(key, descriptor);
                ++result.physical_chunks;
                result.stored_bytes += c.stored_size;
                result.raw_bytes += c.raw_size;
            }
            sum += c.raw_size;
            done += c.raw_size;
            if (progress && !progress({"Verifying LZ4 packs", done, total})) return make_error(Errc::cancelled, "cancelled");
        }
        if (sum != f.logical_size) return make_error(Errc::invalid_data, "logical size mismatch in the manifest");
        ++result.files;
    }
    return result;
}

namespace {

// One packed file, decoded on demand (last chunk cached).
class PackedFile final : public RandomAccess {
public:
    PackedFile(std::shared_ptr<const PackSet> set, std::size_t file) : set_(std::move(set)), file_(file) {}
    Result<std::uint64_t> size() const override { return set_->manifest.files[file_].logical_size; }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        const FileRecord& f = set_->manifest.files[file_];
        if (offset > f.logical_size || out.size() > f.logical_size - offset)
            return make_error(Errc::invalid_argument, "read past the end of a packed file");
        std::size_t done = 0;
        while (done < out.size()) {
            std::uint64_t at = offset + done;
            std::uint64_t local = at >> f.block_shift;
            std::size_t index = static_cast<std::size_t>(f.first_chunk + local);
            if (index != cached_) {
                ALEMBIC_TRY(set_->read_chunk(index, stored_, raw_));
                cached_ = index;
            }
            std::uint64_t within = at - (local << f.block_shift);
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
    mutable std::size_t cached_ = SIZE_MAX;
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
        return std::shared_ptr<const RandomAccess>(std::make_shared<PackedFile>(set_, e.manifest_file));
    }

    Status build(const tree::Source& packed) {
        packed_ = &packed;
        ALEMBIC_ASSIGN(PackSet set, open_set(packed));
        set_ = std::make_shared<const PackSet>(std::move(set));
        root_.is_directory = true;
        root_.mtime = packed.root().mtime;
        copy(packed.root(), root_, "");
        const Manifest& m = set_->manifest;
        for (std::size_t i = 0; i < m.files.size(); ++i) {
            const FileRecord& f = m.files[i];
            if (!(f.flags & kFilePacked)) continue;
            std::string rel = m.file_path(i).substr(6);
            tree::Node* dir = &root_;
            std::size_t at = 0;
            for (;;) {
                std::size_t slash = rel.find('/', at);
                if (slash == std::string::npos) break;
                std::string part = rel.substr(at, slash - at);
                tree::Node* next = nullptr;
                for (tree::Node& c : dir->children)
                    if (c.is_directory && c.name == part) next = &c;
                if (!next) {
                    tree::Node d;
                    d.name = part;
                    d.is_directory = true;
                    d.mtime = f.mtime;
                    dir->children.push_back(std::move(d));
                    next = &dir->children.back();
                }
                dir = next;
                at = slash + 1;
            }
            tree::Node node;
            node.name = rel.substr(at);
            node.size = f.logical_size;
            node.mtime = f.mtime;
            node.id = entries_.size();
            entries_.push_back({nullptr, i});
            std::erase_if(dir->children, [&](const tree::Node& n) { return !n.is_directory && n.name == node.name; });
            dir->children.push_back(std::move(node));
        }
        return {};
    }

private:
    struct Entry {
        const tree::Node* packed_node;
        std::size_t manifest_file;
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

} // namespace alembic::lz4pack
