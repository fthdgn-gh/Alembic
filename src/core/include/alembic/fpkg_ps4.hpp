// SPDX-License-Identifier: GPL-3.0-or-later
// PS4 fake packages, write side: a game tree (a PS4 game's folder or image:
// eboot.bin, sce_sys/param.sfo, ...) to a PS4 package, the way
// maxton/LibOrbisPkg (LGPL-3.0, `6434772`) builds one from the project that
// its pkg_makegp4 extracts: PkgBuilder.cs, PFS/PFSBuilder.cs,
// PFS/PFSCWriter.cs, PFS/FlatPathTable.cs, PKG/Entry.cs, Rif/*.cs,
// SFO/ParamSfo.cs, PlayGo/ChunkDat.cs. See THIRD_PARTY_NOTICES.md.
//
// Layout: CNT header (0x1000, signed) | body (entries from 0x2000) | outer
// PFS (signed, AES-XTS, 0x10000 blocks) whose one file pfs_image.dat is a
// stored PFSC around the inner PFS (unsigned, the game's files). sce_sys
// files with a CNT entry name (param.sfo, icon0.png, ...) become entries
// instead of inner files, like LibOrbisPkg: only those directly in sce_sys.
//
// From an image of a package: its licence (sce_sys/license.dat and
// license.info) is kept; one is generated (a debug RIF) only when missing.
// The PlayGo files and psreserved.dat are always generated again.
#pragma once

#include "alembic/crypto.hpp"
#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace alembic::fpkg::ps4 {

// ---- keys (fpkg_ps4_keys.cpp) ----

// LibOrbisPkg's LibOrbisPkg/Util/Keys.cs at `6434772`: the engine's
// ps4_keys component downloads it; never part of Alembic.
inline constexpr const char* kKeysFileSha256 = "48b44dd03e653a5c0991b0a2975477802ea2639c4e6ea3270b2ff20a1856f8a7";

struct Keys {
    std::array<std::vector<std::uint8_t>, 7> package_public; // PkgPublicKeys: entry keys; [3] signs the header
    std::vector<std::uint8_t> fake_modulus;                  // FakeKeyset.Modulus: wraps the image key
    std::vector<std::uint8_t> rif_modulus;                   // DebugRifKeyset: signs a generated licence
    std::vector<std::uint8_t> rif_private_exponent;
    std::array<std::uint8_t, 16> rif_debug_key{};            // encrypts a generated licence's secret
    std::array<std::uint8_t, 32> keystone_hmac_key{};        // a generated keystone
    std::array<std::uint8_t, 32> keystone_mac_data{};
};
// Reads Keys.cs, checking it is the pinned file (SHA-256 above).
Result<Keys> load_keys(const std::string& keys_cs_path);
// Parses the text of Keys.cs (no hash check).
Result<Keys> parse_keys(std::string_view text);

// sce_sys/keystone for a passcode (Crypto.CreateKeystone).
std::vector<std::uint8_t> create_keystone(const Keys& keys, const std::string& passcode);

// ---- a whole package (fpkg_ps4_build.cpp) ----

struct PackageOptions {
    std::string keys_file; // Keys.cs
    const Keys* keys = nullptr; // or keys already loaded (tests)
    std::string passcode = std::string(32, '0');
    // Every PFS time stamp, in Unix seconds; default: the creation date and
    // time in param.sfo's PUBTOOLINFO (UTC), else the newest file's time.
    std::optional<std::int64_t> timestamp;
    // param.sfo PUBTOOLINFO c_date (YYYYMMDD) and optional c_time (HHMMSS),
    // used when the game's param.sfo carries none; default: today (UTC), no time.
    std::optional<std::string> creation_date;
    std::optional<std::string> creation_time;
    bool overwrite = false;
    unsigned threads = 0; // signing and encryption workers; 0 = every core
};
struct PackageResult {
    std::string content_id;
    std::uint64_t package_length = 0;
    std::size_t inner_files = 0;
    bool generated_license = false;
    bool generated_keystone = false;
    std::vector<std::string> warnings;
};
Result<PackageResult> write_package(const tree::Source& source, const tree::Node& root, const std::string& output_path,
                                    const PackageOptions& options, const ProgressFn& progress = {});

// The CNT entry id of a file name directly in sce_sys, as LibOrbisPkg's
// EntryNames knows it (exposed for tests).
std::optional<std::uint32_t> entry_id_for_name(const std::string& name);
// The same for any path inside sce_sys ("trophy/trophy00.trp"): the whole of
// EntryNames, what pkg_makegp4 extracts entries as.
std::optional<std::uint32_t> entry_id_for_path(const std::string& sce_sys_relative);

} // namespace alembic::fpkg::ps4
