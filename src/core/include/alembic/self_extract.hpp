// SPDX-License-Identifier: GPL-3.0-or-later
// Turn an encrypted SELF (a PS5/PS4 signed executable) back into the plain ELF
// it wraps. The console does the decrypting: this code parses the SELF,
// copies the plaintext parts, and asks a callback for each encrypted segment's
// bytes (on the PS5 the engine maps it through the kernel's own loader path;
// see src/engine/selfdump_ps5.cpp). The assembly here is portable and tested
// on a PC with a stand-in callback.
//
// Ported from ps5-payload-dev/ftpsrv's self.c (GPL-3.0, John Törnblom;
// derived from idlesauce/ps5-self-pager). SELF layout: a 0x20 header, then
// `num_entries` 0x20 segment entries, then the embedded ELF; a PT_SCE_VERSION
// segment's bytes sit at the SELF's `file_size`; the ex-info (after the
// program headers, 0x10-aligned) holds the expected SHA-256 of the ELF.
#pragma once

#include "alembic/io.hpp"
#include "alembic/result.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace alembic::self_extract {

// One encrypted segment the assembler needs decrypted. The callback returns
// exactly `file_size` bytes (the segment's p_filesz).
struct Segment {
    std::uint32_t index;     // program-header index (matches the SELF entry)
    std::uint64_t file_size; // p_filesz
    std::uint64_t vaddr;     // p_vaddr
    std::uint64_t align;     // p_align
};
// Decrypts one segment of the SELF at `self_path`. On the PS5 this is the
// kernel self-pager map; on a PC there is none (returns unsupported).
using Decryptor = std::function<Result<std::vector<std::uint8_t>>(const std::string& self_path, const Segment& segment)>;

// Whether `head` (>= 4 bytes) starts a PS4 or PS5 SELF.
bool is_self(Bytes head);

// The ELF embedded in `self` (its bytes read through the RandomAccess for the
// plaintext parts; `self_path` passed to `decrypt` for the encrypted
// segments). Verifies the assembled ELF's SHA-256 against the SELF ex-info.
Result<std::vector<std::uint8_t>> extract_elf(const RandomAccess& self, const std::string& self_path, const Decryptor& decrypt);

// `source` with every encrypted-SELF executable served as its plain ELF, for a
// conversion (decrypt_self). Only files with a real on-disk path
// (tree::Source::real_path) can be decrypted; images and packages have none.
struct Decrypted {
    std::unique_ptr<tree::Source> tree; // null when nothing was decrypted
    std::vector<std::string> paths;     // tree paths turned into plain ELF
    std::vector<std::pair<std::string, std::string>> skipped; // path, why
};
// `game_sign` fake-signs each decrypted ELF the game way
// (fself::make_prospero_fself: PS5 magic, an executable authority for the
// eboot) so the console will launch it; off serves the plain ELF.
Result<Decrypted> decrypt_executables(const tree::Source& source, const Decryptor& decrypt, bool game_sign = true,
                                      const ProgressFn& progress = {});

} // namespace alembic::self_extract
