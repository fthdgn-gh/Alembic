// SPDX-License-Identifier: GPL-3.0-or-later
// AMPR seekable LZ4 asset packs (AMPRPAK4 manifest + AMPRDAT3 volumes), read
// on the console by drakmor/ampr_emu's PackedStdio runtime
// (fakelib/libSceAmpr.sprx). Ported from drakmor/ampr_emu's
// tools/ampr_pack.py and ampr_pack_format.py (GPL-3.0); the automatic
// profile and the keep-on-disk list follow PS5-LZ4-Compression's
// src/ampr_pack_profile.c and src/ampr_pack.c (GPL-3.0-or-later).
//
// Packing is a tree -> tree transformation: chosen files are cut into
// independent 16 KiB..1 MiB blocks, compressed with raw LZ4 (or stored), and
// placed in `ampr_assets-*.pak` volumes. Every file keeps its AMPRIDX3 file
// ID, so the packed tree carries the ampr_emu.index built from the original
// tree. The output tree drops the packed originals and adds the manifest,
// its `.crc` and `.runtime` sidecars and the volumes.
//
// Volumes are not written to disk first. Pass 1 compresses every block to fix
// the layout (sizes, offsets, build ID); pass 2 compresses again while the
// writer (folder, exFAT, FFPFSC) reads a volume, and checks each block against
// pass 1. LZ4 is deterministic, so this trades CPU time for not needing a
// temporary copy as large as the game.
#pragma once

#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::lz4pack {

inline constexpr const char* kIndexName = "ampr_assets.index";
inline constexpr const char* kRuntimeRelativePath = "fakelib/libSceAmpr.sprx";

// ---------------------------------------------------------------- format

inline constexpr std::uint32_t kFilePacked = 1u << 0;
inline constexpr std::uint32_t kFileStoreOnly = 1u << 1;
inline constexpr std::uint32_t kFileStreaming = 1u << 2;
inline constexpr std::uint32_t kFileHot = 1u << 3;
inline constexpr std::uint32_t kFileRandomAccess = 1u << 4;

inline constexpr std::uint32_t kCodecRaw = 0;
inline constexpr std::uint32_t kCodecLz4 = 1;
inline constexpr std::uint32_t kChunkShared = 1u << 0;
inline constexpr std::uint32_t kChunkStreaming = 1u << 1;
inline constexpr std::uint32_t kChunkPageContained = 1u << 2;
inline constexpr std::uint32_t kChunkPageAligned = 1u << 3;

inline constexpr std::uint32_t kPackStriped = 1u << 0;
inline constexpr std::uint32_t kPackIoPageLayout = 1u << 1;

inline constexpr std::uint32_t kDataHeaderSize = 64;

using BuildId = std::array<std::uint8_t, 16>;

struct FileRecord {
    std::uint64_t path_hash = 0;
    std::uint64_t logical_size = 0;
    std::int64_t mtime = 0;
    std::uint32_t first_chunk = 0;
    std::uint32_t chunk_count = 0;
    std::uint32_t path_offset = 0;
    std::uint32_t path_length = 0;
    std::uint32_t flags = 0;
    std::uint8_t block_shift = 0;
    std::uint8_t packing_class = 0;
};

struct ChunkRecord {
    std::uint64_t offset = 0;      // absolute, in the volume file
    std::uint32_t stored_size = 0; // 1..1 MiB
    std::uint32_t raw_size = 0;    // derived from the file record when read
    std::uint16_t pack_id = 0;
    std::uint8_t codec = 0;
    std::uint8_t flags = 0;
};

struct PackRecord {
    std::uint64_t payload_bytes = 0;
    std::uint64_t file_size = 0;
    std::uint32_t name_offset = 0;
    std::uint32_t name_length = 0;
    std::uint32_t flags = 0;
    std::uint32_t io_page_size = 0;
};

struct Manifest {
    BuildId build_id{};
    std::vector<FileRecord> files; // file ID = index + 1, same as AMPRIDX3
    std::vector<ChunkRecord> chunks;
    std::vector<PackRecord> packs;
    std::vector<std::uint8_t> strings;

    std::string string_at(std::uint32_t offset, std::uint32_t length) const;
    std::string file_path(std::size_t index) const; // "/app0/..."
    std::string pack_name(std::size_t pack_id) const;
};

std::vector<std::uint8_t> encode_manifest(const Manifest& manifest);
// Parses and fully validates (the same checks as ampr_pack_format.py).
Result<Manifest> parse_manifest(Bytes data);

std::array<std::uint8_t, kDataHeaderSize> encode_data_header(std::uint16_t pack_id, const BuildId& build_id,
                                                             std::uint64_t payload_offset, std::uint64_t payload_bytes,
                                                             std::uint32_t flags);
struct DataHeader {
    std::uint32_t pack_id = 0;
    std::uint32_t flags = 0;
    BuildId build_id{};
    std::uint64_t payload_offset = 0;
    std::uint64_t payload_bytes = 0;
};
Result<DataHeader> parse_data_header(Bytes data);

// <manifest>.crc: one CRC-32 of the decoded bytes per chunk, for offline checks.
std::vector<std::uint8_t> encode_chunk_crcs(const BuildId& build_id, const std::vector<std::uint32_t>& crcs);
Result<std::vector<std::uint32_t>> parse_chunk_crcs(Bytes data, const BuildId& build_id, std::uint64_t chunk_count);

// <manifest>.runtime (AMPRCFG1): cache sizes and worker counts for the runtime.
struct RuntimeSettings {
    std::uint64_t decoded_cache_bytes = 128ull << 20;
    std::uint64_t physical_cache_bytes = 32ull << 20;
    std::uint32_t workers = 4;
    std::uint32_t latency_reserve_workers = 1;
};
Status validate(const RuntimeSettings& settings);
std::array<std::uint8_t, 64> encode_runtime(const RuntimeSettings& settings, const BuildId& build_id);
Result<RuntimeSettings> parse_runtime(Bytes data, const BuildId& build_id);

// AMPRIDX3 helpers shared with the pack code: FNV-1a of the ASCII-folded
// UTF-8 path (standard offset basis, unlike AMPRIDX3's own table), never 0.
std::uint64_t asset_path_hash(std::string_view app0_path);

// Python fnmatch.fnmatchcase on '/'-normalised paths with leading slashes
// stripped: '*' also matches '/', '?' one code point, [seq] and [!seq].
bool glob_match(std::string_view pattern, std::string_view path);
bool glob_match_any(const std::vector<std::string>& patterns, std::string_view path);

// ---------------------------------------------------------------- config

enum class Action { loose, compress, store };
enum class Layout { automatic, random, mixed, streaming };
enum class Assignment { balanced, hash, round_robin };

struct Group {
    std::string name;
    std::uint32_t pack_count = 1;
    Assignment assignment = Assignment::balanced;
    std::uint64_t max_pack_size = 0; // 0 = unlimited
    bool stripe_large_files = false;
    std::uint64_t stripe_threshold = 256ull << 20;
    std::uint32_t stripe_group_blocks = 8;
    std::uint32_t io_page_size = 0; // 0 = the config's
};

struct Rule {
    Action action = Action::compress;
    std::vector<std::string> include{"**"};
    std::vector<std::string> exclude;
    std::uint32_t block_shift = 16;
    std::string group = "default";
    bool hc = true; // LZ4 HC, else LZ4 fast
    int level = 12;
    int acceleration = 1;
    std::uint64_t min_savings_bytes = 64;
    double min_savings_ratio = 0.01;
    std::uint64_t io_neutral_min_savings_bytes = 8192;
    double io_neutral_min_savings_ratio = 0.125;
    Layout layout = Layout::automatic;
    bool streaming = false;
    bool hot = false;
    bool force_pack = false;

    Layout resolved_layout() const;
};

// Mirrors ampr_pack.py's BuildConfig; defaults are its defaults.
struct Config {
    std::optional<RuntimeSettings> runtime;
    std::string index_name = kIndexName;
    std::string pack_pattern = "ampr_assets-{id:03d}.pak";
    Action default_action = Action::loose;
    std::uint32_t default_block_shift = 16;
    std::uint32_t io_page_size = 64 * 1024;
    std::uint32_t payload_alignment = 64 * 1024;
    std::uint32_t chunk_alignment = 64;
    unsigned workers = 0; // 0 = all cores; never part of the output
    bool hc = true;
    int level = 12;
    int acceleration = 1;
    std::uint64_t min_savings_bytes = 64;
    double min_savings_ratio = 0.01;
    std::uint64_t io_neutral_min_savings_bytes = 8192;
    double io_neutral_min_savings_ratio = 0.125;
    bool deduplicate = true;
    bool deduplicate_group_scope = false; // "lane" (false) or "group"
    bool deduplicate_streaming = false;
    bool preserve_mtime = true;
    bool validate_index_metadata = true;
    bool self_contained = false;
    bool auto_loose_large_files = true;
    bool auto_loose_hot_files = false;
    std::uint64_t auto_loose_min_file_size = 64ull << 20;
    std::uint32_t auto_loose_sample_blocks = 32;
    std::uint64_t auto_loose_sample_bytes = 16ull << 20;
    double auto_loose_min_savings_ratio = 0.05;
    double auto_loose_max_raw_ratio = 0.90;
    std::vector<Group> groups; // "default" is added when missing
    std::vector<Rule> rules;   // last match wins
    // Forced loose on top of the rules, like ampr_pack.py's --exclude. Not
    // part of the build ID.
    std::vector<std::string> force_loose;
    // When true the runtime cache sizes are derived from the manifest size,
    // as PS5-LZ4-Compression does (needs `runtime` to hold the worker counts).
    bool runtime_auto_budget = false;
};

Status validate(Config& config); // also adds the "default" group

// PS5-LZ4-Compression's automatic profile: packs everything in sub-folders
// plus root data files, keeps executables, system folders, media and
// already-compressed formats loose (its safety list), four balanced lanes
// of up to 16 GiB. `level` 1-4 means LZ4 fast, 5-12 LZ4 HC.
Config automatic_profile(const tree::Node& root, int level = 9, std::uint32_t block_shift = 16);

// PS5-LZ4-Compression's safety list: patterns that always stay loose
// (executables, system folders, indexes, media and compressed formats).
const std::vector<std::string>& safety_exclusions();
// Adds safety_exclusions() to force_loose, as PS5-LZ4-Compression does for
// every profile.
void add_safety_exclusions(Config& config);

// ---------------------------------------------------------------- profiles

struct TomlOptions {
    // Replaces [pack] compression_level, so rules without their own level
    // inherit it (PS5-LZ4-Compression applies the chosen level this way).
    std::optional<int> level;
    // Reads an include_from / exclude_from file, named relative to the TOML
    // file. Unset: profiles that use them are refused.
    std::function<Result<std::string>(const std::string& relative)> read_file;
};
// An ampr_pack.py TOML configuration (`pack --config`), read the way its
// load_config() reads it. Unknown keys are ignored, as there.
Result<Config> config_from_toml(std::string_view text, const TomlOptions& options = {});

// A console-tested profile shipped with Alembic (profiles/lz4/, written by
// danthespal for MkPFS.CSharp and bundled by PS5-LZ4-Compression).
struct BundledProfile {
    std::string id;
    std::string file;
    std::string label;
    std::string tested; // the game and version it ran on
    std::string engine; // "unity", "insomniac" or ""
    std::vector<std::string> title_ids;
    std::string_view text;
};
const std::vector<BundledProfile>& bundled_profiles();

// "unity", "insomniac" or "" from the file layout.
std::string detect_engine(const tree::Node& root);

struct ProfileMatch {
    const BundledProfile* profile = nullptr;
    bool by_title = false; // else by engine layout
    std::string engine;
};
// The bundled profile for a game: by title ID first, then by engine layout.
ProfileMatch match_bundled_profile(const tree::Node& root, std::string_view title_id);

struct ProfileRequest {
    // "auto" (bundled match, else the scan profile), "scan", a bundled
    // profile id, or a path to a .toml file.
    std::string profile = "auto";
    int level = 9;
    std::optional<std::uint32_t> block_shift; // the scan profile's block size
};
struct ProfileChoice {
    Config config;
    std::string name;        // "scan", a bundled id or the file path
    std::string description; // one line for the log
    bool tested = false;     // a bundled, console-tested profile
    std::vector<std::string> notes;
};
Result<ProfileChoice> choose_profile(const tree::Source& source, const ProfileRequest& request);

// The same configuration as ampr_pack.py TOML (for its `pack --config`).
std::string to_toml(const Config& config);
// The JSON ampr_pack.py hashes into the build ID (`_canonical_config_bytes`).
std::string canonical_config_json(const Config& config);

// ---------------------------------------------------------------- packing

// Checkpoints of pass 1, the long part of packing: what compressing each
// chunk gave (32 bytes per chunk, appended to `sidecar_path`). Everything
// else pass 1 decides follows from those, in order, so a run given them
// replays them without reading or compressing and goes on from there.
struct PackCheckpoints {
    std::string sidecar_path;
    std::uint64_t resume_chunks = 0;    // results in the side file to replay
    std::uint64_t interval = 1ull << 30; // input bytes between checkpoints
    // Called once the side file holds `chunks` results on disk; `done` at the
    // end of pass 1. Also called when stopped.
    std::function<Status(std::uint64_t chunks, bool done)> save;
};

struct PackOptions {
    Config config;
    std::optional<PackCheckpoints> checkpoints;
    // The PackedStdio runtime to place at fakelib/libSceAmpr.sprx (replacing
    // any existing one). Without it the console cannot read packed files.
    std::shared_ptr<const RandomAccess> runtime_module;
    // Modification time for files the packer creates (manifest, sidecars,
    // volumes, runtime). Default: now.
    std::optional<std::int64_t> timestamp;
};

struct PackStats {
    std::uint64_t files_total = 0;
    std::uint64_t files_packed = 0;
    std::uint64_t files_loose = 0;
    std::uint64_t files_auto_loose = 0;
    std::uint64_t chunks = 0;
    std::uint64_t chunks_lz4 = 0;
    std::uint64_t chunks_raw = 0;
    std::uint64_t chunks_shared = 0;
    std::uint64_t logical_bytes = 0; // packed files, before packing
    std::uint64_t stored_bytes = 0;  // unique chunk bytes in the volumes
    std::uint64_t padding_bytes = 0;
    std::uint64_t volume_bytes = 0; // all volume files
    std::uint64_t loose_bytes = 0;
    std::uint32_t volumes = 0;
    std::vector<std::string> warnings; // auto-loose decisions and the like
};

// A packed tree. Its volumes are produced on demand while they are read.
class PackedTree : public tree::Source {
public:
    virtual const PackStats& stats() const = 0;
    virtual const Manifest& manifest() const = 0;
    virtual const BuildId& build_id() const = 0;
    // The configuration as used, runtime cache sizes filled in.
    virtual const Config& config() const = 0;
    // Every pattern that forced a file loose outside the rules (force_loose,
    // the keep-on-disk list, empty files as exact patterns): what
    // ampr_pack.py needs as --exclude-from to make the same pack set.
    virtual const std::vector<std::string>& forced_loose_patterns() const = 0;
};

// Pass 1: plans, samples and compresses every packed block. `source` must
// outlive the result. Progress stages: "LZ4 planning", "LZ4 pass 1".
Result<std::unique_ptr<PackedTree>> pack(const tree::Source& source, const PackOptions& options,
                                         const ProgressFn& progress = {});

// ---------------------------------------------------------------- reading

// True when the root has an ampr_assets.index.
bool is_packed(const tree::Node& root);

struct VerifyResult {
    std::uint64_t files = 0;
    std::uint64_t physical_chunks = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t raw_bytes = 0;
};
// Decodes every unique chunk of a packed tree and checks it against the CRC
// sidecar and the volume headers (ampr_pack.py verify).
Result<VerifyResult> verify(const tree::Source& packed, const ProgressFn& progress = {});

// The original tree back: packed files are served from the volumes; the
// manifest, its sidecars and the volumes are left out.
Result<std::unique_ptr<tree::Source>> unpack(const tree::Source& packed);

} // namespace alembic::lz4pack
