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
};

const std::vector<Dependency>& dependencies();
const Dependency* find_dependency(const std::string& id);

// "installed" (every file present with the pinned hash), "missing",
// "partial" or "modified" (a file with another hash).
std::string dependency_state(const Dependency& dep, const std::string& state_dir);
JsonValue dependency_json(const Dependency& dep, const std::string& state_dir);

// Downloads every file, checks its hash, then writes them all.
Status install_dependency(const Dependency& dep, const std::string& state_dir);
Status remove_dependency(const Dependency& dep, const std::string& state_dir);

} // namespace alembic::engine
