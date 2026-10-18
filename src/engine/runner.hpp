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
};

// The output a job writes: spec.output, or next to the input with the
// target's extension ("-lz4" added when packing).
Result<std::string> resolve_output(const JobSpec& spec);
// The target format name for a job ("folder", "exfat", "ffpfsc").
Result<std::string> resolve_target(const JobSpec& spec);

// Absolute, no "." / ".." parts, and below one of `roots`.
bool path_allowed(const std::string& path, const std::vector<std::string>& roots);

Result<JsonValue> run_conversion(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);
// Copy and move jobs.
Result<JsonValue> run_transfer(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);
// Either, by spec.kind.
Result<JsonValue> run_job(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);

// SHA-256 of ampr_emu 0.4.2.1's PackedStdio libSceAmpr.sprx (the official
// release asset), the runtime LZ4 packs are tested with.
inline constexpr const char* kPinnedRuntimeSha256 =
    "69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a";

} // namespace alembic::engine
