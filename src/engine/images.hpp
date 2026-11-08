// SPDX-License-Identifier: GPL-3.0-or-later
// The Images tab (GET /api/images): every image, game folder and package in
// the folders ShadowMount+ scans, hidden ones (".name") included, listed as
// Files lists them, with the AMPR emulator each game runs with (in the game
// or its backport) and whether a newer build of that emulator is out.
// Updating one is an "ampr_emu" job (runner.hpp): "replace" with the game's
// own emulator kind, or adding the one in use.
//
// The folders are ShadowMount+'s scan paths (its API's settings, else
// config.ini's scanpath lines, else its defaults), searched two levels deep;
// ShadowMount+'s game list, when its API answers, names each game's
// backport. Reading every game's eboot.bin takes a while, so the list is
// built on a thread of its own and served as it stands.
#pragma once

#include "json_value.hpp"
#include "runner.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace alembic::engine::images {

// An image, game folder or package found in a scan folder.
struct Found {
    std::string path;
    std::string kind;     // folder (a game folder), exfat, ffpfsc, ffpkg, ffpfs or fpkg
    std::string folder;   // the folder it is in (the list groups by it)
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    std::string backport; // ShadowMount+'s resolved backport folder, "" when none or unknown
};

// ShadowMount+'s scan folders: `custom` (its API's scan_paths) when given,
// else config.ini's scanpath lines, else its defaults (on a PC,
// <state>/homebrew stands in).
std::vector<std::string> scan_dirs(const EngineConfig& config, const std::vector<std::string>& custom = {});
// What those folders hold, two levels deep: game folders (eboot.bin or
// sce_sys/param.json), images by extension (.exfat, .ffpfsc, .ffpkg, .ffpfs)
// and packages (.pkg). Names starting with '.' (hidden from ShadowMount+)
// count; "backports" folders do not.
std::vector<Found> scan(const std::vector<std::string>& dirs);

// Where apps download packages (platform::downloads_folder: /data/downloads
// on the PS5, the system's Downloads folder on a PC),
// searched through every folder below it (not hidden ones) for .pkg files
// only: the Images tab lists those too.
std::string downloads_dir(const EngineConfig& config);
std::vector<Found> scan_packages(const std::string& dir);

// The AMPR emulator of one game: {status, emulator {id, name, version} or
// null, where (game, backport or ""), target, zstd_packs}. status:
// "current", "update" (a newer build of its kind is downloaded here),
// "missing" (uses the APR, has none), "unknown" (a build Alembic does not
// know), "not_downloaded" (its kind is not downloaded here to compare with),
// "none" (the game does not use the APR), "unreadable". target: the emulator
// (component id) an Update or Add puts there. Null for packages: installed,
// they use the console's own APR.
JsonValue ampr_of(const Found& found, const EngineConfig& config);

// One entry as GET /api/images lists it: Files' fields (name, path, kind,
// game, size, mtime) plus folder, hidden, allowed and ampr (ampr_of).
JsonValue entry_json(const Found& found, const EngineConfig& config);

// Undoes what adding the AMPR emulator to Alembic's own app did (0.6.0: the
// app then no longer started): a PPSA99594 backport holding only
// fakelib/libSceAmpr.sprx and ampr_emu.index (in config.backports_root or a
// scan folder's backports, or one level below one), and in the app's own
// folder those two files (the app ships neither). Anything else is left
// alone. Returns what was removed, for the log.
std::vector<std::string> repair_alembic_app(const EngineConfig& config, const std::vector<std::string>& dirs);

class Index {
public:
    explicit Index(const EngineConfig& config) : config_(config) {}
    ~Index();
    // {images, dirs (the folders searched), smp (ShadowMount+'s API answered),
    // ampr (this engine reads games for their AMPR emulator: a PS5 one),
    // refreshing, updated, smp_update_emulators (bool, or null when
    // ShadowMount+ does not say), emulators [{id, label, name, version,
    // latest, downloaded, in_use}]}. Starts a refresh when the list is older
    // than `max_age_s` (0: always) and none runs.
    JsonValue snapshot(std::int64_t max_age_s);

private:
    void refresh();
    const EngineConfig& config_;
    std::mutex mutex_;
    std::thread worker_;
    bool running_ = false;
    std::int64_t updated_ = 0;
    std::vector<JsonValue> images_;  // the rows, filled in while a refresh reads the games
    std::vector<char> dropped_;      // rows found to be Alembic's own app (not listed)
    JsonValue dirs_ = JsonValue::array();
    bool smp_ = false;
    JsonValue smp_update_emulators_;
};

} // namespace alembic::engine::images
