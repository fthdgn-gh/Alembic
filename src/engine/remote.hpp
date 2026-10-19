// SPDX-License-Identifier: GPL-3.0-or-later
// Files a browser on another computer shares with the engine, so a package
// on a PC converts on the console without being copied to it first.
//
// The page registers a file it picked ({name, size, last_modified}) and gets
// a path "/remote/<id>/<name>" to use as a job input. While the engine reads
// it, the page keeps asking for work (want: a long poll) and posts the pieces
// it is asked for (data). The id comes from the file's name, size and time,
// so picking the same file again (after a reload or an engine restart) gives
// the same path and a stopped job can continue.
#pragma once

#include "json_value.hpp"

#include "alembic/io.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace alembic::engine {

class RemoteFiles {
public:
    static constexpr std::uint64_t kPiece = 1 << 20; // the HTTP body limit
    static constexpr const char* kPrefix = "/remote/";

    // A new or known file; returns its path.
    Result<std::string> add(const std::string& name, std::uint64_t size, std::int64_t last_modified);
    // The file behind a "/remote/..." path, or null.
    std::shared_ptr<const RandomAccess> open(const std::string& path) const;
    // The registration of a path (for the engine's checkpoint fingerprints).
    struct Info {
        std::string name;
        std::uint64_t size = 0;
        std::int64_t last_modified = 0;
    };
    std::optional<Info> info(const std::string& path) const;
    static bool is_remote(const std::string& path);

    // For the page: pieces to send (waits up to `wait_ms` for one), and a piece.
    JsonValue want(const std::string& id, int wait_ms);
    Status data(const std::string& id, std::uint64_t offset, const std::string& bytes);
    // The ids being read (for the page: which files it must keep sharing).
    JsonValue list() const;

    struct File;

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<File>> files_;
};

} // namespace alembic::engine
