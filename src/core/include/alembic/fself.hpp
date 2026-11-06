// SPDX-License-Identifier: GPL-3.0-or-later
// Fake-signing. The console's loader refuses a plain PS5 ELF (e_type 0xFE10
// or 0xFE18), so a game whose executables are unsigned never starts. Such a
// game is served here with those files as fake-signed SELFs, byte for byte
// what ampr-emu-ng's tools/make_fself.py --ptype fake writes (its defaults:
// program authority ID 0x3100000000000002, program type 1, versions 0, no
// auth info), as ps5-dump-forge's fself.rs does in every conversion. Ported
// from both (GPL-3.0).
//
// A SELF is planned from the ELF's headers alone and its bytes are made as
// they are read: the headers and meta data, each copied segment (PT_LOAD,
// PT_SCE_RELRO, PT_SCE_DYNLIBDATA, PT_SCE_COMMENT) after a block of zero
// digests, then the PT_SCE_VERSION bytes. Its ex-info holds the SHA-256 of
// the whole ELF, worked out on the first read that reaches it.
#pragma once

#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <memory>
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

} // namespace alembic::fself
