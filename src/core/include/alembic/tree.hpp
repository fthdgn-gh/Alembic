// SPDX-License-Identifier: GPL-3.0-or-later
// A file tree that every container can expose: a folder, an exFAT image, the
// payload of an FFPFSC, and later an FPKG. Writers (exFAT, FFPFSC, folder)
// consume a tree, so any input converts to any output without extracting to
// disk first.
#pragma once

#include "alembic/io.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::tree {

struct Node {
    std::string name; // UTF-8; empty for the root
    bool is_directory = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0; // Unix seconds
    std::uint64_t id = 0;   // meaning is up to the Source
    std::vector<Node> children;
};

class Source {
public:
    virtual ~Source() = default;
    // The whole tree, loaded when the source is opened.
    virtual const Node& root() const = 0;
    virtual Result<std::shared_ptr<const RandomAccess>> open(const Node& file) const = 0;
    // A human name for the tree (folder or image file name), used when a
    // payload name has to be made up.
    virtual const std::string& label() const = 0;
};

// A folder on disk. Skips symlinks and OS junk files (.DS_Store, ._*,
// Thumbs.db, ...); fails on FIFOs, sockets and devices.
Result<std::unique_ptr<Source>> open_folder(const std::string& path);

// Finds a node by '/'-separated path (exact match). Returns null if missing.
const Node* find(const Node& root, std::string_view path);
// Finds a direct child by name, ignoring ASCII case.
const Node* find_child_icase(const Node& directory, std::string_view name);

struct Totals {
    std::uint64_t files = 0;
    std::uint64_t directories = 0; // not counting `node` itself
    std::uint64_t bytes = 0;
};
Totals totals(const Node& node);

// Writes `directory` (usually the root) into `output_folder`, creating it if
// needed. Existing files are an error unless `overwrite`. Sets file times.
Status extract(const Source& source, const Node& directory, const std::string& output_folder, bool overwrite = false,
               const ProgressFn& progress = {});

// Junk files that operating systems drop into folders (.DS_Store, ._x, ...).
bool is_os_junk(std::string_view name);

} // namespace alembic::tree
