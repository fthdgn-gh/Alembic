// SPDX-License-Identifier: GPL-3.0-or-later
// Turns a JobSpec into a convert::run call, like the CLI's `convert`.
#pragma once

#include "jobs.hpp"

#include <functional>
#include <string>
#include <vector>

namespace alembic::engine {

struct EngineConfig {
    std::string state_dir;
    std::vector<std::string> roots; // inputs, outputs and browsing stay below these
    std::string runtime_path;       // default PackedStdio libSceAmpr.sprx
    unsigned threads = 0;           // compression threads, 0 = all cores
    // True when a queued job other than the running one reads `path`; the
    // source is then kept even with delete_source.
    std::function<bool(const std::string& path)> needed_by_other_jobs;

    // The roots that can be used right now (mounted drives only).
    std::vector<std::string> available_roots() const;
};

// The output a job writes: spec.output, or next to the input with the
// target's extension ("-lz4" added when packing).
Result<std::string> resolve_output(const JobSpec& spec);
// The target format name for a job ("folder", "exfat", "ffpfsc", "fpkg", ...).
Result<std::string> resolve_target(const JobSpec& spec);

// ShadowMount+ mounts an image at <base>/<name without extension>_<8 hex>,
// and a FreeBSD mount point is at most 87 characters (MNAMELEN 88): the name
// may have 58 characters for .ffpfsc (/mnt/shadowmnt/pfsc/), 63 for .exfat
// and .ffpkg (/mnt/shadowmnt/). 0 when there is no limit (folders).
std::size_t mount_name_limit(const std::string& output);
// A warning when ShadowMount+ could not mount `output`, else "".
std::string mount_name_warning(const std::string& output);
// `name` (without extension) cut to `limit`: the title ID, the -Axxxx-Vxxxx
// version and a trailing -lz4 / -unpacked tag when the name has them, else
// the first `limit` characters.
std::string shorten_name(const std::string& name, std::size_t limit);

// Absolute, no "." / ".." parts, and below one of `roots`.
bool path_allowed(const std::string& path, const std::vector<std::string>& roots);

Result<JsonValue> run_conversion(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);
// The ids of the components (deps.hpp) a job needs that are not installed:
// the ampr_emu runtime for LZ4 packing when the game ships none.
std::vector<std::string> missing_dependencies(const JobSpec& spec, const EngineConfig& config);

// Copy and move jobs.
Result<JsonValue> run_transfer(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);
// Either, by spec.kind.
Result<JsonValue> run_job(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);

// SHA-256 of ampr_emu 0.4.2.1's PackedStdio libSceAmpr.sprx (the official
// release asset), the runtime LZ4 packs are tested with.
inline constexpr const char* kPinnedRuntimeSha256 =
    "69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a";

} // namespace alembic::engine
