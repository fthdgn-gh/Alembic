// SPDX-License-Identifier: GPL-3.0-or-later
// Builds an LZ4 pack set from a file tree. Ported from drakmor/ampr_emu's
// tools/ampr_pack.py (build_packs, plan_files, PackVolumeWriter, PackLayout;
// GPL-3.0); the keep-on-disk list, runtime placement and runtime cache
// budget follow PS5-LZ4-Compression's src/ampr_pack.c (GPL-3.0-or-later).
// Output is byte-identical to `ampr_pack.py pack` given the same TOML
// (to_toml) and --exclude-from list (forced_loose_patterns).
#include "lz4pack_internal.hpp"

#include "alembic/ampr.hpp"
#include "alembic/bytes.hpp"
#include "alembic/exfat.hpp"
#include "alembic/sha256.hpp"
#include "alembic/unicode.hpp"

#include "lz4.h"
#include "lz4hc.h"
#include "xxhash.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace alembic::lz4pack {

namespace {

using detail::crc;

constexpr std::uint64_t kMiB = 1024 * 1024;

// ---------------------------------------------------------------- workers

// A fixed set of threads running jobs in any order. Jobs get their worker
// index so each thread can keep its own LZ4 state.
class Pool {
public:
    explicit Pool(unsigned threads) {
        if (threads == 0) threads = 1;
        for (unsigned i = 0; i < threads; ++i) threads_.emplace_back([this, i] { run(i); });
    }
    ~Pool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        work_.notify_all();
        for (std::thread& t : threads_) t.join();
    }
    unsigned size() const { return static_cast<unsigned>(threads_.size()); }

    void submit(std::function<void(unsigned)> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(job));
        }
        work_.notify_one();
    }

private:
    void run(unsigned index) {
        for (;;) {
            std::function<void(unsigned)> job;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                work_.wait(lock, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop_front();
            }
            job(index);
        }
    }

    std::vector<std::thread> threads_;
    std::deque<std::function<void(unsigned)>> queue_;
    std::mutex mutex_;
    std::condition_variable work_;
    bool stop_ = false;
};

// Per-worker LZ4 state, allocated once.
struct Lz4States {
    std::vector<std::vector<char>> hc, fast;
    explicit Lz4States(unsigned n) : hc(n), fast(n) {
        for (unsigned i = 0; i < n; ++i) {
            hc[i].resize(static_cast<std::size_t>(LZ4_sizeofStateHC()));
            fast[i].resize(static_cast<std::size_t>(LZ4_sizeofState()));
        }
    }
};

struct CompressParams {
    bool store = false;
    bool hc = true;
    int level = 12;
    int acceleration = 1;
    std::uint64_t min_savings_bytes = 64;
    double min_savings_ratio = 0.01;
    std::uint64_t io_neutral_bytes = 8192;
    double io_neutral_ratio = 0.125;
    bool streaming = false;
    std::uint32_t io_page_size = 65536;
};

CompressParams params_for(const Rule& rule, std::uint32_t io_page_size) {
    CompressParams p;
    p.store = rule.action == Action::store;
    p.hc = rule.hc;
    p.level = std::clamp(rule.level, 1, 12);
    p.acceleration = std::max(1, rule.acceleration);
    p.min_savings_bytes = rule.min_savings_bytes;
    p.min_savings_ratio = rule.min_savings_ratio;
    p.io_neutral_bytes = rule.io_neutral_min_savings_bytes;
    p.io_neutral_ratio = rule.io_neutral_min_savings_ratio;
    p.streaming = rule.resolved_layout() == Layout::streaming;
    p.io_page_size = io_page_size;
    return p;
}

// One block through _compress_block(): LZ4 unless the gain is too small.
struct Block {
    std::vector<std::uint8_t> raw;
    std::vector<std::uint8_t> stored; // empty when RAW (the raw bytes are the stored bytes)
    std::uint32_t codec = kCodecRaw;
    std::uint32_t stored_size = 0;
    std::uint32_t raw_crc = 0;
    std::uint32_t stored_crc = 0;
    std::uint64_t h1 = 0, h2 = 0; // raw fingerprint for de-duplication
    bool keep_stored = false;     // pass 2 needs the bytes, pass 1 only the sizes
    bool done = false;
};

void compress_block(Block& b, const CompressParams& p, Lz4States& states, unsigned worker, bool fingerprint) {
    b.raw_crc = crc(b.raw);
    if (fingerprint) {
        b.h1 = XXH64(b.raw.data(), b.raw.size(), 0);
        b.h2 = XXH64(b.raw.data(), b.raw.size(), 0x9E3779B97F4A7C15ull);
    }
    auto as_raw = [&] {
        b.codec = kCodecRaw;
        b.stored_size = static_cast<std::uint32_t>(b.raw.size());
        b.stored_crc = b.raw_crc;
        b.stored.clear();
    };
    if (p.store || b.raw.empty()) return as_raw();
    int src = static_cast<int>(b.raw.size());
    int capacity = LZ4_compressBound(src);
    b.stored.resize(static_cast<std::size_t>(capacity));
    int written = p.hc ? LZ4_compress_HC_extStateHC(states.hc[worker].data(), reinterpret_cast<const char*>(b.raw.data()),
                                                    reinterpret_cast<char*>(b.stored.data()), src, capacity, p.level)
                       : LZ4_compress_fast_extState(states.fast[worker].data(),
                                                    reinterpret_cast<const char*>(b.raw.data()),
                                                    reinterpret_cast<char*>(b.stored.data()), src, capacity,
                                                    p.acceleration);
    if (written <= 0) return as_raw(); // cannot happen with compressBound
    std::int64_t saved = static_cast<std::int64_t>(b.raw.size()) - written;
    double ratio = static_cast<double>(saved) / static_cast<double>(b.raw.size());
    std::uint64_t required_bytes = p.min_savings_bytes;
    double required_ratio = p.min_savings_ratio;
    if (!p.streaming) {
        std::uint64_t raw_pages = div_round_up(b.raw.size(), p.io_page_size);
        std::uint64_t compressed_pages = div_round_up(static_cast<std::uint64_t>(written), p.io_page_size);
        if (compressed_pages >= raw_pages) {
            required_bytes = std::max(required_bytes, p.io_neutral_bytes);
            required_ratio = std::max(required_ratio, p.io_neutral_ratio);
        }
    }
    if (saved < static_cast<std::int64_t>(required_bytes) || ratio < required_ratio) return as_raw();
    b.codec = kCodecLz4;
    b.stored_size = static_cast<std::uint32_t>(written);
    b.stored.resize(static_cast<std::size_t>(written));
    b.stored_crc = crc(b.stored);
    if (!b.keep_stored) b.stored = {};
}

// Hands blocks to the pool; callers wait for each one. The destructor waits
// for every job, so no job outlives the object.
class Compressor {
public:
    Compressor(Pool& pool, Lz4States& states) : pool_(pool), states_(states) {}
    ~Compressor() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return pending_ == 0; });
    }
    Compressor(const Compressor&) = delete;
    Compressor& operator=(const Compressor&) = delete;

    void submit(const std::shared_ptr<Block>& block, const CompressParams& params, bool fingerprint) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++pending_;
        }
        pool_.submit([this, block, params, fingerprint](unsigned worker) {
            compress_block(*block, params, states_, worker, fingerprint);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                block->done = true;
                --pending_;
            }
            done_.notify_all();
        });
    }
    void wait(const Block& block) {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [&] { return block.done; });
    }

private:
    Pool& pool_;
    Lz4States& states_;
    std::mutex mutex_;
    std::condition_variable done_;
    std::size_t pending_ = 0;
};

// ---------------------------------------------------------------- tree helpers

std::string join(const std::string& a, const std::string& b) { return a.empty() ? b : a + "/" + b; }

tree::Node* find_dir_icase(tree::Node& dir, std::string_view name) {
    for (tree::Node& child : dir.children)
        if (child.is_directory && unicode::ascii_iequals(child.name, name)) return &child;
    return nullptr;
}

std::string ascii_lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

// Escapes a literal path for fnmatch.
std::string glob_escape(const std::string& path) {
    std::string out;
    for (char c : path) {
        if (c == '[' || c == '*' || c == '?') {
            out += '[';
            out += c;
            out += ']';
        } else {
            out += c;
        }
    }
    return out;
}

// PS5-LZ4-Compression's k_keep_on_disk: files read outside the game process
// (PlayGo markers, boot files of one engine), plus every empty file.
const char* const kKeepOnDisk[] = {
    "cache_ps5/pgc_*_dummy_file",
    "cache_ps5/playgo.pgm",
    "cache_ps5/load_menu_catalog.log",
    "cache_ps5/all_shaders.texmeshman",
    "cache_ps5/game.sprig.packman",
    "cache_ps5/game.sprig.texmeshman",
    "cache_ps5/pulse.sprig.texmeshman",
    "sounds/scream/_generated/buss_config.bus",
    "sounds/scream/_generated/distance_model.dml",
    "sounds/scream/_generated/group_mix.gmx",
};

// str.format() for the fields pack_pattern may use: {group} {lane} {volume}
// {id}, optionally with ":d" / ":Nd" / ":0Nd".
Result<std::string> render_pattern(const std::string& pattern, const std::string& group, std::uint64_t lane,
                                   std::uint64_t volume, std::uint64_t id) {
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];
        if (c == '{' && i + 1 < pattern.size() && pattern[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        if (c == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            out += '}';
            ++i;
            continue;
        }
        if (c != '{') {
            if (c == '}') return make_error(Errc::invalid_argument, "invalid pack_pattern: " + pattern);
            out += c;
            continue;
        }
        std::size_t close = pattern.find('}', i);
        if (close == std::string::npos) return make_error(Errc::invalid_argument, "invalid pack_pattern: " + pattern);
        std::string field = pattern.substr(i + 1, close - i - 1);
        std::string spec;
        if (std::size_t colon = field.find(':'); colon != std::string::npos) {
            spec = field.substr(colon + 1);
            field = field.substr(0, colon);
        }
        i = close;
        if (field == "group") {
            if (!spec.empty()) return make_error(Errc::unsupported, "unsupported pack_pattern format: " + pattern);
            out += group;
            continue;
        }
        std::uint64_t value;
        if (field == "lane") value = lane;
        else if (field == "volume") value = volume;
        else if (field == "id") value = id;
        else return make_error(Errc::invalid_argument, "pack_pattern uses unsupported field " + field);
        std::string digits = std::to_string(value);
        if (!spec.empty()) {
            if (spec.back() == 'd') spec.pop_back();
            bool zero = !spec.empty() && spec[0] == '0';
            std::size_t width = 0;
            for (char d : spec) {
                if (d < '0' || d > '9') return make_error(Errc::unsupported, "unsupported pack_pattern format: " + pattern);
                width = width * 10 + static_cast<std::size_t>(d - '0');
            }
            if (digits.size() < width) digits.insert(0, width - digits.size(), zero ? '0' : ' ');
        }
        out += digits;
    }
    return out;
}

// The glob that matches any rendering of the pattern (_pack_output_glob).
std::string pattern_glob(const std::string& pattern) {
    std::string out;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 1 < pattern.size() && pattern[i + 1] == '{') {
            out += '{';
            ++i;
        } else if (pattern[i] == '}' && i + 1 < pattern.size() && pattern[i + 1] == '}') {
            out += '}';
            ++i;
        } else if (pattern[i] == '{') {
            std::size_t close = pattern.find('}', i);
            if (close == std::string::npos) break;
            out += '*';
            i = close;
        } else {
            out += pattern[i];
        }
    }
    std::replace(out.begin(), out.end(), '\\', '/');
    std::size_t start = out.find_first_not_of('/');
    return start == std::string::npos ? std::string() : out.substr(start);
}

// ---------------------------------------------------------------- volumes

// PackVolumeWriter without the file: only positions, so pass 2 can produce
// the same bytes later.
struct VolumePlan {
    std::uint16_t pack_id = 0;
    std::string name;
    std::size_t group = 0;
    std::uint32_t lane = 0;
    std::uint32_t chunk_alignment = 64;
    std::uint32_t io_page_size = 65536;
    std::uint64_t payload_offset = 0;
    std::uint64_t max_pack_size = 0;
    std::uint32_t flags = 0;
    std::uint64_t position = 0;
    std::uint64_t padding_bytes = 0;
    std::vector<std::uint32_t> placed; // chunk indices, in offset order

    std::pair<std::uint64_t, std::uint32_t> placement_for(std::uint32_t stored, bool streaming,
                                                          bool extent_start) const {
        std::uint64_t pos = position;
        if (extent_start) pos = align_up(pos, io_page_size);
        std::uint64_t offset;
        if (streaming) {
            offset = align_up(pos, chunk_alignment);
        } else if (stored >= io_page_size) {
            offset = align_up(pos, io_page_size);
        } else {
            offset = align_up(pos, chunk_alignment);
            std::uint64_t page_end = align_up(offset + 1, io_page_size);
            if (offset + stored > page_end) offset = align_up(offset, io_page_size);
        }
        std::uint32_t placement = 0;
        if (stored <= io_page_size && offset / io_page_size == (offset + stored - 1) / io_page_size)
            placement |= kChunkPageContained;
        if (offset % io_page_size == 0) placement |= kChunkPageAligned;
        return {offset, placement};
    }
    bool can_fit(std::uint32_t stored, bool streaming, bool extent_start) const {
        std::uint64_t end = placement_for(stored, streaming, extent_start).first + stored;
        return max_pack_size == 0 || align_up(end, io_page_size) <= max_pack_size;
    }
    std::uint64_t final_size() const { return align_up(position, io_page_size); }
};

struct Item {
    std::string app0; // "/app0/<rel>" as in AMPRIDX3
    std::string rel;
    std::uint64_t index_size = 0;
    std::int64_t index_mtime = 0;
    tree::Node* node = nullptr; // in the deployed tree; null if the index names a file we lack
    Rule rule;
    std::size_t group = 0;
    std::uint32_t lane = 0;
    std::uint32_t first_chunk = 0;
    std::uint32_t chunk_count = 0;
    bool packed() const { return rule.action != Action::loose; }
};

// What a node of the output tree holds.
struct Content {
    enum Kind { input, bytes, module, volume } kind = input;
    const tree::Node* input_node = nullptr;
    std::size_t index = 0; // into blobs_ or volumes_
};

// An in-memory file.
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

class PackedTreeImpl;

// Pass 2: one volume, produced while it is read. Chunks are recompressed on
// the pool (read-ahead window) and checked against pass 1.
class VolumeFile final : public RandomAccess {
public:
    VolumeFile(const PackedTreeImpl& tree, std::size_t volume) : tree_(tree), volume_(volume) {}
    ~VolumeFile() override;
    Result<std::uint64_t> size() const override;
    Status read_at(std::uint64_t offset, MutableBytes out) const override;

private:
    struct Pending {
        std::size_t position; // index into VolumePlan::placed
        std::shared_ptr<Block> block;
    };
    Status fill(std::size_t position) const;
    Result<std::shared_ptr<Block>> start(std::size_t position) const;
    Status check(std::size_t position, const Block& block) const;

    const PackedTreeImpl& tree_;
    std::size_t volume_;
    mutable std::mutex mutex_;
    mutable std::deque<Pending> window_;
    mutable std::size_t next_ = 0; // next position to start
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
        case Content::bytes:
            return std::shared_ptr<const RandomAccess>(std::make_shared<BytesFile>(blobs_[c.index]));
        case Content::module: return module_;
        case Content::volume:
            return std::shared_ptr<const RandomAccess>(std::make_shared<VolumeFile>(*this, c.index));
        }
        return make_error(Errc::invalid_argument, "unknown packed tree node");
    }
    const PackStats& stats() const override { return stats_; }
    const Manifest& manifest() const override { return manifest_; }
    const BuildId& build_id() const override { return manifest_.build_id; }
    const Config& config() const override { return config_; }
    const std::vector<std::string>& forced_loose_patterns() const override { return forced_patterns_; }

    Status build(const tree::Source& source, const PackOptions& options, const ProgressFn& progress);

    // Used by VolumeFile.
    const tree::Source* source_ = nullptr;
    Config config_;
    Manifest manifest_;
    std::vector<VolumePlan> volumes_;
    std::vector<Item> items_;
    std::vector<std::uint32_t> raw_crcs_, stored_crcs_;
    std::vector<std::uint32_t> chunk_item_; // chunk index -> item index
    std::vector<const tree::Node*> item_input_; // item -> its node in the input tree (packed items)
    std::unique_ptr<Pool> pool_;
    std::unique_ptr<Lz4States> states_;
    std::uint32_t io_page_for(std::size_t group) const {
        return config_.groups[group].io_page_size ? config_.groups[group].io_page_size : config_.io_page_size;
    }

private:
    Status make_deployed_tree(const PackOptions& options, std::int64_t now);
    Status plan(const ProgressFn& progress);
    Status pass_one(const ProgressFn& progress);
    Status sample(Item& item, bool& use_loose, std::string& note);
    Status finish(std::int64_t now);

    std::uint32_t add_content(Content c) {
        contents_.push_back(c);
        return static_cast<std::uint32_t>(contents_.size() - 1);
    }
    tree::Node& add_file(const std::string& rel, std::uint64_t size, std::int64_t mtime, Content c);

    tree::Node root_;
    std::vector<Content> contents_;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> blobs_;
    std::shared_ptr<const RandomAccess> module_;
    std::vector<std::uint8_t> ampr_index_;
    PackStats stats_;
    std::vector<std::string> forced_patterns_;
};

// ---------------------------------------------------------------- deployed tree

// Copies the input tree into `out`, skipping OS junk and a stale root index.
void copy_tree(const tree::Node& in, tree::Node& out, std::vector<Content>& contents, bool is_root) {
    for (const tree::Node& child : in.children) {
        if (tree::is_os_junk(child.name)) continue;
        if (is_root && !child.is_directory &&
            (unicode::ascii_iequals(child.name, "ampr_emu.index") ||
             unicode::ascii_iequals(child.name, "ampr_emu.index.tmp")))
            continue;
        tree::Node node;
        node.name = child.name;
        node.is_directory = child.is_directory;
        node.size = child.size;
        node.mtime = child.mtime;
        if (child.is_directory) {
            copy_tree(child, node, contents, false);
        } else {
            Content c;
            c.kind = Content::input;
            c.input_node = &child;
            contents.push_back(c);
            node.id = contents.size() - 1;
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
    node.id = add_content(c);
    dir->children.push_back(std::move(node));
    return dir->children.back();
}

Status PackedTreeImpl::make_deployed_tree(const PackOptions& options, std::int64_t now) {
    root_ = tree::Node{};
    root_.is_directory = true;
    root_.mtime = source_->root().mtime;
    copy_tree(source_->root(), root_, contents_, true);

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
                              "the game has no fakelib/libSceAmpr.sprx: pass the PackedStdio runtime from ampr_emu "
                              "(packed files cannot be read without it)");
        stats_.warnings.push_back("kept the game's own fakelib/libSceAmpr.sprx; it must be ampr_emu's PackedStdio "
                                  "build (0.4.2 or later) to read the packs");
    }

    ALEMBIC_ASSIGN(ampr_index_, exfat::build_ampr_index(*this, root_, ampr::Scheme::ampr_emu));
    if (ampr_index_.empty()) return make_error(Errc::invalid_argument, "the tree has no files to pack");
    auto blob = std::make_shared<const std::vector<std::uint8_t>>(ampr_index_);
    blobs_.push_back(blob);
    Content c;
    c.kind = Content::bytes;
    c.index = blobs_.size() - 1;
    add_file("ampr_emu.index", ampr_index_.size(), now, c);
    return {};
}

// ---------------------------------------------------------------- planning

std::map<std::string, tree::Node*> index_nodes(tree::Node& root) {
    std::map<std::string, tree::Node*> out;
    std::function<void(tree::Node&, const std::string&)> walk = [&](tree::Node& n, const std::string& prefix) {
        for (tree::Node& child : n.children) {
            std::string rel = join(prefix, child.name);
            if (child.is_directory) walk(child, rel);
            else out[rel] = &child;
        }
    };
    walk(root, "");
    return out;
}

Rule loose_rule(std::uint32_t block_shift) {
    Rule r;
    r.action = Action::loose;
    r.block_shift = block_shift;
    return r;
}

Status PackedTreeImpl::sample(Item& item, bool& use_loose, std::string& note) {
    use_loose = false;
    const Rule& rule = item.rule;
    if (!config_.auto_loose_large_files || rule.action != Action::compress || rule.force_pack ||
        (rule.hot && !config_.auto_loose_hot_files) || item.node->size < config_.auto_loose_min_file_size ||
        item.node->size == 0)
        return {};
    std::uint64_t size = item.node->size;
    std::uint64_t block = 1ull << rule.block_shift;
    std::uint64_t total_blocks = div_round_up(size, block);
    std::uint64_t byte_limited = std::max<std::uint64_t>(1, config_.auto_loose_sample_bytes / block);
    std::uint64_t count = std::min<std::uint64_t>(config_.auto_loose_sample_blocks, byte_limited);
    std::vector<std::uint64_t> indices;
    if (total_blocks <= count) {
        for (std::uint64_t i = 0; i < total_blocks; ++i) indices.push_back(i);
    } else if (count == 1) {
        indices.push_back(total_blocks / 2);
    } else {
        for (std::uint64_t s = 0; s < count; ++s) {
            std::uint64_t v = (s * (total_blocks - 1)) / (count - 1);
            if (indices.empty() || indices.back() != v) indices.push_back(v);
        }
    }
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, open(*item.node));
    CompressParams params = params_for(rule, io_page_for(item.group));
    Compressor compressor(*pool_, *states_);
    std::vector<std::shared_ptr<Block>> blocks;
    for (std::uint64_t index : indices) {
        auto b = std::make_shared<Block>();
        std::uint64_t offset = index * block;
        b->raw.resize(static_cast<std::size_t>(std::min(block, size - offset)));
        ALEMBIC_TRY(file->read_at(offset, b->raw));
        compressor.submit(b, params, false);
        blocks.push_back(b);
    }
    std::uint64_t sampled = 0, stored = 0, raw_blocks = 0;
    for (auto& b : blocks) {
        compressor.wait(*b);
        sampled += b->raw.size();
        stored += b->stored_size;
        raw_blocks += b->codec == kCodecRaw;
    }
    double savings = sampled ? 1.0 - static_cast<double>(stored) / static_cast<double>(sampled) : 0.0;
    double raw_ratio = indices.empty() ? 0.0 : static_cast<double>(raw_blocks) / static_cast<double>(indices.size());
    use_loose = savings < config_.auto_loose_min_savings_ratio || raw_ratio >= config_.auto_loose_max_raw_ratio;
    char text[160];
    std::snprintf(text, sizeof text, " size=%llu saving=%.2f%% rawBlocks=%.2f%%",
                  static_cast<unsigned long long>(size), savings * 100, raw_ratio * 100);
    note = item.rel + text;
    return {};
}

Status PackedTreeImpl::plan(const ProgressFn& progress) {
    // Read back the index we built: file ID = record + 1.
    const std::vector<std::uint8_t>& idx = ampr_index_;
    std::uint64_t count = get_u64(idx.data() + 16), path_bytes = get_u64(idx.data() + 24);
    std::uint64_t paths = 48 + count * 24;
    std::map<std::string, tree::Node*> nodes = index_nodes(root_);
    items_.resize(count);
    std::map<std::string, std::size_t> group_index;
    for (std::size_t g = 0; g < config_.groups.size(); ++g) group_index[config_.groups[g].name] = g;

    Rule default_rule;
    default_rule.action = config_.default_action;
    default_rule.block_shift = config_.default_block_shift;
    default_rule.hc = config_.hc;
    default_rule.level = config_.level;
    default_rule.acceleration = config_.acceleration;
    default_rule.min_savings_bytes = config_.min_savings_bytes;
    default_rule.min_savings_ratio = config_.min_savings_ratio;
    default_rule.io_neutral_min_savings_bytes = config_.io_neutral_min_savings_bytes;
    default_rule.io_neutral_min_savings_ratio = config_.io_neutral_min_savings_ratio;

    std::string index_name = config_.index_name;
    std::string pack_glob = pattern_glob(config_.pack_pattern);
    std::vector<std::string> keep(std::begin(kKeepOnDisk), std::end(kKeepOnDisk));
    forced_patterns_ = config_.force_loose;
    forced_patterns_.insert(forced_patterns_.end(), keep.begin(), keep.end());

    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint8_t* r = idx.data() + 48 + i * 24;
        std::uint32_t po = get_u32(r), pl = get_u32(r + 4);
        if (static_cast<std::uint64_t>(po) + pl >= path_bytes || pl < 7)
            return make_error(Errc::invalid_data, "the generated AMPR index has a bad path");
        Item& item = items_[i];
        item.app0.assign(reinterpret_cast<const char*>(idx.data() + paths + po), pl);
        item.rel = item.app0.substr(6);
        item.index_size = get_u64(r + 8);
        item.index_mtime = static_cast<std::int64_t>(get_u64(r + 16));
        auto found = nodes.find(item.rel);
        item.node = found == nodes.end() ? nullptr : found->second;

        Rule rule = default_rule;
        for (const Rule& candidate : config_.rules)
            if (glob_match_any(candidate.include, item.rel) && !glob_match_any(candidate.exclude, item.rel))
                rule = candidate;
        bool empty = item.index_size == 0;
        if (glob_match_any(config_.force_loose, item.rel) || glob_match_any(keep, item.rel) || empty)
            rule = loose_rule(rule.block_shift);
        if (empty && rule.action == Action::loose) forced_patterns_.push_back(glob_escape(item.rel));
        if (item.rel == index_name || item.rel == index_name + ".crc" || item.rel == index_name + ".runtime" ||
            glob_match(pack_glob, item.rel))
            rule = loose_rule(rule.block_shift);
        item.rule = rule;
        item.group = group_index.count(rule.group) ? group_index[rule.group] : group_index["default"];
        if (item.packed()) {
            if (!item.node) return make_error(Errc::invalid_data, "source is missing for file id " + std::to_string(i + 1));
            if (config_.validate_index_metadata && item.node->size != item.index_size)
                return make_error(Errc::invalid_data, "AMPRIDX3 size mismatch for " + item.rel);
            bool use_loose = false;
            std::string note;
            ALEMBIC_TRY(sample(item, use_loose, note));
            if (use_loose) {
                if (config_.self_contained) {
                    stats_.warnings.push_back("self-contained: auto-loose suppressed for " + note);
                } else {
                    ++stats_.files_auto_loose;
                    stats_.warnings.push_back("auto-loose: " + note);
                    item.rule.action = Action::loose;
                    item.rule.hot = false;
                    item.rule.streaming = false;
                    item.rule.layout = Layout::mixed;
                }
            }
        }
        if (progress && !progress({"LZ4 planning", i + 1, count})) return make_error(Errc::cancelled, "cancelled");
    }

    // _assign_lanes()
    for (std::size_t g = 0; g < config_.groups.size(); ++g) {
        const Group& group = config_.groups[g];
        std::vector<std::size_t> members;
        for (std::size_t i = 0; i < items_.size(); ++i)
            if (items_[i].packed() && items_[i].group == g) members.push_back(i);
        if (group.assignment == Assignment::balanced) {
            std::vector<std::string> lower(items_.size());
            for (std::size_t i : members) lower[i] = ascii_lower(items_[i].rel);
            std::sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) {
                if (items_[a].node->size != items_[b].node->size) return items_[a].node->size > items_[b].node->size;
                if (lower[a] != lower[b]) return lower[a] < lower[b];
                return a < b;
            });
            std::vector<std::uint64_t> loads(group.pack_count, 0);
            for (std::size_t i : members) {
                std::uint32_t best = 0;
                for (std::uint32_t l = 1; l < group.pack_count; ++l)
                    if (loads[l] < loads[best]) best = l;
                items_[i].lane = best;
                loads[best] += items_[i].node->size;
            }
        } else if (group.assignment == Assignment::hash) {
            for (std::size_t i : members) items_[i].lane = static_cast<std::uint32_t>(asset_path_hash(items_[i].rel) % group.pack_count);
        } else {
            for (std::size_t i : members) items_[i].lane = static_cast<std::uint32_t>(i % group.pack_count);
        }
    }
    return {};
}

// ---------------------------------------------------------------- pass 1

struct DedupeTable {
    // Open addressing on {h1, h2, raw crc}; the rest of the key (domain,
    // layout, codec, raw size) is checked on the stored chunk.
    struct Slot {
        std::uint64_t h1 = 0, h2 = 0;
        std::uint32_t crc = 0;
        std::uint32_t chunk = UINT32_MAX;
    };
    std::vector<Slot> slots = std::vector<Slot>(1024);
    std::size_t used = 0;

    template <class Same>
    std::uint32_t find(std::uint64_t h1, std::uint64_t h2, std::uint32_t c, Same&& same) const {
        std::size_t mask = slots.size() - 1;
        for (std::size_t p = h1 & mask;; p = (p + 1) & mask) {
            const Slot& s = slots[p];
            if (s.chunk == UINT32_MAX) return UINT32_MAX;
            if (s.h1 == h1 && s.h2 == h2 && s.crc == c && same(s.chunk)) return s.chunk;
        }
    }
    void insert(std::uint64_t h1, std::uint64_t h2, std::uint32_t c, std::uint32_t chunk) {
        if ((used + 1) * 10 > slots.size() * 7) {
            std::vector<Slot> old = std::move(slots);
            slots.assign(old.size() * 2, Slot{});
            used = 0;
            for (const Slot& s : old)
                if (s.chunk != UINT32_MAX) place(s);
        }
        place(Slot{h1, h2, c, chunk});
    }

private:
    void place(const Slot& s) {
        std::size_t mask = slots.size() - 1;
        std::size_t p = s.h1 & mask;
        while (slots[p].chunk != UINT32_MAX) p = (p + 1) & mask;
        slots[p] = s;
        ++used;
    }
};

Status PackedTreeImpl::pass_one(const ProgressFn& progress) {
    std::uint64_t total_bytes = 0;
    for (const Item& item : items_)
        if (item.packed()) total_bytes += item.node->size;
    std::uint64_t done_bytes = 0;

    std::map<std::pair<std::size_t, std::uint32_t>, std::size_t> current; // (group, lane) -> volume
    std::map<std::pair<std::size_t, std::uint32_t>, std::uint32_t> volume_numbers;
    std::set<std::string> names;
    DedupeTable dedupe;
    Compressor compressor(*pool_, *states_);

    auto new_volume = [&](std::size_t g, std::uint32_t lane, std::uint32_t flags) -> Result<std::size_t> {
        if (volumes_.size() > 0xFFFF) return make_error(Errc::too_large, "pack count exceeds the uint16 format limit");
        const Group& group = config_.groups[g];
        std::uint32_t& number = volume_numbers[{g, lane}];
        VolumePlan v;
        v.pack_id = static_cast<std::uint16_t>(volumes_.size());
        ALEMBIC_ASSIGN(v.name, render_pattern(config_.pack_pattern, group.name, lane, number, v.pack_id));
        ++number;
        std::replace(v.name.begin(), v.name.end(), '\\', '/');
        if (v.name.size() > 1017) return make_error(Errc::invalid_argument, "pack_pattern produced a name that is too long");
        if (!names.insert(v.name).second)
            return make_error(Errc::invalid_argument, "pack_pattern produced a duplicate name: " + v.name);
        if (!detail::is_safe_relative(v.name)) return make_error(Errc::invalid_argument, "unsafe pack name: " + v.name);
        v.group = g;
        v.lane = lane;
        v.io_page_size = io_page_for(g);
        v.chunk_alignment = config_.chunk_alignment;
        v.payload_offset = align_up(kDataHeaderSize, std::max(config_.payload_alignment, v.io_page_size));
        v.position = v.payload_offset;
        v.padding_bytes = v.payload_offset - kDataHeaderSize;
        v.max_pack_size = group.max_pack_size;
        v.flags = flags | kPackIoPageLayout;
        volumes_.push_back(std::move(v));
        current[{g, lane}] = volumes_.size() - 1;
        return volumes_.size() - 1;
    };
    auto writer_for = [&](std::size_t g, std::uint32_t lane, std::uint32_t stored, bool striped, bool streaming,
                          bool extent_start) -> Result<std::size_t> {
        std::uint32_t flags = kPackIoPageLayout | (striped ? kPackStriped : 0);
        auto it = current.find({g, lane});
        std::size_t w;
        if (it == current.end()) {
            ALEMBIC_ASSIGN(w, new_volume(g, lane, flags));
        } else {
            w = it->second;
        }
        if (!volumes_[w].can_fit(stored, streaming, extent_start)) {
            ALEMBIC_ASSIGN(w, new_volume(g, lane, flags));
            if (!volumes_[w].can_fit(stored, streaming, true))
                return make_error(Errc::invalid_argument, "one block exceeds the max pack size of group " +
                                                              config_.groups[g].name);
        }
        if (striped) volumes_[w].flags |= kPackStriped;
        return w;
    };

    for (std::size_t i = 0; i < items_.size(); ++i) {
        Item& item = items_[i];
        if (!item.packed()) continue;
        const Group& group = config_.groups[item.group];
        Layout layout = item.rule.resolved_layout();
        bool streaming = layout == Layout::streaming;
        std::uint64_t block_size = 1ull << item.rule.block_shift;
        std::uint64_t size = item.node->size;
        bool striped = group.stripe_large_files && group.pack_count > 1 && size >= group.stripe_threshold;
        if (manifest_.chunks.size() > 0xFFFFFFFFull) return make_error(Errc::too_large, "too many chunks");
        item.first_chunk = static_cast<std::uint32_t>(manifest_.chunks.size());
        CompressParams params = params_for(item.rule, io_page_for(item.group));
        bool allow_dedupe = config_.deduplicate && (!streaming || config_.deduplicate_streaming);

        ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> file, open(*item.node));
        std::uint64_t total_blocks = div_round_up(size, block_size);
        std::deque<std::shared_ptr<Block>> pending;
        std::size_t window = std::max<std::size_t>(4, pool_->size() * 4);
        std::uint64_t next_read = 0;
        for (std::uint64_t chunk_index = 0; chunk_index < total_blocks; ++chunk_index) {
            while (next_read < total_blocks && pending.size() < window) {
                auto b = std::make_shared<Block>();
                std::uint64_t offset = next_read * block_size;
                b->raw.resize(static_cast<std::size_t>(std::min(block_size, size - offset)));
                ALEMBIC_TRY(file->read_at(offset, b->raw));
                compressor.submit(b, params, allow_dedupe);
                pending.push_back(b);
                ++next_read;
            }
            std::shared_ptr<Block> block = pending.front();
            pending.pop_front();
            compressor.wait(*block);

            std::uint32_t lane = item.lane;
            if (striped) lane = static_cast<std::uint32_t>((item.lane + chunk_index / group.stripe_group_blocks) % group.pack_count);
            std::uint32_t raw_size = static_cast<std::uint32_t>(block->raw.size());
            ChunkRecord record;
            bool shared = false;
            if (allow_dedupe) {
                std::uint32_t prior = dedupe.find(block->h1, block->h2, block->raw_crc, [&](std::uint32_t idx) {
                    const ChunkRecord& p = manifest_.chunks[idx];
                    const VolumePlan& pv = volumes_[p.pack_id];
                    bool same_domain = config_.deduplicate_group_scope ? pv.group == item.group
                                                                       : (pv.group == item.group && pv.lane == lane);
                    bool prior_streaming = p.flags & kChunkStreaming;
                    return same_domain && prior_streaming == streaming && p.codec == block->codec &&
                           p.raw_size == raw_size;
                });
                if (prior != UINT32_MAX) {
                    const ChunkRecord& p = manifest_.chunks[prior];
                    record = p;
                    record.flags = static_cast<std::uint8_t>(kChunkShared | (p.flags & (kChunkPageContained | kChunkPageAligned)) |
                                                             (streaming ? kChunkStreaming : 0));
                    shared = true;
                    ++stats_.chunks_shared;
                    stored_crcs_.push_back(stored_crcs_[prior]);
                }
            }
            if (!shared) {
                bool extent_start = streaming && (chunk_index == 0 || (striped && chunk_index % group.stripe_group_blocks == 0));
                ALEMBIC_ASSIGN(std::size_t w, writer_for(item.group, lane, block->stored_size, striped, streaming, extent_start));
                VolumePlan& v = volumes_[w];
                auto [offset, placement] = v.placement_for(block->stored_size, streaming, extent_start);
                v.padding_bytes += offset - v.position;
                v.position = offset + block->stored_size;
                std::uint32_t flags = placement | (streaming ? kChunkStreaming : 0);
                bool page_safe = block->stored_size <= v.io_page_size ? (placement & kChunkPageContained)
                                                                      : (placement & kChunkPageAligned);
                if (!streaming && !page_safe) return make_error(Errc::invalid_data, "a chunk was not placed page-safely");
                record.offset = offset;
                record.stored_size = block->stored_size;
                record.raw_size = raw_size;
                record.pack_id = v.pack_id;
                record.codec = static_cast<std::uint8_t>(block->codec);
                record.flags = static_cast<std::uint8_t>(flags);
                v.placed.push_back(static_cast<std::uint32_t>(manifest_.chunks.size()));
                if (allow_dedupe)
                    dedupe.insert(block->h1, block->h2, block->raw_crc, static_cast<std::uint32_t>(manifest_.chunks.size()));
                stats_.stored_bytes += block->stored_size;
                stored_crcs_.push_back(block->stored_crc);
            }
            manifest_.chunks.push_back(record);
            raw_crcs_.push_back(block->raw_crc);
            chunk_item_.push_back(static_cast<std::uint32_t>(i));
            ++stats_.chunks;
            stats_.logical_bytes += raw_size;
            if (record.codec == kCodecLz4) ++stats_.chunks_lz4;
            else ++stats_.chunks_raw;
            done_bytes += raw_size;
            if (progress && !progress({"LZ4 pass 1", done_bytes, total_bytes})) return make_error(Errc::cancelled, "cancelled");
        }
        item.chunk_count = static_cast<std::uint32_t>(manifest_.chunks.size() - item.first_chunk);
        ++stats_.files_packed;
    }
    return {};
}

// ---------------------------------------------------------------- finish

Status PackedTreeImpl::finish(std::int64_t now) {
    // Node pointers into root_ go stale once files are added or removed, so
    // keep what pass 2 needs (the input nodes) and the IDs to drop.
    std::set<std::uint64_t> packed_ids;
    item_input_.assign(items_.size(), nullptr);
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].packed()) continue;
        packed_ids.insert(items_[i].node->id);
        item_input_[i] = contents_[items_[i].node->id].input_node;
    }

    std::vector<std::string> sorted_groups;
    for (const Group& g : config_.groups) sorted_groups.push_back(g.name);
    std::sort(sorted_groups.begin(), sorted_groups.end());

    // StringTable: file paths in file-ID order, then pack names.
    std::vector<std::uint8_t>& strings = manifest_.strings;
    std::map<std::string, std::pair<std::uint32_t, std::uint32_t>> offsets;
    auto add_string = [&](const std::string& s) -> Result<std::pair<std::uint32_t, std::uint32_t>> {
        auto it = offsets.find(s);
        if (it != offsets.end()) return it->second;
        if (strings.size() + s.size() + 1 > 0x100000000ull) return make_error(Errc::too_large, "string table too large");
        std::pair<std::uint32_t, std::uint32_t> r{static_cast<std::uint32_t>(strings.size()),
                                                  static_cast<std::uint32_t>(s.size())};
        strings.insert(strings.end(), s.begin(), s.end());
        strings.push_back(0);
        offsets[s] = r;
        return r;
    };
    for (Item& item : items_) {
        auto canonical = detail::canonical_asset_path(item.app0);
        if (!canonical) return make_error(Errc::invalid_data, "path outside /app0: " + item.app0);
        ALEMBIC_ASSIGN(auto at, add_string(*canonical));
        FileRecord r;
        r.path_hash = asset_path_hash(*canonical);
        r.path_offset = at.first;
        r.path_length = at.second;
        if (!item.packed()) {
            r.logical_size = item.index_size;
            r.mtime = item.index_mtime;
            ++stats_.files_loose;
            stats_.loose_bytes += item.index_size;
        } else {
            r.logical_size = item.node->size;
            r.mtime = config_.preserve_mtime ? item.node->mtime : item.index_mtime;
            r.first_chunk = item.first_chunk;
            r.chunk_count = item.chunk_count;
            r.flags = kFilePacked;
            if (item.rule.action == Action::store) r.flags |= kFileStoreOnly;
            Layout layout = item.rule.resolved_layout();
            if (layout == Layout::streaming) r.flags |= kFileStreaming;
            else if (layout == Layout::random) r.flags |= kFileRandomAccess;
            if (item.rule.hot) r.flags |= kFileHot;
            r.block_shift = static_cast<std::uint8_t>(item.rule.block_shift);
            r.packing_class = static_cast<std::uint8_t>(
                std::find(sorted_groups.begin(), sorted_groups.end(), config_.groups[item.group].name) - sorted_groups.begin());
        }
        manifest_.files.push_back(r);
    }
    for (VolumePlan& v : volumes_) {
        ALEMBIC_ASSIGN(auto at, add_string(v.name));
        PackRecord p;
        p.payload_bytes = v.final_size() - v.payload_offset;
        p.file_size = v.final_size();
        p.name_offset = at.first;
        p.name_length = at.second;
        p.flags = v.flags;
        p.io_page_size = v.io_page_size;
        manifest_.packs.push_back(p);
        v.padding_bytes += v.final_size() - v.position;
        stats_.padding_bytes += v.padding_bytes;
        stats_.volume_bytes += v.final_size();
    }
    stats_.volumes = static_cast<std::uint32_t>(volumes_.size());
    stats_.files_total = items_.size();

    // deterministic_build_id()
    std::vector<std::uint8_t> crc_payload(raw_crcs_.size() * 4);
    for (std::size_t i = 0; i < raw_crcs_.size(); ++i) put_u32(crc_payload.data() + i * 4, raw_crcs_[i]);
    std::string config_json = canonical_config_json(config_);
    std::vector<std::uint8_t> parts[] = {
        std::vector<std::uint8_t>(config_json.begin(), config_json.end()),
        detail::encode_file_records(manifest_.files),
        detail::encode_chunk_records(manifest_.chunks),
        crc_payload,
        detail::encode_pack_records(manifest_.packs),
        strings,
    };
    Sha256 sha;
    sha.update(Bytes(reinterpret_cast<const std::uint8_t*>("AMPRPACK4\0"), 10));
    for (const auto& part : parts) {
        std::uint8_t len[8];
        put_u64(len, part.size());
        sha.update(Bytes(len, 8));
        sha.update(part);
    }
    Sha256::Digest digest = sha.finish();
    std::memcpy(manifest_.build_id.data(), digest.data(), 16);

    std::vector<std::uint8_t> manifest_bytes = encode_manifest(manifest_);
    // PS5-LZ4-Compression's runtime_budget(): what is left of the 384 MiB pool.
    if (config_.runtime && config_.runtime_auto_budget) {
        RuntimeSettings& rt = *config_.runtime;
        std::uint64_t pool = 384 * kMiB;
        std::uint64_t fixed = 32 * 2 * kMiB + rt.workers * kMiB + 32 * kMiB;
        std::uint64_t used = fixed + manifest_bytes.size() + ampr_index_.size() + 16 * kMiB;
        std::uint64_t avail = pool > used ? pool - used : 0;
        std::uint64_t physical = std::min(avail / 4, 64 * kMiB);
        std::uint64_t decoded = std::min(avail - physical, 256 * kMiB);
        rt.physical_cache_bytes = physical & ~std::uint64_t{16383};
        rt.decoded_cache_bytes = decoded & ~std::uint64_t{16383};
    }

    auto add_blob = [&](const std::string& rel, std::vector<std::uint8_t> data) {
        auto blob = std::make_shared<const std::vector<std::uint8_t>>(std::move(data));
        blobs_.push_back(blob);
        Content c;
        c.kind = Content::bytes;
        c.index = blobs_.size() - 1;
        add_file(rel, blob->size(), now, c);
    };
    add_blob(config_.index_name, std::move(manifest_bytes));
    add_blob(config_.index_name + ".crc", encode_chunk_crcs(manifest_.build_id, raw_crcs_));
    if (config_.runtime) {
        auto rt = encode_runtime(*config_.runtime, manifest_.build_id);
        add_blob(config_.index_name + ".runtime", std::vector<std::uint8_t>(rt.begin(), rt.end()));
    }
    for (std::size_t v = 0; v < volumes_.size(); ++v) {
        Content c;
        c.kind = Content::volume;
        c.index = v;
        add_file(volumes_[v].name, volumes_[v].final_size(), now, c);
    }

    // Packed originals leave the tree; their data lives in the volumes now.
    std::function<void(tree::Node&)> prune = [&](tree::Node& dir) {
        std::erase_if(dir.children, [&](const tree::Node& n) { return !n.is_directory && packed_ids.count(n.id); });
        for (tree::Node& child : dir.children)
            if (child.is_directory) prune(child);
    };
    prune(root_);
    for (Item& item : items_) item.node = nullptr; // stale from here on
    return {};
}

Status PackedTreeImpl::build(const tree::Source& source, const PackOptions& options, const ProgressFn& progress) {
    source_ = &source;
    if (is_packed(source.root())) return make_error(Errc::invalid_argument, "this tree is already LZ4 packed");
    config_ = options.config;
    ALEMBIC_TRY(validate(config_));
    std::int64_t now = options.timestamp.value_or(static_cast<std::int64_t>(std::time(nullptr)));
    unsigned threads = config_.workers ? config_.workers : std::max(1u, std::thread::hardware_concurrency());
    pool_ = std::make_unique<Pool>(threads);
    states_ = std::make_unique<Lz4States>(pool_->size());

    ALEMBIC_TRY(make_deployed_tree(options, now));
    ALEMBIC_TRY(plan(progress));
    ALEMBIC_TRY(pass_one(progress));
    ALEMBIC_TRY(finish(now));
    return {};
}

// ---------------------------------------------------------------- pass 2

VolumeFile::~VolumeFile() {
    // Jobs hold their blocks; the compressor waits for them.
    compressor_.reset();
}

Result<std::uint64_t> VolumeFile::size() const { return tree_.volumes_[volume_].final_size(); }

// Reads the raw block of placed position `position` and starts compressing it.
Result<std::shared_ptr<Block>> VolumeFile::start(std::size_t position) const {
    const VolumePlan& v = tree_.volumes_[volume_];
    std::uint32_t chunk_index = v.placed[position];
    const ChunkRecord& chunk = tree_.manifest_.chunks[chunk_index];
    std::uint32_t item_index = tree_.chunk_item_[chunk_index];
    const Item& item = tree_.items_[item_index];
    if (open_item_ != item_index) {
        open_file_.reset();
        ALEMBIC_ASSIGN(open_file_, tree_.source_->open(*tree_.item_input_[item_index]));
        open_item_ = item_index;
    }
    auto block = std::make_shared<Block>();
    std::uint64_t local = chunk_index - item.first_chunk;
    block->raw.resize(chunk.raw_size);
    ALEMBIC_TRY(open_file_->read_at(local << item.rule.block_shift, block->raw));
    if (chunk.codec == kCodecRaw) {
        block->raw_crc = crc(block->raw);
        block->codec = kCodecRaw;
        block->stored_size = chunk.raw_size;
        block->stored_crc = block->raw_crc;
        block->done = true;
        return block;
    }
    if (!compressor_) compressor_ = std::make_unique<Compressor>(*tree_.pool_, *tree_.states_);
    block->keep_stored = true;
    compressor_->submit(block, params_for(item.rule, tree_.io_page_for(item.group)), false);
    return block;
}

Status VolumeFile::check(std::size_t position, const Block& block) const {
    const VolumePlan& v = tree_.volumes_[volume_];
    std::uint32_t chunk_index = v.placed[position];
    const ChunkRecord& chunk = tree_.manifest_.chunks[chunk_index];
    if (block.raw_crc != tree_.raw_crcs_[chunk_index] || block.codec != chunk.codec ||
        block.stored_size != chunk.stored_size || block.stored_crc != tree_.stored_crcs_[chunk_index])
        return make_error(Errc::invalid_data, "an LZ4 block came out differently the second time (the source changed?): " +
                                                  tree_.items_[tree_.chunk_item_[chunk_index]].rel);
    return {};
}

// Makes `position` the front of the window and keeps the window running ahead.
Status VolumeFile::fill(std::size_t position) const {
    const VolumePlan& v = tree_.volumes_[volume_];
    while (!window_.empty() && window_.front().position < position) window_.pop_front();
    if (window_.empty() || window_.front().position != position) {
        window_.clear();
        next_ = position;
    }
    std::size_t depth = std::max<std::size_t>(8, tree_.pool_->size() * 4);
    while (next_ < v.placed.size() && window_.size() < depth) {
        ALEMBIC_ASSIGN(std::shared_ptr<Block> block, start(next_));
        window_.push_back({next_, std::move(block)});
        ++next_;
    }
    return {};
}

Status VolumeFile::read_at(std::uint64_t offset, MutableBytes out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const VolumePlan& v = tree_.volumes_[volume_];
    std::uint64_t file_size = v.final_size();
    if (offset > file_size || out.size() > file_size - offset)
        return make_error(Errc::invalid_argument, "read past the end of a pack volume");
    std::fill(out.begin(), out.end(), 0);
    std::uint64_t end = offset + out.size();
    if (offset < kDataHeaderSize) {
        const PackRecord& p = tree_.manifest_.packs[volume_];
        auto header = encode_data_header(v.pack_id, tree_.manifest_.build_id, v.payload_offset, p.payload_bytes, p.flags);
        std::uint64_t n = std::min<std::uint64_t>(kDataHeaderSize, end) - offset;
        std::memcpy(out.data(), header.data() + offset, n);
    }
    const auto& chunks = tree_.manifest_.chunks;
    // First placed chunk that ends after `offset`.
    auto it = std::partition_point(v.placed.begin(), v.placed.end(), [&](std::uint32_t c) {
        return chunks[c].offset + chunks[c].stored_size <= offset;
    });
    for (std::size_t pos = static_cast<std::size_t>(it - v.placed.begin()); pos < v.placed.size(); ++pos) {
        const ChunkRecord& c = chunks[v.placed[pos]];
        if (c.offset >= end) break;
        ALEMBIC_TRY(fill(pos));
        Pending& p = window_.front();
        if (!p.block->done) compressor_->wait(*p.block);
        ALEMBIC_TRY(check(pos, *p.block));
        const std::vector<std::uint8_t>& bytes = p.block->codec == kCodecRaw ? p.block->raw : p.block->stored;
        std::uint64_t from = std::max(offset, c.offset), to = std::min(end, c.offset + c.stored_size);
        std::memcpy(out.data() + (from - offset), bytes.data() + (from - c.offset), to - from);
    }
    return {};
}

} // namespace

Result<std::unique_ptr<PackedTree>> pack(const tree::Source& source, const PackOptions& options,
                                         const ProgressFn& progress) {
    auto tree = std::make_unique<PackedTreeImpl>();
    ALEMBIC_TRY(tree->build(source, options, progress));
    return std::unique_ptr<PackedTree>(std::move(tree));
}

} // namespace alembic::lz4pack
