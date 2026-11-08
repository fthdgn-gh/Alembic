// SPDX-License-Identifier: GPL-3.0-or-later
// The Images tab (GET /api/images): the images and game folders ShadowMount+
// runs, those whose game uses the console's APR, which AMPR emulator each
// runs with (in the game or its backport), and whether a newer build of that
// emulator is out. Updating one is an "ampr_emu" job (runner.hpp): "replace"
// with the game's own emulator kind, or adding the one in use.
//
// The list comes from ShadowMount+'s API (/api/v1/games) when it answers,
// else from its scan folders (config.ini's scanpath lines, else its
// defaults), one level deep. Reading every game's eboot.bin takes a while,
// so the list is built on a thread of its own and served as it stands.
#pragma once

#include "json_value.hpp"
#include "runner.hpp"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace alembic::engine::images {

// An image or game folder found by ShadowMount+ or in a scan folder.
struct Found {
    std::string path;
    std::string backport; // ShadowMount+'s resolved backport folder, "" when none or unknown
};

// ShadowMount+'s scan folders: config.ini's scanpath lines, else its defaults
// (on a PC, <state>/homebrew stands in).
std::vector<std::string> scan_dirs(const EngineConfig& config);
// What those folders hold, one level deep: folders with eboot.bin and images
// by extension (.exfat, .ffpfsc, .ffpkg, .ffpfs); not "backports" nor names
// starting with '.' (ShadowMount+ skips both).
std::vector<Found> scan(const std::vector<std::string>& dirs);

// One entry as GET /api/images lists it: {path, title_id, kind (folder or
// image), where (game, backport or ""), emulator {id, name, version} or
// null, status, target, zstd_packs, allowed}. status: "current", "update"
// (a newer build of its kind is downloaded here), "missing" (uses the APR,
// has none), "unknown" (a build Alembic does not know), "not_downloaded"
// (its kind is not downloaded here to compare with). target: the emulator
// (component id) an Update or Add puts there. nullopt: the game does not use
// the APR, or cannot be read (`unreadable` set).
std::optional<JsonValue> evaluate(const Found& found, const EngineConfig& config, bool* unreadable = nullptr);

class Index {
public:
    explicit Index(const EngineConfig& config) : config_(config) {}
    ~Index();
    // {images, source (shadowmount or scan), refreshing, updated, others
    // (images whose game does not use the APR), unreadable,
    // smp_update_emulators (bool, or null when ShadowMount+ does not say),
    // emulators [{id, label, name, downloaded, in_use}]}. Starts a refresh
    // when the list is older than `max_age_s` (0: always) and none runs.
    JsonValue snapshot(std::int64_t max_age_s);

private:
    void refresh();
    const EngineConfig& config_;
    std::mutex mutex_;
    std::thread worker_;
    bool running_ = false;
    std::int64_t updated_ = 0;
    JsonValue images_ = JsonValue::array();
    std::string source_;
    std::int64_t others_ = 0, unreadable_ = 0;
    JsonValue smp_update_emulators_;
};

} // namespace alembic::engine::images
