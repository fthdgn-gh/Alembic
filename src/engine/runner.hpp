// SPDX-License-Identifier: GPL-3.0-or-later
// Turns a JobSpec into a convert::run call, like the CLI's `convert`.
#pragma once

#include "jobs.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace alembic::engine {

struct EngineConfig {
    std::string state_dir;
    std::vector<std::string> roots; // inputs, outputs and browsing stay below these
    std::string runtime_path;       // ampr_emu's PackedStdio libSceAmpr.sprx (LZ4 packs and the AMPR emulator)
    // Where "ampr_emu" jobs put ShadowMount+ backport folders for images:
    // /data/homebrew/backports on the console (ShadowMount+'s fallback,
    // checked whatever its scan paths are).
    std::string backports_root;
    // ShadowMount+'s API: from its config.ini (the console), or "HOST:PORT"
    // (tests); neither set = no ShadowMount+ follow-up.
    std::string shadowmount_config;
    std::string shadowmount_address;
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
// Asks ShadowMount+ for a rescan (retry counters reset); false with the
// reason when its API is not there.
bool shadowmount_rescan(const EngineConfig& config, std::string* why = nullptr);

// Checkpoints of interrupted conversions, in <state>/resume/<hash of the
// output>.json, with the partial output at "<output>.alembic-partial". A
// conversion continues from one only if the job's options and the input's
// size and modification time are unchanged.
struct CheckpointInfo {
    std::uint64_t done = 0;  // progress when it was saved
    std::uint64_t total = 0;
};
inline constexpr const char* kPartialSuffix = ".alembic-partial";
// Whether this job keeps its progress when stopped (every output but FPKG; no LZ4 packing).
bool keeps_progress(const JobSpec& spec);
std::optional<CheckpointInfo> checkpoint_info(const JobSpec& spec, const EngineConfig& config);
// Deletes the checkpoint and the partial output.
void discard_checkpoint(const JobSpec& spec, const EngineConfig& config);
// The ids of the components (deps.hpp) a job needs that are not installed:
// the ampr_emu runtime for LZ4 packing when the game ships none, for a
// conversion that adds the AMPR emulator, and for an "ampr_emu" job.
std::vector<std::string> missing_dependencies(const JobSpec& spec, const EngineConfig& config);
// Whether a game needs ampr_emu (ampr_emu::check, kept per path, size and
// mtime); nullopt when it cannot be read.
struct AmprState {
    bool imports = false;        // eboot.bin uses the console's APR
    bool ships = false;          // its selected fakelib holds libSceAmpr.sprx
    bool ships_in_fakelib = false; // fakelib/libSceAmpr.sprx itself (what LZ4 packing uses)
    bool needed() const { return imports && !ships; }
};
std::optional<AmprState> ampr_state(const std::string& path, const std::string& passcode);
// The folder an "ampr_emu" job writes the backport into for this image:
// <scan path>/backports when a backport for the title is already there (it
// would win over the fallback), else config.backports_root.
std::string backports_root_for(const std::string& image, const std::string& title_id, const EngineConfig& config);

// Copy and move jobs.
Result<JsonValue> run_transfer(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);
// Either, by spec.kind.
Result<JsonValue> run_job(const JobSpec& spec, const EngineConfig& config, const ProgressFn& progress);

// SHA-256 of ampr_emu 0.4.2.1's PackedStdio libSceAmpr.sprx (the official
// release asset), the runtime LZ4 packs are tested with.
inline constexpr const char* kPinnedRuntimeSha256 =
    "69e6c4d5e4f5fb83c9e01815db5861c4c75734acbf4595cafa50d4c218116d1a";

} // namespace alembic::engine
