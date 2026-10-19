// SPDX-License-Identifier: GPL-3.0-or-later
// drakmor/ampr_emu's libSceAmpr.sprx stands in for the console's APR (the
// asynchronous file reader games use) when a game runs from a mounted image or
// a folder instead of an installed package. Without it such a game fails at
// start (APR_FATAL_ERROR_ASYNC, or an abort in eboot.bin). ShadowMount+ loads
// it from the game's fakelib/ folder but never adds it there itself: its
// emulators folder only replaces a copy the game already has.
//
// This module tells whether a game needs the emulator and adds it to a tree.
#pragma once

#include "alembic/io.hpp"
#include "alembic/tree.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace alembic::ampr_emu {

inline constexpr const char* kFileName = "libSceAmpr.sprx";

struct Check {
    bool imports_ampr = false;   // eboot.bin names libSceAmpr among its libraries
    bool ships_emulator = false; // the fakelib folder below holds libSceAmpr.sprx
    // The folder ShadowMount+ mounts as the game's fakelib: "fakelib2" when
    // the game has one (it is then used alone), else "fakelib", in the
    // game's own spelling; "fakelib" when it has neither.
    std::string fakelib = "fakelib";
    bool has_fakelib = false; // that folder exists
    bool needed() const { return imports_ampr && !ships_emulator; }
};

// Looks at eboot.bin (read once, front to back) and the fakelib folders.
Result<Check> check(const tree::Source& source, const ProgressFn& progress = {});

// `source` with `emulator` added as <check.fakelib>/libSceAmpr.sprx
// (replacing a copy already there), plus ampr_emu.index when the game has
// none (the emulator cannot save one into a read-only image; an LZ4-packed
// game keeps its own). Files of `source` are served unchanged.
Result<std::unique_ptr<tree::Source>> add(const tree::Source& source, const Check& check,
                                          std::shared_ptr<const RandomAccess> emulator, std::int64_t mtime);

// Adding the emulator to a game that already exists, without rewriting it.
struct AttachResult {
    Check check;               // of the game itself
    std::string folder;        // where files went (the game folder, or the backport folder)
    std::vector<std::string> written; // paths below `folder`
    bool already_there = false; // nothing to do: the emulator was already in place
    std::vector<std::string> notes;
};

// A game folder (a dump ShadowMount+ runs as a folder): writes
// <fakelib>/libSceAmpr.sprx into it, and ampr_emu.index when it has none.
Result<AttachResult> attach_to_folder(const std::string& folder, const RandomAccess& emulator,
                                      const ProgressFn& progress = {});

// An image (or any tree that cannot be written to): writes a ShadowMount+
// backport folder, <backports_root>/<TITLE_ID>/, which ShadowMount+ lays over
// the mounted game. A backport's fakelib replaces the game's whole fakelib,
// so the game's own fakelib files are copied next to the emulator (except
// libkernel.sprx, which ShadowMount+ drops anyway). An existing backport
// folder for the title only gets the emulator added. ampr_emu.index is
// written there too when the game has none.
Result<AttachResult> attach_backport(const tree::Source& game, const std::string& backports_root,
                                     const RandomAccess& emulator, const ProgressFn& progress = {});

} // namespace alembic::ampr_emu
