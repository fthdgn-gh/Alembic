// SPDX-License-Identifier: GPL-3.0-or-later
// Converts between dump formats. Every input is opened as a file tree, so any
// supported input can become any supported output without a temporary copy.
#pragma once

#include "alembic/ampr_emu.hpp"
#include "alembic/exfat.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/fpkg_build.hpp"
#include "alembic/io.hpp"
#include "alembic/lz4pack.hpp"
#include "alembic/resume.hpp"
#include "alembic/tree.hpp"
#include "alembic/ufs2.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace alembic::convert {

enum class Format { folder, exfat, ffpfsc, fpkg, ffpkg, ffpfs };

const char* name(Format format);
// "folder", "exfat", "ffpfsc", "fpkg", "ffpkg", "ffpfs" (ASCII case ignored).
std::optional<Format> parse_format(const std::string& text);
// From the output path: ".exfat"/".img" → exFAT, ".ffpfsc" → FFPFSC,
// ".pkg" → FPKG, ".ffpkg" → FFPKG (UFS2), ".ffpfs" → PFS, a trailing '/' or an existing folder → folder.
std::optional<Format> guess_output_format(const std::string& path);
// From the input's contents (or that it is a folder).
Result<Format> detect(const std::string& path);

// Inputs that are not files on this machine (for the engine: a file a
// browser on another computer shares). The provider returns the file for
// such a path, else null; detect(), open() and run() then read it like a
// file on disk. Only file inputs (packages and images), not folders.
using FileProvider = std::function<std::shared_ptr<const RandomAccess>(const std::string& path)>;
void set_file_provider(FileProvider provider);
bool is_provided(const std::string& path);

// An opened input: always a file tree, plus the raw image when there is one.
struct Input {
    Format format;
    std::string path;
    std::unique_ptr<tree::Source> tree;
    std::shared_ptr<const RandomAccess> image; // exFAT/UFS2/PFS image or FFPFSC payload; null for folders and FPKG
    std::optional<ffpfsc::Info> ffpfsc;        // FFPFSC inputs only
    bool image_is_ufs2 = false;                // the image is UFS2 rather than exFAT
    bool image_is_pfs = false;                 // the image is PFS rather than exFAT
    std::string detail;                        // e.g. the FPKG layout dialect
};
// `passcode` is used for FPKG inputs (default: 32 zeros).
Result<Input> open(const std::string& path, const std::string& passcode = std::string(32, '0'));

struct Options {
    // Rebuild the exFAT image from the file tree instead of copying the
    // existing image byte for byte. Rebuilt images use the deterministic
    // MkPFS/PS5PKGTool layout (and refresh ampr_emu.index).
    bool rebuild = false;
    bool overwrite = false;
    std::string passcode = std::string(32, '0'); // FPKG inputs
    exfat::BuildOptions exfat;
    ffpfsc::BuildOptions ffpfsc;
    // FFPKG output. An empty volume name becomes the title ID (else the output name).
    ufs2::BuildOptions ufs2;
    // .ffpfs output takes its block size, time stamp and case mode from
    // `ffpfsc` (pfs_block_size, build_timestamp, case_insensitive).
    // Pack the tree into LZ4 asset packs on the way (folder, exFAT and FFPFSC
    // outputs; images are rebuilt).
    std::optional<lz4pack::PackOptions> lz4;
    // FPKG output: keys_folder is required; `overwrite` comes from above.
    fpkg::build::PackageOptions fpkg;
    // Resumable output (every target, LZ4 packing included; see resume.hpp).
    resume::Session* resume = nullptr;
    // Serve LZ4-packed files as ordinary files again (drops the packs).
    bool lz4_unpack = false;
    // ampr_emu's libSceAmpr.sprx (ampr_emu.hpp), added to the game's fakelib
    // folder when eboot.bin imports libSceAmpr and the game ships none. Every
    // output but FPKG (an installed package has the console's own APR); LZ4
    // packing adds its runtime by itself. Images are then rebuilt.
    std::shared_ptr<const RandomAccess> ampr_emulator;
    bool ampr_emulator_always = false; // add it even when the game does not seem to need it
};

struct Report {
    Format from = Format::folder;
    Format to = Format::folder;
    std::string method; // what was done, for the user
    std::string detail; // e.g. FFPFSC inner name
    std::optional<lz4pack::PackStats> lz4; // set when packing
    std::string lz4_config_toml;            // the pack configuration, as ampr_pack.py TOML
    std::vector<std::string> lz4_forced_loose;
    std::vector<std::string> warnings;
    bool resumable = false; // the session was used
    bool ampr_needed = false;       // the game imports libSceAmpr and ships no emulator
    bool ampr_emulator_added = false;
    std::string ampr_emulator_path; // where it went, e.g. "fakelib/libSceAmpr.sprx"
    std::string title_id;           // from sce_sys/param.json, when it has one
};

Result<Report> run(const std::string& input, const std::string& output, Format to, const Options& options = {},
                   const ProgressFn& progress = {});

// Compares the files of two inputs byte for byte. LZ4-packed inputs are
// unpacked first; ampr_emu.index is ignored (outputs rebuild it).
struct Comparison {
    std::uint64_t files = 0; // compared (present in both)
    std::uint64_t bytes = 0;
    std::vector<std::string> only_in_a;
    std::vector<std::string> only_in_b;
    std::vector<std::string> different; // size or content
    bool same() const { return only_in_a.empty() && only_in_b.empty() && different.empty(); }
};
Result<Comparison> compare(const std::string& a, const std::string& b, const std::string& passcode = std::string(32, '0'),
                           const ProgressFn& progress = {});

} // namespace alembic::convert
