// SPDX-License-Identifier: GPL-3.0-or-later
// Third-party files Alembic needs but does not ship: each is downloaded on
// request from a fixed URL (an official release or a pinned commit) and kept
// only if its SHA-256 matches, under the engine's state folder.
#pragma once

#include "json_value.hpp"

#include "alembic/result.hpp"

#include <string>
#include <vector>

namespace alembic::engine {

struct DependencyFile {
    std::string url;
    std::string sha256;
    std::size_t size;
    std::string path; // relative to the state folder
};

struct Dependency {
    std::string id;
    std::string label;
    std::string purpose;
    std::string source;  // where it comes from, for the user
    std::string license;
    std::vector<DependencyFile> files;
    // Components that do the same job (kAmprRuntimeGroup): one of them is in
    // use at a time. "" for the others.
    std::string group;
    std::string short_name; // "ampr_emu 0.4.2.1", for the engine's facts
};

// The AMPR emulators (fakelib/libSceAmpr.sprx); each is one file.
inline constexpr const char* kAmprRuntimeGroup = "ampr_runtime";

const std::vector<Dependency>& dependencies();
const Dependency* find_dependency(const std::string& id);

// "installed" (every file present with the pinned hash), "missing",
// "partial" or "modified" (a file with another hash).
std::string dependency_state(const Dependency& dep, const std::string& state_dir);
JsonValue dependency_json(const Dependency& dep, const std::string& state_dir);

// Downloads every file, checks its hash, then writes them all. An AMPR
// emulator downloaded becomes the one in use.
Status install_dependency(const Dependency& dep, const std::string& state_dir);
Status remove_dependency(const Dependency& dep, const std::string& state_dir);

// The AMPR emulator in use: the one chosen last (runtime/in-use) when it is
// downloaded, else the first one downloaded, else nullptr.
const Dependency* ampr_runtime_in_use(const std::string& state_dir);
// Makes `dep` (an AMPR emulator) the one in use.
Status use_dependency(const Dependency& dep, const std::string& state_dir);
// The AMPR emulator whose pinned SHA-256 is `sha256`, else nullptr.
const Dependency* ampr_runtime_by_hash(const std::string& sha256);

} // namespace alembic::engine
