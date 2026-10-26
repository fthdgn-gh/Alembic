// SPDX-License-Identifier: GPL-3.0-or-later
// FFPFSC: an unsigned, unencrypted PS5 PFS image holding one PFSC-compressed
// payload (normally an exFAT image of a game dump). Ported from PS5PKGTool's
// FfpfscImage.cs; output is byte-identical to it for the same timestamp.
//
// Layout, in PFS blocks of `pfs_block_size` bytes (default 64 KiB):
//   0  PFS header (version 2, magic 20130315)
//   1  inode table: 0 super root, 1 flat_path_table, 2 uroot, 3 payload
//   2  super root directory: flat_path_table, uroot
//   3  flat path table: {u32 hash("/" + name), u32 inode 3}
//   4  reserved (zero)
//   5  uroot directory: ".", "..", <payload name>
//   6+ PFSC container (see pfsc.hpp), padded to a whole block
#pragma once

#include "alembic/exfat.hpp"
#include "alembic/io.hpp"
#include "alembic/pfsc.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace alembic::resume {
struct Session;
}

namespace alembic::ffpfsc {

struct BuildOptions {
    std::uint32_t pfs_block_size = 0x10000; // power of two, 4 KiB to 1 MiB
    bool case_insensitive = true;           // PFS mode flag 0x8
    bool overwrite_existing = false;
    // Name of the payload inside the image (ASCII). Default: from the source
    // file name, or "<titleId>.exfat" from sce_sys/param.json for a dump.
    std::optional<std::string> inner_file_name;
    // Fixed PFS timestamp for reproducible output; default is the current time.
    std::optional<std::int64_t> build_timestamp;
    pfsc::CompressionOptions compression;
    // Resumable write (see resume.hpp): the partial file and checkpoints. The
    // caller keeps the session alive for the call.
    resume::Session* resume = nullptr;
};

struct BuildResult {
    std::string output_path;
    std::string inner_file_name;
    std::string game_root; // tree builds: where sce_sys/param.json was found ("." = top)
    std::uint64_t source_length = 0;
    std::uint64_t pfsc_stored_length = 0;
    std::uint64_t container_length = 0;
    std::uint32_t pfs_block_size = 0;
    std::uint32_t pfsc_block_count = 0;
    std::uint32_t compressed_block_count = 0;
};

struct Info {
    std::uint32_t pfs_version = 0;
    std::uint32_t pfs_mode = 0;
    std::uint32_t pfs_block_size = 0;
    std::uint64_t pfs_block_count = 0;
    std::uint32_t inode_count = 0;
    std::string inner_file_name;
    std::uint64_t payload_offset = 0;
    std::uint64_t stored_length = 0;  // PFSC container bytes
    std::uint64_t logical_length = 0; // decoded payload bytes
    std::int64_t timestamp = 0;
    pfsc::Info pfsc;
};

struct VerifyResult {
    Info info;
    std::string decoded_sha256;
    std::optional<std::string> source_sha256; // when an original was given
    std::optional<bool> source_matches;
};

// Packs a file tree: the exFAT image is streamed straight into the PFSC
// encoder, so no temporary image is written. If `search_root` has no
// sce_sys/param.json, the one sub-folder (any depth) that has it is packed.
Result<BuildResult> create_from_tree(const tree::Source& source, const tree::Node& search_root,
                                     const std::string& output_path, const BuildOptions& options = {},
                                     const exfat::BuildOptions& exfat_options = {}, const ProgressFn& progress = {});

// Same, for a folder on disk. Refuses an output inside the folder.
Result<BuildResult> create_from_directory(const std::string& source_directory, const std::string& output_path,
                                          const BuildOptions& options = {},
                                          const exfat::BuildOptions& exfat_options = {},
                                          const ProgressFn& progress = {});

// Wraps an image as-is (exFAT, UFS2, ...). `inner_name` is used unless
// options.inner_file_name is set.
Result<BuildResult> create_from_payload(const RandomAccess& image, const std::string& inner_name,
                                        const std::string& output_path, const BuildOptions& options = {},
                                        const ProgressFn& progress = {});

// Wraps an image file; the inner name defaults to the file name.
Result<BuildResult> create_from_image(const std::string& source_path, const std::string& output_path,
                                      const BuildOptions& options = {}, const ProgressFn& progress = {});

// Random access to the decoded payload of an FFPFSC.
struct Payload {
    Info info;
    std::shared_ptr<const RandomAccess> image;
};
Result<Payload> open_payload(std::shared_ptr<const RandomAccess> container);

Result<Info> inspect(const RandomAccess& source);

// Writes the decoded payload (the inner image) to a new file.
Status extract_payload(const std::string& ffpfsc_path, const std::string& output_path,
                       const ProgressFn& progress = {});

// Decodes every block and hashes the payload; compares with `original` if given.
Result<VerifyResult> verify(const std::string& ffpfsc_path, const std::optional<std::string>& original = std::nullopt,
                            const ProgressFn& progress = {});

// The game root PS5PKGTool would pick inside `search_root`; `relative_path`
// receives its path ("" for the root itself). A folder with sce_sys/param.sfo
// and no param.json (a PS4 game) counts as a root too.
Result<const tree::Node*> resolve_game_root(const tree::Node& search_root, std::string* relative_path = nullptr);
// The titleId of sce_sys/param.json in `game_root` (upper case), if valid;
// for a PS4 game, TITLE_ID from sce_sys/param.sfo.
std::optional<std::string> read_title_id(const tree::Source& source, const tree::Node& game_root);
// Which console a game is for, in this order: its title ID (CUSA: PS4, PPSA:
// PS5; other prefixes are homebrew or system software and say nothing), the
// SDK eboot.bin was built with, then its metadata (param.sfo with no
// param.json, or one without a contentId: PS4; param.json: PS5).
enum class Platform { unknown, ps4, ps5 };
Platform game_platform(const tree::Source& source, const tree::Node& game_root);
// An executable's process parameters (PT_SCE_PROCPARAM, "ORBI"), read from an
// ELF or a SELF with either magic. Their 64-bit SDK field holds the PS4 SDK in
// its low half; PS5 executables put their SDK in the high half (PS4 ones leave
// it 0, whatever the parameters' version). Ported from drakmor/ShadowMountPlus
// (GPL-3.0) src/sm_pkg.c read_executable_sdk.
struct ProcessParams {
    std::uint32_t version = 0; // 0: not readable
    std::uint32_t ps4_sdk = 0; // e.g. 0x11008001 for 11.00
    std::uint32_t ps5_sdk = 0; // e.g. 0x04000031 for 4.00; 0 on PS4
    Platform platform() const {
        return !version ? Platform::unknown : ps5_sdk ? Platform::ps5 : ps4_sdk ? Platform::ps4 : Platform::unknown;
    }
};
ProcessParams executable_process_params(const RandomAccess& executable);
// A PS4 game Alembic can handle as one: game_platform says PS4 and it has
// sce_sys/param.sfo.
bool is_ps4_game(const tree::Source& source, const tree::Node& game_root);
// A PS4 game's tree with the sce_sys/param.json it needs to start from an
// image or folder (sfo::ps4_param_json); null when it already has one or is
// not a PS4 game.
Result<std::unique_ptr<tree::Source>> with_ps4_param_json(const tree::Source& source);
// "<TITLEID>.exfat" from sce_sys/param.json, else `folder_name` sanitized + ".exfat".
std::string resolve_dump_image_name(const tree::Source& source, const tree::Node& game_root,
                                    const std::string& folder_name);

} // namespace alembic::ffpfsc
