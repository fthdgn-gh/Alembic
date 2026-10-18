// SPDX-License-Identifier: GPL-3.0-or-later
// File manager operations for the engine: delete, rename, new folder, free
// space, and copy / move with progress (run as queue jobs).
#pragma once

#include "alembic/io.hpp"
#include "alembic/result.hpp"

#include <cstdint>
#include <string>

namespace alembic::engine {

struct SpaceInfo {
    std::uint64_t total = 0;
    std::uint64_t free = 0; // available to us
};
Result<SpaceInfo> space(const std::string& path);

// Deletes a file, or a folder and everything in it.
Status remove_tree(const std::string& path);
Status make_folder(const std::string& path);
// Renames within the same folder: `new_name` is a plain name.
Status rename_entry(const std::string& path, const std::string& new_name);

// Bytes in a file or folder (recursively).
Result<std::uint64_t> tree_size(const std::string& path);

// Copies a file or folder to `destination` (the new path, not its parent).
// Files go through "<destination>.alembic-partial" and are renamed at the
// end; on failure or cancel everything this call created is removed.
// Modification times are kept.
Status copy_tree(const std::string& source, const std::string& destination, bool overwrite, const ProgressFn& progress);
// rename() when both are on the same file system, else copy then delete.
Status move_tree(const std::string& source, const std::string& destination, bool overwrite, const ProgressFn& progress);

// True when `inner` is `outer` or inside it.
bool is_within(const std::string& inner, const std::string& outer);

} // namespace alembic::engine
