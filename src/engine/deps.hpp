// SPDX-License-Identifier: GPL-3.0-or-later
// Third-party files Alembic needs but does not ship, downloaded on request
// under the engine's state folder.
//
// Two kinds:
// - Followed releases (the AMPR emulators): the latest release of a GitHub
//   project. The engine asks GitHub at start (and on request) whether a newer
//   one is out, and with "update automatically" on (the default) downloads it.
//   A download is checked against the SHA-256 GitHub gives for the asset and,
//   when the release has one, its SHA256SUMS. The build Alembic was last
//   tested with is kept as a fallback for when GitHub's API can't be reached.
// - Fixed files (the key files): one URL at a pinned commit each, kept only
//   with the pinned SHA-256. They are constants the writers check, so they
//   don't follow anything.
//
// <state>/components.json keeps what was downloaded (version, SHA-256), the
// builds seen before (so a game's emulator still gets its name after an
// update) and the "update automatically" switch.
#pragma once

#include "json_value.hpp"

#include "alembic/result.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace alembic::engine {

struct DependencyFile {
    std::string url;
    std::string sha256;
    std::size_t size;
    std::string path; // relative to the state folder
};

// A GitHub project whose latest release a dependency follows.
struct ReleaseFeed {
    std::string repo;  // "owner/name"; "" for fixed files
    std::string asset; // the release asset to download
};

struct Dependency {
    std::string id;
    std::string label;
    std::string purpose;
    std::string source;  // where it comes from, for the user
    std::string license;
    // Fixed files: what is downloaded. A followed release: one file, its
    // fallback build (url, SHA-256, size) and where it is kept (path).
    std::vector<DependencyFile> files;
    // Components that do the same job (kAmprRuntimeGroup): one of them is in
    // use at a time. "" for the others.
    std::string group;
    std::string name;            // "ampr_emu": with a version, how a build is named
    ReleaseFeed feed;            // repo "" for fixed files
    std::string fallback_version; // files[0]'s version, for a followed release
};

// The AMPR emulators (fakelib/libSceAmpr.sprx); each is one file.
inline constexpr const char* kAmprRuntimeGroup = "ampr_runtime";

const std::vector<Dependency>& dependencies();
const Dependency* find_dependency(const std::string& id);

// "installed" (every file present with the expected hash: the pinned one, or
// for a followed release the one downloaded), "missing", "partial" or
// "modified" (a file with another hash).
std::string dependency_state(const Dependency& dep, const std::string& state_dir);
// Also, for a followed release: "version" (downloaded), "latest" (from the
// last check), "update" (a newer one is out), "follows" (owner/name).
JsonValue dependency_json(const Dependency& dep, const std::string& state_dir);
// {auto_update, checking}, for GET /api/deps/status (GET /api/deps stays a
// plain list for older apps).
JsonValue dependencies_status_json(const std::string& state_dir);

// Downloads every file, checks its hash, then writes them all. A followed
// release gets its latest build (the fallback one when GitHub's API can't be
// reached). An AMPR emulator downloaded becomes the one in use when
// `make_in_use` (an update keeps the choice as it is).
Status install_dependency(const Dependency& dep, const std::string& state_dir, bool make_in_use = true);
Status remove_dependency(const Dependency& dep, const std::string& state_dir);

// Asks GitHub for each followed release's latest build (blocking; a few
// small requests). With `apply` and "update automatically" on, downloads
// those that are downloaded here and have a newer build. `log` gets a line
// per finding.
void check_dependency_updates(const std::string& state_dir, bool apply, const std::function<void(const std::string&)>& log);
bool dependency_check_running();
bool auto_update_dependencies(const std::string& state_dir);
Status set_auto_update_dependencies(const std::string& state_dir, bool on);

// The AMPR emulator in use: the one chosen last (runtime/in-use) when it is
// downloaded, else the first one downloaded, else nullptr.
const Dependency* ampr_runtime_in_use(const std::string& state_dir);
// Makes `dep` (an AMPR emulator) the one in use.
Status use_dependency(const Dependency& dep, const std::string& state_dir);

// An AMPR emulator build Alembic knows by its SHA-256: a fallback build or
// one downloaded here before.
struct KnownBuild {
    const Dependency* dep = nullptr;
    std::string version; // "0.4.2.1"
    std::string name() const { return dep->name + " " + version; }
};
std::optional<KnownBuild> ampr_runtime_by_hash(const std::string& sha256, const std::string& state_dir);

} // namespace alembic::engine
