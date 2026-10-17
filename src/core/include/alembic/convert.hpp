// SPDX-License-Identifier: GPL-3.0-or-later
// Converts between dump formats. Every input is opened as a file tree, so any
// supported input can become any supported output without a temporary copy.
#pragma once

#include "alembic/exfat.hpp"
#include "alembic/ffpfsc.hpp"
#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <memory>
#include <optional>
#include <string>

namespace alembic::convert {

enum class Format { folder, exfat, ffpfsc, fpkg };

const char* name(Format format);
// "folder", "exfat", "ffpfsc", "fpkg" (ASCII case ignored).
std::optional<Format> parse_format(const std::string& text);
// From the output path: ".exfat"/".img" → exFAT, ".ffpfsc" → FFPFSC,
// ".pkg" → FPKG, a trailing '/' or an existing folder → folder.
std::optional<Format> guess_output_format(const std::string& path);
// From the input's contents (or that it is a folder).
Result<Format> detect(const std::string& path);

// An opened input: always a file tree, plus the raw image when there is one.
struct Input {
    Format format;
    std::string path;
    std::unique_ptr<tree::Source> tree;
    std::shared_ptr<const RandomAccess> image; // exFAT image or FFPFSC payload; null for folders
    std::optional<ffpfsc::Info> ffpfsc;        // FFPFSC inputs only
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
};

struct Report {
    Format from;
    Format to;
    std::string method; // what was done, for the user
    std::string detail; // e.g. FFPFSC inner name
};

Result<Report> run(const std::string& input, const std::string& output, Format to, const Options& options = {},
                   const ProgressFn& progress = {});

} // namespace alembic::convert
