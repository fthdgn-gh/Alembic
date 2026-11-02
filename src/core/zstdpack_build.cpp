// SPDX-License-Identifier: GPL-3.0-or-later
// Builds zstd packs from a file tree. Ported from quer3q/ps5-dump-forge's
// ps5-dump-forge-zstd/src/writer.rs (pack_file, Volumes, pack_run; GPL-3.0)
// and ps5-dump-forge-core/src/zstd_pack.rs (which files, the runtime and a
// fresh ampr_emu.index); the tree handling follows Alembic's LZ4 packer.
//
// ps5-dump-forge writes each file in one pass while its encoded chunks fit
// a buffer and reads it again otherwise; here pass 1 only measures (codec,
// size and CRC of every chunk, decided per file by the 1% store-only rule)
// and the volumes are made in pass 2 while they are read. Same bytes.
#include "zstdpack_internal.hpp"

#include "alembic/ampr.hpp"
#include "alembic/exfat.hpp"
#include "alembic/lz4pack.hpp"
#include "alembic/thread_pool.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <random>
#include <set>

namespace alembic::zstdpack {

namespace {

using detail::crc;

// One chunk on its way through the pool.
struct Block {
    std::vector<std::uint8_t> raw;
    std::vector<std::uint8_t> frame; // the zstd frame when it is smaller than raw
    bool zstd = false;               // the frame is smaller
    bool raw_only = false;           // store raw without compressing (store-only file, raw chunk)
    bool keep_frame = false;         // pass 2 writes the bytes; pass 1 needs only sizes and CRCs
    std::uint32_t frame_size = 0;
    std::uint32_t raw_crc = 0, frame_crc = 0;
    std::optional<Error> error;
    bool done = false;
};

// Per-worker encoders, made once (Pool jobs get their worker index).
struct Encoders {
    std::vector<std::unique_ptr<Encoder>> each;
    explicit Encoders(unsigned n) {
        for (unsigned i = 0; i < n; ++i) each.push_back(std::make_unique<Encoder>());
    }
};

void compress_block(Block& b, int level, Encoder& encoder) {
    b.raw_crc = crc(b.raw);
    if (b.raw_only) return;
    auto smaller = encoder.compress(b.raw, level, b.frame);
    if (!smaller.ok()) {
        b.error = smaller.take_error();
        return;
    }
    b.zstd = *smaller;
    if (b.zstd) {
        b.frame_size = static_cast<std::uint32_t>(b.frame.size());
        b.frame_crc = crc(b.frame);
    }
    if (!b.zstd || !b.keep_frame) b.frame = {};
}

// Hands blocks to the pool; callers wait for each one. The destructor waits
// for every job, so no job outlives the object.
class Compressor {
public:
    Compressor(Pool& pool, Encoders& encoders, int level) : pool_(pool), encoders_(encoders), level_(level) {}
    ~Compressor() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return pending_ == 0; });
    }
    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;

    void submit(const std::shared_ptr<Block>& block) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++pending_;
        }
        pool_.submit([this, block](unsigned worker) {
            compress_block(*block, level_, *encoders_.each[worker]);
            std::lock_guard<std::mutex> lock(mutex_);
            block->done = true;
            --pending_;
            done_.notify_all();
        });
    }
    void wait(const Block& block) {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return block.done; });
    }

private:
    Pool& pool_;
    Encoders& encoders_;
    int level_;
    std::mutex mutex_;
    std::condition_variable done_;
    std::size_t pending_ = 0;
};

tree::Node* find_dir_icase(tree::Node& dir, std::string_view name) {
    for (tree::Node& c : dir.children)
        if (c.is_directory && unicode::ascii_iequals(c.name, name)) return &c;
    return nullptr;
}

std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }

// `stored * 100 > size * 99`, exactly: the file saves less than 1%.
bool store_only(std::uint64_t stored, std::uint64_t size) {
    __extension__ typedef unsigned __int128 u128;
    return static_cast<u128>(stored) * 100 > static_cast<u128>(size) * 99;
}

// What a node of the output tree holds.
struct Content {
    enum Kind { input, bytes, module, volume } kind = input;
    const tree::Node* input_node = nullptr;
    std::size_t index = 0; // into blobs_ or the volumes
};

class BytesFile final : public RandomAccess {
public:
    explicit BytesFile(std::shared_ptr<const std::vector<std::uint8_t>> data) : data_(std::move(data)) {}
    Result<std::uint64_t> size() const override { return data_->size(); }
    Status read_at(std::uint64_t offset, MutableBytes out) const override {
        if (offset > data_->size() || out.size() > data_->size() - offset)
            return make_error(Errc::invalid_argument, "read past the end of an in-memory file");
        if (!out.empty()) std::memcpy(out.data(), data_->data() + offset, out.size());
        return {};
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> data_;
};

// A packed file in index order.
struct Item {
    std::string rel;
    const tree::Node* input_node = nullptr; // in the source tree
    std::uint64_t output_id = 0;            // its node id in the output tree
    std::uint64_t size = 0;
    std::uint32_t first_chunk = 0;
    bool store_only = false;
};

class PackedTreeImpl;

// Pass 2: one volume, made while it is read. zstd chunks are compressed again
// on the pool (a read-ahead window) and checked against pass 1.
class VolumeFile final : public RandomAccess {
public:
    VolumeFile(const PackedTreeImpl& tree, std::size_t volume) : tree_(tree), volume_(volume) {}
    ~VolumeFile() override { compressor_.reset(); }
    Result<std::uint64_t> size() const override;
    Status read_at(std::uint64_t offset, MutableBytes out) const override;

private:
    struct Pending {
        std::uint64_t chunk;
        std::shared_ptr<Block> block;
    };
    Status fill(std::uint64_t chunk) const;
    Result<std::shared_ptr<Block>> start(std::uint64_t chunk) const;

    const PackedTreeImpl& tree_;
    std::size_t volume_;
    mutable std::mutex mutex_;
    mutable std::deque<Pending> window_;
    mutable std::uint64_t next_ = 0;
    mutable std::size_t open_item_ = SIZE_MAX;
    mutable std::shared_ptr<const RandomAccess> open_file_;
    mutable std::unique_ptr<Compressor> compressor_;
};

class PackedTreeImpl final : public PackedTree {
public:
    const tree::Node& root() const override { return root_; }
    const std::string& label() const override { return source_->label(); }
    Result<std::shared_ptr<const RandomAccess>> open(const tree::Node& file) const override {
        if (file.is_directory || file.id >= contents_.size())
            return make_error(Errc::invalid_argument, "not a file of this packed tree");
        const Content& c = contents_[file.id];
        switch (c.kind) {
        case Content::input: return source_->open(*c.input_node);
        case Content::bytes: return std::shared_ptr<const RandomAccess>(std::make_shared<BytesFile>(blobs_[c.index]));
        case Content::module: return module_;
        case Content::volume: return std::shared_ptr<const RandomAccess>(std::make_shared<VolumeFile>(*this, c.index));
        }
        return make_error(Errc::invalid_argument, "unknown packed tree node");
    }
    const PackStats& stats() const override { return stats_; }
    const Index& index() const override { return index_; }

    Status build(const tree::Source& source, const PackOptions& options, const ProgressFn& progress);

    // Used by VolumeFile.
    const tree::Source* source_ = nullptr;
    Index index_;
    std::vector<Item> items_;
    std::vector<std::uint32_t> chunk_item_; // chunk -> item
    std::vector<std::uint32_t> chunk_crc_;  // CRC-32 of each chunk's stored bytes
    std::vector<std::uint64_t> volume_first_; // first chunk of each volume, then the chunk count
    std::unique_ptr<Pool> pool_;
    std::unique_ptr<Encoders> encoders_;
    int level_ = kDefaultLevel;

private:
    Status make_deployed_tree(const PackOptions& options, std::int64_t now);
    Status choose(const PackOptions& options);
    Status pass_one(const ProgressFn& progress, std::uint64_t volume_limit);
    Status finish(std::int64_t now, std::uint64_t volume_limit);
    tree::Node& add_file(const std::string& rel, std::uint64_t size, std::int64_t mtime, Content c);
    void copy_tree(const tree::Node& in, tree::Node& out, bool is_root);

    tree::Node root_;
    std::vector<Content> contents_;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> blobs_;
    std::shared_ptr<const RandomAccess> module_;
    PackStats stats_;
};

// ---------------------------------------------------------------- output tree

// The input tree, without OS junk and a stale root ampr_emu.index.
void PackedTreeImpl::copy_tree(const tree::Node& in, tree::Node& out, bool is_root) {
    for (const tree::Node& child : in.children) {
        if (tree::is_os_junk(child.name)) continue;
        if (is_root && !child.is_directory &&
            (unicode::ascii_iequals(child.name, "ampr_emu.index") || unicode::ascii_iequals(child.name, "ampr_emu.index.tmp")))
            continue;
        tree::Node node;
        node.name = child.name;
        node.is_directory = child.is_directory;
        node.size = child.size;
        node.mtime = child.mtime;
        if (child.is_directory) {
            copy_tree(child, node, false);
        } else {
            Content c;
            c.input_node = &child;
            contents_.push_back(c);
            node.id = contents_.size() - 1;
        }
        out.children.push_back(std::move(node));
    }
}

tree::Node& PackedTreeImpl::add_file(const std::string& rel, std::uint64_t size, std::int64_t mtime, Content c) {
    tree::Node* dir = &root_;
    std::size_t at = 0;
    for (;;) {
        std::size_t slash = rel.find('/', at);
        if (slash == std::string::npos) break;
        std::string part = rel.substr(at, slash - at);
        tree::Node* next = find_dir_icase(*dir, part);
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
    std::string name = rel.substr(at);
    std::erase_if(dir->children, [&](const tree::Node& n) { return !n.is_directory && unicode::ascii_iequals(n.name, name); });
    tree::Node node;
    node.name = name;
    node.size = size;
    node.mtime = mtime;
    contents_.push_back(c);
    node.id = contents_.size() - 1;
    dir->children.push_back(std::move(node));
    return dir->children.back();
}

Status PackedTreeImpl::make_deployed_tree(const PackOptions& options, std::int64_t now) {
    root_ = tree::Node{};
    root_.is_directory = true;
    root_.mtime = source_->root().mtime;
    copy_tree(source_->root(), root_, true);

    if (options.runtime_module) {
        ALEMBIC_ASSIGN(std::uint64_t size, options.runtime_module->size());
        module_ = options.runtime_module;
        // Keep the existing spelling of fakelib/ and of the file name.
        std::string rel = kRuntimeRelativePath;
        if (tree::Node* fake = find_dir_icase(root_, "fakelib")) {
            rel = fake->name + "/libSceAmpr.sprx";
            for (const tree::Node& n : fake->children)
                if (!n.is_directory && unicode::ascii_iequals(n.name, "libSceAmpr.sprx")) rel = fake->name + "/" + n.name;
        }
        Content c;
        c.kind = Content::module;
        add_file(rel, size, now, c);
    } else {
        const tree::Node* fake = find_dir_icase(root_, "fakelib");
        const tree::Node* module = fake ? tree::find_child_icase(*fake, "libSceAmpr.sprx") : nullptr;
        if (!module || module->is_directory)
            return make_error(Errc::invalid_argument,
                              "the game has no fakelib/libSceAmpr.sprx: pass ampr-emu-ng's libSceAmpr.sprx "
                              "(packed files cannot be read without a runtime that reads zstd packs)");
        stats_.warnings.push_back("kept the game's own fakelib/libSceAmpr.sprx; it must be ampr-emu-ng's to read "
                                  "zstd packs");
    }

    // ampr_emu.index lists every file, loose and packed (built before the
    // packs exist, so it does not list them), as ps5-dump-forge writes it anew.
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> ampr_index, exfat::build_ampr_index(*this, root_, ampr::Scheme::ampr_emu));
    if (ampr_index.empty()) return make_error(Errc::invalid_argument, "the tree has no files to pack");
    blobs_.push_back(std::make_shared<const std::vector<std::uint8_t>>(std::move(ampr_index)));
    Content c;
    c.kind = Content::bytes;
    c.index = blobs_.size() - 1;
    add_file("ampr_emu.index", blobs_.back()->size(), now, c);
    return {};
}

Status PackedTreeImpl::choose(const PackOptions& options) {
    std::set<std::string> wanted;
    if (options.packed_set) wanted.insert(options.packed_set->begin(), options.packed_set->end());
    std::function<void(const tree::Node&, const std::string&)> walk = [&](const tree::Node& dir, const std::string& prefix) {
        for (const tree::Node& n : dir.children) {
            std::string rel = join(prefix, n.name);
            if (n.is_directory) {
                walk(n, rel);
                continue;
            }
            ++stats_.files_total;
            const Content& c = contents_[n.id];
            bool pack = options.packed_set ? wanted.count(rel) && !always_loose(rel) : default_selected(rel, n.size);
            if (!pack || c.kind != Content::input) {
                stats_.loose_bytes += n.size;
                continue;
            }
            if (!valid_path(rel) || rel.size() > 0xFFFF) {
                stats_.warnings.push_back("left loose (a path the format cannot hold): " + rel);
                stats_.loose_bytes += n.size;
                continue;
            }
            Item item;
            item.rel = rel;
            item.input_node = c.input_node;
            item.output_id = n.id;
            item.size = n.size;
            items_.push_back(std::move(item));
        }
    };
    walk(root_, "");
    std::sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return detail::key_less(a.rel, b.rel); });
    for (std::size_t i = 1; i < items_.size(); ++i)
        if (items_[i].rel == items_[i - 1].rel) return make_error(Errc::invalid_data, "a path appears twice: " + items_[i].rel);
    std::uint64_t chunks = 0;
    for (const Item& item : items_) chunks += chunk_count(item.size);
    if (chunks > 0xFFFFFFFFull) return make_error(Errc::too_large, "too many zstd chunks");
    return {};
}

// ---------------------------------------------------------------- pass 1

Status PackedTreeImpl::pass_one(const ProgressFn& progress, std::uint64_t volume_limit) {
    std::uint64_t total = 0, done = 0;
    for (const Item& item : items_) total += item.size;
    Compressor compressor(*pool_, *encoders_, level_);
    std::uint64_t cursor = 0; // in the open volume; 0 = none open
    struct Result1 {
        bool zstd;
        std::uint32_t frame_size, raw_crc, frame_crc;
    };
    std::vector<Result1> results;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        Item& item = items_[i];
        item.first_chunk = static_cast<std::uint32_t>(index_.chunks.size());
        const std::uint64_t count = chunk_count(item.size);
        results.clear();
        if (count) {
            ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, source_->open(*item.input_node));
            std::deque<std::shared_ptr<Block>> pending;
            const std::size_t window = std::max<std::size_t>(4, pool_->size() * 4);
            std::uint64_t next_read = 0;
            for (std::uint64_t k = 0; k < count; ++k) {
                while (next_read < count && pending.size() < window) {
                    auto b = std::make_shared<Block>();
                    b->raw.resize(static_cast<std::size_t>(chunk_raw_size(item.size, next_read)));
                    ALEMBIC_TRY(file->read_at(next_read * kChunkSize, b->raw));
                    compressor.submit(b);
                    pending.push_back(std::move(b));
                    ++next_read;
                }
                std::shared_ptr<Block> b = pending.front();
                pending.pop_front();
                compressor.wait(*b);
                if (b->error) return *b->error;
                results.push_back({b->zstd, b->frame_size, b->raw_crc, b->frame_crc});
                done += b->raw.size();
                if (progress && !progress({"zstd pass 1", done, total})) return make_error(Errc::cancelled, "cancelled");
            }
        }
        // The 1% rule on what the chunks would store.
        std::uint64_t stored = 0;
        for (std::uint64_t k = 0; k < count; ++k)
            stored += results[k].zstd ? results[k].frame_size : chunk_raw_size(item.size, k);
        item.store_only = count && store_only(stored, item.size);
        for (std::uint64_t k = 0; k < count; ++k) {
            const Result1& r = results[k];
            const bool zstd = r.zstd && !item.store_only;
            const std::uint32_t size = zstd ? r.frame_size : static_cast<std::uint32_t>(chunk_raw_size(item.size, k));
            if (cursor && cursor + size > volume_limit) {
                index_.volume_sizes.push_back(cursor);
                cursor = 0;
            }
            if (!cursor) {
                if (index_.volume_sizes.size() >= kMaxVolumes) return make_error(Errc::too_large, "too many zstd pack volumes");
                volume_first_.push_back(index_.chunks.size());
                cursor = kVolumeHeaderSize;
            }
            Chunk c;
            c.offset = static_cast<std::uint32_t>(cursor);
            c.volume = static_cast<std::uint16_t>(index_.volume_sizes.size());
            c.stored_size = size;
            c.codec = zstd ? Codec::zstd : Codec::raw;
            cursor += size;
            index_.chunks.push_back(c);
            chunk_item_.push_back(static_cast<std::uint32_t>(i));
            chunk_crc_.push_back(zstd ? r.frame_crc : r.raw_crc);
            ++stats_.chunks;
            ++(zstd ? stats_.chunks_zstd : stats_.chunks_raw);
            stats_.stored_bytes += size;
        }
        stats_.logical_bytes += item.size;
        ++stats_.files_packed;
        if (item.store_only) ++stats_.files_store_only;
    }
    if (cursor) index_.volume_sizes.push_back(cursor);
    volume_first_.push_back(index_.chunks.size());
    return {};
}

// ---------------------------------------------------------------- finish

Status PackedTreeImpl::finish(std::int64_t now, std::uint64_t volume_limit) {
    for (const Item& item : items_) {
        FileRecord f;
        f.path = item.rel;
        f.size = item.size;
        f.first_chunk = item.first_chunk;
        f.store_only = item.store_only;
        index_.files.push_back(std::move(f));
    }
    ALEMBIC_ASSIGN(std::vector<std::uint8_t> bytes, encode_index(index_, volume_limit));
    stats_.volumes = static_cast<std::uint32_t>(index_.volume_sizes.size());
    for (std::uint64_t s : index_.volume_sizes) stats_.volume_bytes += s;

    std::set<std::uint64_t> packed_ids;
    for (const Item& item : items_) packed_ids.insert(item.output_id);
    std::function<void(tree::Node&)> prune = [&](tree::Node& dir) {
        std::erase_if(dir.children, [&](const tree::Node& n) { return !n.is_directory && packed_ids.count(n.id); });
        for (tree::Node& child : dir.children)
            if (child.is_directory) prune(child);
    };
    prune(root_);

    blobs_.push_back(std::make_shared<const std::vector<std::uint8_t>>(std::move(bytes)));
    Content c;
    c.kind = Content::bytes;
    c.index = blobs_.size() - 1;
    add_file(kIndexName, blobs_.back()->size(), now, c);
    for (std::size_t v = 0; v < index_.volume_sizes.size(); ++v) {
        Content vc;
        vc.kind = Content::volume;
        vc.index = v;
        add_file(volume_name(static_cast<std::uint32_t>(v)), index_.volume_sizes[v], now, vc);
    }
    return {};
}

Status PackedTreeImpl::build(const tree::Source& source, const PackOptions& options, const ProgressFn& progress) {
    source_ = &source;
    if (is_packed(source.root())) return make_error(Errc::invalid_argument, "this tree is already zstd packed");
    if (lz4pack::is_packed(source.root()))
        return make_error(Errc::invalid_argument, "this tree has LZ4 packs: unpack them first");
    if (options.level < kMinLevel || options.level > kMaxLevel)
        return make_error(Errc::invalid_argument, "the zstd level must be 1-22");
    if (options.volume_limit < kVolumeHeaderSize + kChunkSize || options.volume_limit > kVolumeLimit)
        return make_error(Errc::invalid_argument, "bad zstd volume limit");
    level_ = options.level;
    index_.level = static_cast<std::uint8_t>(options.level);
    if (options.archive_id) {
        index_.archive_id = *options.archive_id;
    } else {
        std::random_device rd;
        for (auto& b : index_.archive_id) b = static_cast<std::uint8_t>(rd());
    }
    std::int64_t now = options.timestamp.value_or(static_cast<std::int64_t>(std::time(nullptr)));
    pool_ = std::make_unique<Pool>(worker_count(options.workers));
    encoders_ = std::make_unique<Encoders>(pool_->size());

    ALEMBIC_TRY(make_deployed_tree(options, now));
    ALEMBIC_TRY(choose(options));
    ALEMBIC_TRY(pass_one(progress, options.volume_limit));
    ALEMBIC_TRY(finish(now, options.volume_limit));
    return {};
}

// ---------------------------------------------------------------- pass 2

Result<std::uint64_t> VolumeFile::size() const { return tree_.index_.volume_sizes[volume_]; }

// Reads chunk `chunk` and starts compressing it when it is a zstd one.
Result<std::shared_ptr<Block>> VolumeFile::start(std::uint64_t chunk) const {
    const Chunk& c = tree_.index_.chunks[chunk];
    std::uint32_t item_index = tree_.chunk_item_[chunk];
    const Item& item = tree_.items_[item_index];
    if (open_item_ != item_index) {
        open_file_.reset();
        ALEMBIC_ASSIGN(open_file_, tree_.source_->open(*item.input_node));
        open_item_ = item_index;
    }
    auto block = std::make_shared<Block>();
    std::uint64_t local = chunk - item.first_chunk;
    block->raw.resize(static_cast<std::size_t>(chunk_raw_size(item.size, local)));
    ALEMBIC_TRY(open_file_->read_at(local * kChunkSize, block->raw));
    if (c.codec == Codec::raw) {
        block->raw_only = true;
        block->raw_crc = crc(block->raw);
        block->done = true;
        return block;
    }
    if (!compressor_) compressor_ = std::make_unique<Compressor>(*tree_.pool_, *tree_.encoders_, tree_.level_);
    block->keep_frame = true;
    compressor_->submit(block);
    return block;
}

// Makes `chunk` the front of the window and keeps the window running ahead.
Status VolumeFile::fill(std::uint64_t chunk) const {
    const std::uint64_t end = tree_.volume_first_[volume_ + 1];
    while (!window_.empty() && window_.front().chunk < chunk) window_.pop_front();
    if (window_.empty() || window_.front().chunk != chunk) {
        window_.clear();
        next_ = chunk;
    }
    const std::size_t depth = std::max<std::size_t>(8, tree_.pool_->size() * 4);
    while (next_ < end && window_.size() < depth) {
        ALEMBIC_ASSIGN(std::shared_ptr<Block> block, start(next_));
        window_.push_back({next_, std::move(block)});
        ++next_;
    }
    return {};
}

Status VolumeFile::read_at(std::uint64_t offset, MutableBytes out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const Index& ix = tree_.index_;
    const std::uint64_t file_size = ix.volume_sizes[volume_];
    if (offset > file_size || out.size() > file_size - offset)
        return make_error(Errc::invalid_argument, "read past the end of a zstd pack volume");
    const std::uint64_t end = offset + out.size();
    if (offset < kVolumeHeaderSize) {
        VolumeHeader h;
        h.id = static_cast<std::uint32_t>(volume_);
        h.count = static_cast<std::uint32_t>(ix.volume_sizes.size());
        h.archive_id = ix.archive_id;
        h.payload_bytes = file_size - kVolumeHeaderSize;
        ALEMBIC_ASSIGN(auto header, encode_volume_header(h));
        std::uint64_t n = std::min<std::uint64_t>(kVolumeHeaderSize, end) - offset;
        std::memcpy(out.data(), header.data() + offset, n);
    }
    const std::uint64_t first = tree_.volume_first_[volume_], last = tree_.volume_first_[volume_ + 1];
    // First chunk of the volume that ends after `offset`.
    std::uint64_t lo = first, hi = last;
    while (lo < hi) {
        std::uint64_t mid = lo + (hi - lo) / 2;
        const Chunk& c = ix.chunks[mid];
        if (std::uint64_t{c.offset} + c.stored_size <= offset) lo = mid + 1;
        else hi = mid;
    }
    for (std::uint64_t k = lo; k < last; ++k) {
        const Chunk& c = ix.chunks[k];
        if (c.offset >= end) break;
        ALEMBIC_TRY(fill(k));
        Pending& p = window_.front();
        if (compressor_) compressor_->wait(*p.block);
        const Block& b = *p.block;
        if (b.error) return *b.error;
        const bool same = c.codec == Codec::raw ? b.raw_crc == tree_.chunk_crc_[k]
                                                : b.zstd && b.frame_size == c.stored_size && b.frame_crc == tree_.chunk_crc_[k];
        if (!same)
            return make_error(Errc::invalid_data, "a zstd chunk came out differently the second time (the source changed?): " +
                                                      tree_.items_[tree_.chunk_item_[k]].rel);
        const std::vector<std::uint8_t>& bytes = c.codec == Codec::raw ? b.raw : b.frame;
        std::uint64_t from = std::max<std::uint64_t>(offset, c.offset), to = std::min<std::uint64_t>(end, std::uint64_t{c.offset} + c.stored_size);
        std::memcpy(out.data() + (from - offset), bytes.data() + (from - c.offset), static_cast<std::size_t>(to - from));
    }
    return {};
}

} // namespace

Result<std::unique_ptr<PackedTree>> pack(const tree::Source& source, const PackOptions& options, const ProgressFn& progress) {
    auto tree = std::make_unique<PackedTreeImpl>();
    ALEMBIC_TRY(tree->build(source, options, progress));
    return std::unique_ptr<PackedTree>(std::move(tree));
}

} // namespace alembic::zstdpack
