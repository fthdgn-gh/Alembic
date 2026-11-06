// SPDX-License-Identifier: GPL-3.0-or-later
// Fake-signing. The console's loader refuses a plain PS5 ELF (e_type 0xFE10
// or 0xFE18), so a game whose executables are unsigned never starts. Such a
// game is served here with those files as fake-signed SELFs, byte for byte
// what ampr-emu-ng's tools/make_fself.py --ptype fake writes (its defaults:
// program authority ID 0x3100000000000002, program type 1, versions 0, no
// auth info), as ps5-dump-forge's fself.rs does in every conversion. Ported
// from both (GPL-3.0). Packages use ProsperoPkgTool's flavour (below).
//
// A SELF is planned from the ELF's headers alone and its bytes are made as
// they are read: the headers and meta data, each copied segment (PT_LOAD,
// PT_SCE_RELRO, PT_SCE_DYNLIBDATA, PT_SCE_COMMENT) after a block of zero
// digests, then the PT_SCE_VERSION bytes. Its ex-info holds the SHA-256 of
// the whole ELF, worked out on the first read that reaches it.
#pragma once

#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::fself {

// What may be an executable, by name: .elf, .self, .prx, .sprx, and a .bin
// named eboot* (any case). Data .bin files are never opened.
bool is_executable_name(std::string_view path);

// Whether `head` (at least 0x12 bytes) starts a plain PS5 ELF: the ELF magic
// and e_type 0xFE10 (ET_SCE_EXEC_ASLR) or 0xFE18 (ET_SCE_DYNAMIC).
bool is_unsigned_elf(Bytes head);

// The unsigned executables of `source` ('/'-separated paths, tree order), at
// most 4096 of them; `fakelib*/` included (the loader loads those too).
Result<std::vector<std::string>> find_unsigned(const tree::Source& source);

// The SELF of a whole ELF in memory (tests, and a small file's way).
Result<std::vector<std::uint8_t>> make_fself(Bytes elf);

struct Signed {
    // `source` with its unsigned executables served as SELFs at their SELF
    // sizes; null when it has none that could be signed.
    std::unique_ptr<tree::Source> tree;
    std::vector<std::string> signed_paths;
    // An unsigned executable that could not be planned, left as it is, and why.
    std::vector<std::pair<std::string, std::string>> refused;
    // Records of the game's own ampr_emu.index set to the SELF sizes (the
    // runtime answers /app0 lookups from it, and a kept index must agree).
    std::size_t index_records_resized = 0;
};
Result<Signed> sign(const tree::Source& source);

// ---- ProsperoPkgTool's flavour (Content/ProsperoSelfBuilder.cs, MIT) ----
// What img_create applies to a package's modules (FakeSignInnerModules):
// the PS5 SELF magic, program type 0x10000101, an authority ID derived from
// the ELF (its type and the byte at 0x3F00), each segment's data right after
// its digests, and the module's .sceversion section appended, or for a
// stripped module one made from eboot.bin's record. Errors are
// Errc::unsupported: img_create packs such a file unchanged.

// img_create's IsFakeSignTarget: a file named eboot.bin, *.elf, *.prx or
// *.sprx (any case), anywhere in the game.
bool is_prospero_target(std::string_view path);

using VersionRecord = std::array<std::uint8_t, 8>;

// TryGetSceVersionRecord: the first 8-byte version tuple of an ELF's
// .sceversion section or of the trailer after a SELF's file size; none when
// there is no such record (an error when the ELF's sections overrun it).
Result<std::optional<VersionRecord>> sceversion_record(const RandomAccess& image);

struct ProsperoOptions {
    // For a stripped module (no .sceversion): its file name without the
    // extension and the application's record, to make one from.
    std::string sceversion_name;
    std::optional<VersionRecord> sceversion_record;
};
Result<std::vector<std::uint8_t>> make_prospero_fself(Bytes elf, const ProsperoOptions& options = {});

} // namespace alembic::fself
