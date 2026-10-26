// SPDX-License-Identifier: GPL-3.0-or-later
// Ported from PS5PKGTool PS5PKGTool.Ffpfsc/FfpfscImage.cs (GPL-3.0).
#include "alembic/ffpfsc.hpp"
#include "alembic/resume.hpp"

#include "alembic/bytes.hpp"
#include "alembic/json.hpp"
#include "alembic/sfo.hpp"
#include "alembic/sha256.hpp"
#include "alembic/tree.hpp"
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <vector>

namespace alembic::ffpfsc {

namespace {

constexpr std::uint64_t kPfsMagic = 20130315;
constexpr std::uint64_t kPfsVersion = 2;
constexpr std::uint16_t kModeCaseInsensitive = 0x8;
constexpr std::uint32_t kInodeSize = 0xA8;
constexpr std::uint16_t kPermissionsRx = 0x16D;
constexpr std::uint16_t kModeDirectory = 0x4000;
constexpr std::uint16_t kModeFile = 0x8000;
constexpr std::uint32_t kFlagCompressed = 0x1;
constexpr std::uint32_t kFlagReadOnly = 0x10;
constexpr std::uint32_t kFlagInternal = 0x20000;
constexpr std::uint32_t kDirentFile = 2;
constexpr std::uint32_t kDirentDirectory = 3;
constexpr std::uint32_t kDirentDot = 4;
constexpr std::uint32_t kDirentDotDot = 5;
constexpr std::uint32_t kPayloadInode = 3;
constexpr std::uint32_t kInodeCount = 4;

struct Inode {
    std::uint16_t mode = 0;
    std::uint16_t link_count = 0;
    std::uint32_t flags = 0;
    std::uint64_t size = 0;
    std::uint64_t size_compressed = 0; // for the payload: the decoded length
    std::uint32_t blocks = 0;
    std::int32_t first_block = 0;
    std::int64_t timestamp = 0;
};

struct Dirent {
    std::uint32_t inode;
    std::uint32_t type;
    std::string name;
};

Error invalid(const std::string& message) {
    return make_error(Errc::invalid_data, message);
}

Status ensure_ascii_name(const std::string& name) {
    for (char c : name)
        if (static_cast<unsigned char>(c) > 0x7F || c == '\0')
            return make_error(Errc::invalid_argument, "PFS file names must be ASCII: " + name);
    return {};
}

Status ensure_plain_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos)
        return make_error(Errc::invalid_argument, "the inner payload name must be a plain file name: " + name);
    return ensure_ascii_name(name);
}

// Java-style string hash over upper-cased characters, as PS5PKGTool's HashPath.
std::uint32_t hash_path(const std::string& ascii_path) {
    std::uint32_t hash = 0;
    for (char c : ascii_path) {
        auto folded = static_cast<std::uint32_t>(c >= 'a' && c <= 'z' ? c - 32 : c);
        hash = folded + 31u * hash;
    }
    return hash;
}

std::vector<std::uint8_t> make_dirent(std::uint32_t inode, std::uint32_t type, const std::string& name) {
    std::size_t size = align_up(name.size() + 17, 8);
    std::vector<std::uint8_t> out(size, 0);
    put_u32(out.data(), inode);
    put_u32(out.data() + 4, type);
    put_u32(out.data() + 8, static_cast<std::uint32_t>(name.size()));
    put_u32(out.data() + 12, static_cast<std::uint32_t>(size));
    std::memcpy(out.data() + 16, name.data(), name.size());
    return out;
}

void append(std::vector<std::uint8_t>& to, const std::vector<std::uint8_t>& data) {
    to.insert(to.end(), data.begin(), data.end());
}

std::vector<std::uint8_t> build_header(std::uint32_t block_size, bool case_insensitive, std::uint64_t block_count,
                                       std::uint32_t inode_blocks, std::int64_t timestamp) {
    std::vector<std::uint8_t> h(block_size, 0);
    put_u64(&h[0x00], kPfsVersion);
    put_u64(&h[0x08], kPfsMagic);
    h[0x1A] = 1;
    put_u16(&h[0x1C], case_insensitive ? kModeCaseInsensitive : 0);
    put_u32(&h[0x20], block_size);
    put_u64(&h[0x28], 1);
    put_u64(&h[0x30], kInodeCount);
    put_u64(&h[0x38], block_count);
    put_u64(&h[0x40], inode_blocks);

    std::uint8_t* sig = &h[0x50]; // the signature inode, unused when unsigned
    put_u16(sig + 0x02, 1);
    put_u32(sig + 0x04, kFlagReadOnly);
    std::uint64_t inode_bytes = static_cast<std::uint64_t>(inode_blocks) * block_size;
    put_u64(sig + 0x08, inode_bytes);
    put_u64(sig + 0x10, inode_bytes);
    for (int i = 0; i < 4; ++i) put_u64(sig + 0x18 + i * 8, static_cast<std::uint64_t>(timestamp));
    put_u32(sig + 0x60, inode_blocks);
    put_u64(sig + 0x88, 1);
    put_u32(&h[0x368], 1);
    return h;
}

std::vector<std::uint8_t> inode_table(const Inode (&inodes)[kInodeCount]) {
    std::vector<std::uint8_t> out(kInodeCount * kInodeSize, 0);
    for (std::uint32_t n = 0; n < kInodeCount; ++n) {
        const Inode& inode = inodes[n];
        std::uint8_t* d = out.data() + n * kInodeSize;
        put_u16(d + 0x00, inode.mode);
        put_u16(d + 0x02, inode.link_count);
        put_u32(d + 0x04, inode.flags);
        put_u64(d + 0x08, inode.size);
        put_u64(d + 0x10, inode.size_compressed);
        for (int i = 0; i < 4; ++i) put_u64(d + 0x18 + i * 8, static_cast<std::uint64_t>(inode.timestamp));
        put_u32(d + 0x60, inode.blocks);
        for (int i = 0; i < 12; ++i) {
            std::int32_t pointer = i == 0 ? inode.first_block : (n == 0 ? 0 : -1);
            put_u32(d + 0x64 + i * 4, static_cast<std::uint32_t>(pointer));
        }
    }
    return out;
}

Inode parse_inode(const std::uint8_t* d) {
    Inode inode;
    inode.mode = get_u16(d);
    inode.link_count = get_u16(d + 2);
    inode.flags = get_u32(d + 4);
    inode.size = get_u64(d + 8);
    inode.size_compressed = get_u64(d + 0x10);
    inode.timestamp = static_cast<std::int64_t>(get_u64(d + 0x18));
    inode.blocks = get_u32(d + 0x60);
    inode.first_block = static_cast<std::int32_t>(get_u32(d + 0x64));
    return inode;
}

Status validate_range(const Inode& inode, std::uint64_t total_blocks, std::uint32_t block_size, const char* label) {
    bool bad = inode.size > INT64_MAX || inode.size_compressed > INT64_MAX || inode.blocks == 0 ||
               inode.first_block <= 0 || static_cast<std::uint64_t>(inode.first_block) >= total_blocks ||
               inode.blocks > total_blocks - static_cast<std::uint64_t>(inode.first_block) ||
               inode.size > static_cast<std::uint64_t>(inode.blocks) * block_size;
    if (bad) return invalid(std::string("the PFS ") + label + " inode has an invalid range");
    return {};
}

Result<std::vector<Dirent>> read_directory(const RandomAccess& source, const Inode& inode, std::uint32_t block_size) {
    std::uint64_t length = std::min<std::uint64_t>(inode.size, static_cast<std::uint64_t>(inode.blocks) * block_size);
    if (length > 64u * 1024 * 1024) return invalid("a PFS directory is too large");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length));
    ALEMBIC_TRY(source.read_at(static_cast<std::uint64_t>(inode.first_block) * block_size, data));
    std::vector<Dirent> out;
    std::size_t offset = 0;
    while (offset + 16 <= data.size()) {
        std::uint32_t ino = get_u32(&data[offset]);
        std::uint32_t type = get_u32(&data[offset + 4]);
        std::uint32_t name_length = get_u32(&data[offset + 8]);
        std::uint32_t entry_size = get_u32(&data[offset + 12]);
        if (ino == 0 && type == 0 && name_length == 0 && entry_size == 0) break;
        if (name_length > INT32_MAX || entry_size > INT32_MAX || entry_size < 16 || (entry_size & 7) != 0 ||
            name_length > entry_size - 16 || entry_size > data.size() - offset)
            return invalid("a PFS directory entry is malformed");
        std::string name(reinterpret_cast<const char*>(&data[offset + 16]), name_length);
        for (char c : name)
            if (static_cast<unsigned char>(c) > 0x7F) return invalid("a PFS file name is not ASCII");
        out.push_back({ino, type, std::move(name)});
        offset += entry_size;
    }
    return out;
}

bool has_dirent(const std::vector<Dirent>& entries, std::uint32_t inode, std::uint32_t type, const char* name) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const Dirent& e) { return e.inode == inode && e.type == type && e.name == name; });
}

Status validate_options(const BuildOptions& options) {
    std::uint32_t b = options.pfs_block_size;
    if (b < 0x1000 || b > 0x100000 || (b & (b - 1)) != 0)
        return make_error(Errc::invalid_argument, "the PFS block size must be a power of two from 4 KiB to 1 MiB");
    if (options.build_timestamp && (*options.build_timestamp < 0 || *options.build_timestamp > 253'402'300'799))
        return make_error(Errc::invalid_argument, "the build timestamp must be a valid Unix time");
    if (options.compression.level < 0 || options.compression.level > 9)
        return make_error(Errc::invalid_argument, "compression level must be between 0 and 9");
    if (options.compression.minimum_gain_percent < 0 || options.compression.minimum_gain_percent > 100)
        return make_error(Errc::invalid_argument, "minimum gain must be between 0 and 100 percent");
    return {};
}

// Pushes the payload, from byte `start` on, into a PFSC encoder positioned in the output file.
using PayloadWriter = Status (*)(void* context, ByteSink& sink, std::uint64_t start);

Result<BuildResult> create_core(std::uint64_t source_length, PayloadWriter write_payload, void* context,
                                const std::string& output_path, const std::string& inner_name,
                                const BuildOptions& options, const char* stage, const ProgressFn& progress) {
    const std::uint32_t block_size = options.pfs_block_size;
    std::int64_t timestamp = options.build_timestamp.value_or(static_cast<std::int64_t>(std::time(nullptr)));

    // Continuing an interrupted build: its time stamp, and where the PFSC
    // payload stopped.
    resume::Session* session = options.resume;
    std::optional<pfsc::Encoder::Checkpoint> from;
    if (session && session->from) {
        const resume::State& s = *session->from;
        auto get = [&](const char* key) { auto it = s.find(key); return it == s.end() ? ~std::uint64_t{0} : it->second; };
        if (get("format") != resume::kFfpfscFormat || get("source_length") != source_length || get("block_size") != block_size ||
            get("blocks") > UINT32_MAX || get("compressed") > UINT32_MAX)
            return make_error(Errc::invalid_data, "the checkpoint does not match this conversion");
        timestamp = static_cast<std::int64_t>(get("timestamp"));
        from = pfsc::Encoder::Checkpoint{static_cast<std::uint32_t>(get("blocks")), get("written"),
                                         static_cast<std::uint32_t>(get("compressed"))};
    }

    std::vector<std::uint8_t> fpt(8);
    put_u32(fpt.data(), hash_path("/" + inner_name));
    put_u32(fpt.data() + 4, kPayloadInode);
    std::vector<std::uint8_t> super_root = make_dirent(1, kDirentFile, "flat_path_table");
    append(super_root, make_dirent(2, kDirentDirectory, "uroot"));
    std::vector<std::uint8_t> user_root = make_dirent(2, kDirentDot, ".");
    append(user_root, make_dirent(2, kDirentDotDot, ".."));
    append(user_root, make_dirent(kPayloadInode, kDirentFile, inner_name));

    const std::uint32_t inode_blocks = static_cast<std::uint32_t>(div_round_up(kInodeCount, block_size / kInodeSize));
    const std::int32_t super_root_block = static_cast<std::int32_t>(1 + inode_blocks);
    const std::int32_t fpt_block = super_root_block + 1;
    const std::int32_t user_root_block = fpt_block + 2; // one reserved block in between
    const std::int32_t payload_block = user_root_block + 1;
    const std::uint64_t payload_offset = static_cast<std::uint64_t>(payload_block) * block_size;

    Inode inodes[kInodeCount] = {
        {static_cast<std::uint16_t>(kModeDirectory | kPermissionsRx), 1, kFlagInternal | kFlagReadOnly, block_size,
         block_size, 1, super_root_block, timestamp},
        {static_cast<std::uint16_t>(kModeFile | kPermissionsRx), 1, kFlagInternal | kFlagReadOnly, fpt.size(),
         fpt.size(), 1, fpt_block, timestamp},
        {static_cast<std::uint16_t>(kModeDirectory | kPermissionsRx), 3, kFlagReadOnly, block_size, block_size, 1,
         user_root_block, timestamp},
        {static_cast<std::uint16_t>(kModeFile | kPermissionsRx), 1, kFlagReadOnly, 0, 0, 1, payload_block, timestamp},
    };

    ALEMBIC_ASSIGN(AtomicOutput output, session ? AtomicOutput::create_partial(output_path, session->partial_path, from.has_value(),
                                                                               options.overwrite_existing)
                                                : AtomicOutput::create(output_path, options.overwrite_existing));
    File& file = output.file();
    ALEMBIC_TRY(file.write_at(0, build_header(block_size, options.case_insensitive, 0, inode_blocks, timestamp)));
    ALEMBIC_TRY(file.write_at(block_size, inode_table(inodes)));
    ALEMBIC_TRY(file.write_at(static_cast<std::uint64_t>(super_root_block) * block_size, super_root));
    ALEMBIC_TRY(file.write_at(static_cast<std::uint64_t>(fpt_block) * block_size, fpt));
    ALEMBIC_TRY(file.write_at(static_cast<std::uint64_t>(user_root_block) * block_size, user_root));

    ProgressFn pfsc_progress;
    if (progress)
        pfsc_progress = [&](const ProgressEvent& e) { return progress({stage, e.done, e.total}); };
    pfsc::Encoder encoder(file, payload_offset, source_length, options.compression, pfsc_progress);
    if (session && session->save)
        encoder.set_checkpoints(session->interval, [&](const pfsc::Encoder::Checkpoint& c) {
            return session->save(resume::State{{"format", resume::kFfpfscFormat},
                                               {"timestamp", static_cast<std::uint64_t>(timestamp)},
                                               {"source_length", source_length},
                                               {"block_size", block_size},
                                               {"blocks", c.blocks},
                                               {"written", c.written},
                                               {"compressed", c.compressed}});
        });
    ALEMBIC_TRY(encoder.begin(from));
    if (Status s = write_payload(context, encoder, from ? static_cast<std::uint64_t>(from->blocks) * pfsc::kBlockSize : 0); !s.ok()) {
        (void)encoder.save_now(); // a pause loses nothing already written
        return s.error();
    }
    ALEMBIC_ASSIGN(pfsc::WriteResult pfsc_result, encoder.finish());

    std::uint64_t payload_blocks = std::max<std::uint64_t>(1, div_round_up(pfsc_result.stored_length, block_size));
    if (payload_blocks > UINT32_MAX) return make_error(Errc::too_large, "the payload is too large for one PFS inode");
    inodes[kPayloadInode].flags = kFlagReadOnly | kFlagCompressed;
    inodes[kPayloadInode].size = pfsc_result.stored_length;
    inodes[kPayloadInode].size_compressed = source_length;
    inodes[kPayloadInode].blocks = static_cast<std::uint32_t>(payload_blocks);
    std::uint64_t block_count = static_cast<std::uint64_t>(payload_block) + payload_blocks;
    ALEMBIC_TRY(file.write_at(0, build_header(block_size, options.case_insensitive, block_count, inode_blocks, timestamp)));
    ALEMBIC_TRY(file.write_at(block_size, inode_table(inodes)));
    ALEMBIC_TRY(file.truncate(block_count * block_size));

    if (progress && !progress({"Verifying", source_length, source_length}))
        return make_error(Errc::cancelled, "cancelled");
    ALEMBIC_ASSIGN(Info info, inspect(file));
    (void)info;
    ALEMBIC_TRY(output.commit());

    BuildResult result;
    result.output_path = output_path;
    result.inner_file_name = inner_name;
    result.source_length = source_length;
    result.pfsc_stored_length = pfsc_result.stored_length;
    result.container_length = block_count * block_size;
    result.pfs_block_size = block_size;
    result.pfsc_block_count = pfsc_result.block_count;
    result.compressed_block_count = pfsc_result.compressed_block_count;
    return result;
}

bool is_title_id(const std::string& value) {
    // ^[A-Za-z]{4}[0-9]{5}$. .NET's '$' would also accept one trailing '\n',
    // which then ends up inside the inner file name; we do not copy that.
    if (value.size() != 9) return false;
    for (int i = 0; i < 4; ++i)
        if (!((value[i] >= 'A' && value[i] <= 'Z') || (value[i] >= 'a' && value[i] <= 'z'))) return false;
    for (int i = 4; i < 9; ++i)
        if (value[i] < '0' || value[i] > '9') return false;
    return true;
}

struct BuilderPayload {
    const exfat::ImageBuilder* builder;
};

Status write_builder_payload(void* context, ByteSink& sink, std::uint64_t start) {
    return static_cast<BuilderPayload*>(context)->builder->write(sink, {}, std::min(start, static_cast<BuilderPayload*>(context)->builder->image_length()));
}

struct ImagePayload {
    const RandomAccess* source;
    std::uint64_t length;
};

Status write_image_payload(void* context, ByteSink& sink, std::uint64_t start) {
    auto* payload = static_cast<ImagePayload*>(context);
    std::vector<std::uint8_t> buffer(1 << 20);
    for (std::uint64_t done = std::min(start, payload->length); done < payload->length;) {
        auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), payload->length - done));
        ALEMBIC_TRY(payload->source->read_at(done, MutableBytes(buffer.data(), n)));
        ALEMBIC_TRY(sink.write(Bytes(buffer.data(), n)));
        done += n;
    }
    return {};
}

Result<std::string> read_small(const tree::Source& source, const tree::Node& file) {
    if (file.size > 16u * 1024 * 1024) return make_error(Errc::too_large, file.name + " is too large");
    ALEMBIC_ASSIGN(std::shared_ptr<const RandomAccess> in, source.open(file));
    std::string data(static_cast<std::size_t>(file.size), '\0');
    ALEMBIC_TRY(in->read_at(0, MutableBytes(reinterpret_cast<std::uint8_t*>(data.data()), data.size())));
    return data;
}

const tree::Node* sce_sys_file(const tree::Node& directory, const char* name) {
    const tree::Node* sce_sys = tree::find_child_icase(directory, "sce_sys");
    if (!sce_sys || !sce_sys->is_directory) return nullptr;
    // PS5PKGTool checks File.Exists, which ignores case on the usual macOS
    // and Windows file systems.
    const tree::Node* param = tree::find_child_icase(*sce_sys, name);
    return param && !param->is_directory ? param : nullptr;
}

const tree::Node* param_json(const tree::Node& directory) {
    return sce_sys_file(directory, "param.json");
}

// A game root: a PS5 game's param.json, else a PS4 game's param.sfo.
const tree::Node* game_param(const tree::Node& directory) {
    if (const tree::Node* p = param_json(directory)) return p;
    return sce_sys_file(directory, "param.sfo");
}

std::string lower_ascii(std::string s) {
    for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return s;
}

} // namespace

Result<const tree::Node*> resolve_game_root(const tree::Node& search_root, std::string* relative_path) {
    if (game_param(search_root)) return &search_root;
    struct Pending {
        const tree::Node* node;
        std::string path;
    };
    std::vector<const tree::Node*> found;
    std::vector<std::string> found_paths;
    std::vector<Pending> pending{{&search_root, ""}};
    while (!pending.empty()) {
        Pending current = pending.back();
        pending.pop_back();
        std::vector<const tree::Node*> children;
        for (const tree::Node& child : current.node->children)
            if (child.is_directory) children.push_back(&child);
        // PS5PKGTool pushes children in descending order, so they are visited ascending.
        std::stable_sort(children.begin(), children.end(), [](const tree::Node* a, const tree::Node* b) {
            return lower_ascii(a->name) > lower_ascii(b->name);
        });
        for (const tree::Node* child : children) {
            std::string child_path = current.path.empty() ? child->name : current.path + "/" + child->name;
            if (unicode::ascii_iequals(child->name, "sce_sys") && game_param(*current.node)) {
                if (std::find(found.begin(), found.end(), current.node) == found.end()) {
                    found.push_back(current.node);
                    found_paths.push_back(current.path);
                }
                if (found.size() > 1) {
                    std::string list;
                    for (const std::string& p : found_paths) list += "\n  " + (p.empty() ? "." : p);
                    return make_error(Errc::invalid_argument,
                                      "several game roots were found; pick one game folder directly:" + list);
                }
                continue;
            }
            pending.push_back({child, child_path});
        }
    }
    if (found.size() == 1) {
        if (relative_path) *relative_path = found_paths[0];
        return found[0];
    }
    if (relative_path) relative_path->clear();
    return &search_root;
}

std::optional<std::string> read_title_id(const tree::Source& source, const tree::Node& game_root) {
    const tree::Node* param = param_json(game_root);
    if (!param) {
        // A PS4 game: TITLE_ID in param.sfo.
        const tree::Node* sfo = sce_sys_file(game_root, "param.sfo");
        if (!sfo) return std::nullopt;
        auto data = read_small(source, *sfo);
        if (!data.ok()) return std::nullopt;
        auto parsed = sfo::parse(Bytes(reinterpret_cast<const std::uint8_t*>(data->data()), data->size()));
        if (!parsed.ok()) return std::nullopt;
        auto id = parsed->get("TITLE_ID");
        if (!id || !is_title_id(*id)) return std::nullopt;
        for (char& c : *id) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        return id;
    }
    auto text = read_small(source, *param);
    if (!text.ok()) return std::nullopt;
    auto properties = json::top_level_properties(*text);
    if (!properties.ok()) return std::nullopt;
    for (const json::Property& p : *properties) {
        if (!unicode::ascii_iequals(p.name, "titleId") || !p.value || !is_title_id(*p.value)) continue;
        std::string id = *p.value;
        for (char& c : id) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        return id;
    }
    return std::nullopt;
}

bool is_ps4_game(const tree::Node& game_root) {
    return !param_json(game_root) && sce_sys_file(game_root, "param.sfo");
}

std::string resolve_dump_image_name(const tree::Source& source, const tree::Node& game_root,
                                    const std::string& folder_name) {
    if (auto id = read_title_id(source, game_root)) return *id + ".exfat";

    // The folder name with anything outside printable ASCII (and the
    // characters exFAT forbids) replaced by '_', per UTF-16 code unit.
    std::string name;
    auto utf16 = unicode::utf8_to_utf16(folder_name);
    std::u16string units = utf16.ok() ? *utf16 : u"";
    for (char16_t c : units) {
        bool keep = c >= u' ' && c <= u'~' && std::u16string_view(u"\"*/:<>?\\|").find(c) == std::u16string_view::npos;
        name += keep ? static_cast<char>(c) : '_';
    }
    std::size_t first = name.find_first_not_of(" .");
    std::size_t last = name.find_last_not_of(" .");
    name = first == std::string::npos ? "" : name.substr(first, last - first + 1);
    return (name.empty() ? "PS5_GAME" : name) + ".exfat";
}

Result<BuildResult> create_from_tree(const tree::Source& source, const tree::Node& search_root,
                                     const std::string& output_path, const BuildOptions& options,
                                     const exfat::BuildOptions& exfat_options, const ProgressFn& progress) {
    ALEMBIC_TRY(validate_options(options));
    if (!options.overwrite_existing && path_exists(output_path))
        return make_error(Errc::invalid_argument, "the output file already exists: " + output_path);
    std::string relative;
    ALEMBIC_ASSIGN(const tree::Node* game_root, resolve_game_root(search_root, &relative));
    std::string folder_name = game_root == &search_root ? (search_root.name.empty() ? source.label() : search_root.name)
                                                        : game_root->name;
    std::string inner_name = options.inner_file_name.value_or(resolve_dump_image_name(source, *game_root, folder_name));
    ALEMBIC_TRY(ensure_plain_name(inner_name));

    ALEMBIC_ASSIGN(exfat::ImageBuilder builder, exfat::ImageBuilder::from_source(source, *game_root, exfat_options));
    BuilderPayload payload{&builder};
    ALEMBIC_ASSIGN(BuildResult result, create_core(builder.image_length(), write_builder_payload, &payload,
                                                   output_path, inner_name, options, "Packing dump", progress));
    result.game_root = relative.empty() ? "." : relative;
    return result;
}

Result<BuildResult> create_from_payload(const RandomAccess& image, const std::string& inner_name,
                                        const std::string& output_path, const BuildOptions& options,
                                        const ProgressFn& progress) {
    ALEMBIC_TRY(validate_options(options));
    if (!options.overwrite_existing && path_exists(output_path))
        return make_error(Errc::invalid_argument, "the output file already exists: " + output_path);
    std::string name = options.inner_file_name.value_or(inner_name);
    ALEMBIC_TRY(ensure_plain_name(name));
    ALEMBIC_ASSIGN(std::uint64_t length, image.size());
    ImagePayload payload{&image, length};
    return create_core(length, write_image_payload, &payload, output_path, name, options, "Compressing", progress);
}

Result<Payload> open_payload(std::shared_ptr<const RandomAccess> container) {
    Payload payload;
    ALEMBIC_ASSIGN(payload.info, inspect(*container));
    ALEMBIC_ASSIGN(std::shared_ptr<pfsc::Reader> reader,
                   pfsc::Reader::open(std::move(container), payload.info.payload_offset, payload.info.stored_length,
                                      payload.info.logical_length));
    payload.image = std::move(reader);
    return payload;
}

Result<BuildResult> create_from_directory(const std::string& source_directory, const std::string& output_path,
                                          const BuildOptions& options, const exfat::BuildOptions& exfat_options,
                                          const ProgressFn& progress) {
    ALEMBIC_TRY(validate_options(options));
    if (!is_directory(source_directory)) return make_error(Errc::invalid_argument, "not a folder: " + source_directory);
    if (!options.overwrite_existing && path_exists(output_path))
        return make_error(Errc::invalid_argument, "the output file already exists: " + output_path);
    ALEMBIC_ASSIGN(std::string source_full, absolute_path(source_directory));
    ALEMBIC_ASSIGN(std::string output_full, absolute_path(output_path));
    if (output_full == source_full || output_full.rfind(source_full + "/", 0) == 0)
        return make_error(Errc::invalid_argument, "save the FFPFSC image outside the source dump folder");
    ALEMBIC_ASSIGN(std::unique_ptr<tree::Source> folder, tree::open_folder(source_full));
    return create_from_tree(*folder, folder->root(), output_path, options, exfat_options, progress);
}

Result<BuildResult> create_from_image(const std::string& source_path, const std::string& output_path,
                                      const BuildOptions& options, const ProgressFn& progress) {
    if (!is_regular_file(source_path)) return make_error(Errc::invalid_argument, "not a file: " + source_path);
    ALEMBIC_ASSIGN(std::string source_full, absolute_path(source_path));
    ALEMBIC_ASSIGN(std::string output_full, absolute_path(output_path));
    if (source_full == output_full)
        return make_error(Errc::invalid_argument, "the output path must differ from the source image");
    ALEMBIC_ASSIGN(File source, File::open(source_path, File::Mode::read));
    return create_from_payload(source, base_name(source_path), output_path, options, progress);
}

Result<Info> inspect(const RandomAccess& source) {
    ALEMBIC_ASSIGN(std::uint64_t file_size, source.size());
    if (file_size < 0x1000) return invalid("the PFS image is too small");
    std::uint8_t h[0x50];
    ALEMBIC_TRY(source.read_at(0, MutableBytes(h, sizeof h)));
    std::uint64_t version = get_u64(h + 0x00);
    std::uint64_t magic = get_u64(h + 0x08);
    std::uint16_t mode = get_u16(h + 0x1C);
    std::uint32_t block_size = get_u32(h + 0x20);
    std::uint64_t leading_blocks = get_u64(h + 0x28);
    std::uint64_t inode_count = get_u64(h + 0x30);
    std::uint64_t block_count = get_u64(h + 0x38);
    std::uint64_t inode_blocks = get_u64(h + 0x40);

    if (version != kPfsVersion || magic != kPfsMagic) return invalid("unsupported PFS header");
    if ((mode & ~kModeCaseInsensitive) != 0) return invalid("only unsigned, unencrypted PFS images are supported");
    if (block_size < 0x1000 || block_size > 0x100000 || (block_size & (block_size - 1)) != 0)
        return invalid("the PFS block size is invalid");
    if (leading_blocks != 1 || inode_count < kInodeCount || inode_count > block_size / kInodeSize)
        return invalid("the PFS inode layout is unsupported");
    if (inode_blocks != 1 || block_count == 0 || block_count > file_size / block_size)
        return invalid("the PFS block ranges are invalid");
    if (file_size != block_count * block_size) return invalid("the PFS file length does not match its block count");

    std::vector<std::uint8_t> table(kInodeCount * kInodeSize);
    ALEMBIC_TRY(source.read_at(block_size, table));
    Inode inodes[kInodeCount];
    for (std::uint32_t i = 0; i < kInodeCount; ++i) inodes[i] = parse_inode(table.data() + i * kInodeSize);
    ALEMBIC_TRY(validate_range(inodes[0], block_count, block_size, "super root"));
    ALEMBIC_TRY(validate_range(inodes[1], block_count, block_size, "flat path table"));
    ALEMBIC_TRY(validate_range(inodes[2], block_count, block_size, "user root"));
    ALEMBIC_TRY(validate_range(inodes[3], block_count, block_size, "payload"));

    ALEMBIC_ASSIGN(std::vector<Dirent> super_root, read_directory(source, inodes[0], block_size));
    if (!has_dirent(super_root, 1, kDirentFile, "flat_path_table") || !has_dirent(super_root, 2, kDirentDirectory, "uroot"))
        return invalid("the PFS super root is missing an entry");
    ALEMBIC_ASSIGN(std::vector<Dirent> user_root, read_directory(source, inodes[2], block_size));
    if (!has_dirent(user_root, 2, kDirentDot, ".") || !has_dirent(user_root, 2, kDirentDotDot, ".."))
        return invalid("the PFS user root is missing '.' or '..'");
    const Dirent* payload = nullptr;
    for (const Dirent& e : user_root) {
        if (e.inode != kPayloadInode || e.type != kDirentFile) continue;
        if (payload) return invalid("the PFS user root has more than one payload entry");
        payload = &e;
    }
    if (!payload) return invalid("the PFS user root has no payload file");

    if ((inodes[3].flags & kFlagCompressed) == 0) return invalid("the PFS payload is not PFSC-compressed");
    if (inodes[1].size != 8) return invalid("the PFS flat path table does not match the payload entry");
    std::uint8_t fpt[8];
    ALEMBIC_TRY(source.read_at(static_cast<std::uint64_t>(inodes[1].first_block) * block_size, MutableBytes(fpt, 8)));
    if (get_u32(fpt) != hash_path("/" + payload->name) || get_u32(fpt + 4) != kPayloadInode)
        return invalid("the PFS flat path table does not match the payload entry");

    Info info;
    info.payload_offset = static_cast<std::uint64_t>(inodes[3].first_block) * block_size;
    ALEMBIC_ASSIGN(info.pfsc, pfsc::inspect(source, info.payload_offset, inodes[3].size));
    if (info.pfsc.padded_logical_length < inodes[3].size_compressed ||
        info.pfsc.padded_logical_length - inodes[3].size_compressed >= pfsc::kBlockSize)
        return invalid("the PFSC logical size does not match the payload inode");

    info.pfs_version = static_cast<std::uint32_t>(version);
    info.pfs_mode = mode;
    info.pfs_block_size = block_size;
    info.pfs_block_count = block_count;
    info.inode_count = static_cast<std::uint32_t>(inode_count);
    info.inner_file_name = payload->name;
    info.stored_length = inodes[3].size;
    info.logical_length = inodes[3].size_compressed;
    info.timestamp = inodes[3].timestamp;
    return info;
}

Status extract_payload(const std::string& ffpfsc_path, const std::string& output_path, const ProgressFn& progress) {
    ALEMBIC_ASSIGN(std::string input_full, absolute_path(ffpfsc_path));
    ALEMBIC_ASSIGN(std::string output_full, absolute_path(output_path));
    if (input_full == output_full) return make_error(Errc::invalid_argument, "the output must differ from the input");
    ALEMBIC_ASSIGN(File input, File::open(ffpfsc_path, File::Mode::read));
    ALEMBIC_ASSIGN(Info info, inspect(input));
    ALEMBIC_ASSIGN(AtomicOutput output, AtomicOutput::create(output_path));
    FileSink sink(output.file(), 0);
    ProgressFn decode_progress;
    if (progress) decode_progress = [&](const ProgressEvent& e) { return progress({"Extracting", e.done, e.total}); };
    ALEMBIC_TRY(pfsc::decode(input, sink, info.logical_length, info.payload_offset, info.stored_length, decode_progress));
    return output.commit();
}

Result<VerifyResult> verify(const std::string& ffpfsc_path, const std::optional<std::string>& original,
                            const ProgressFn& progress) {
    ALEMBIC_ASSIGN(File input, File::open(ffpfsc_path, File::Mode::read));
    VerifyResult result;
    ALEMBIC_ASSIGN(result.info, inspect(input));
    Sha256 decoded;
    ProgressFn decode_progress;
    if (progress) decode_progress = [&](const ProgressEvent& e) { return progress({"Verifying", e.done, e.total}); };
    ALEMBIC_TRY(pfsc::decode(input, decoded, result.info.logical_length, result.info.payload_offset,
                             result.info.stored_length, decode_progress));
    result.decoded_sha256 = Sha256::hex(decoded.finish());

    if (original) {
        ALEMBIC_ASSIGN(File source, File::open(*original, File::Mode::read));
        ALEMBIC_ASSIGN(std::uint64_t length, source.size());
        Sha256 hash;
        std::vector<std::uint8_t> buffer(1 << 20);
        for (std::uint64_t done = 0; done < length;) {
            auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), length - done));
            ALEMBIC_TRY(source.read_at(done, MutableBytes(buffer.data(), n)));
            hash.update(Bytes(buffer.data(), n));
            done += n;
        }
        result.source_sha256 = Sha256::hex(hash.finish());
        result.source_matches = length == result.info.logical_length && *result.source_sha256 == result.decoded_sha256;
    }
    return result;
}

} // namespace alembic::ffpfsc
