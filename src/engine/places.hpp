// SPDX-License-Identifier: GPL-3.0-or-later
// Folders the person added to Places (Files > a folder > Add to Places),
// kept in <state>/places.json. Each engine keeps its own and shares them
// with paired engines (with its drives, in /api/peer/fs/roots), so they can
// be picked as a destination on every engine.
#pragma once

#include "alembic/result.hpp"

#include <mutex>
#include <string>
#include <vector>

namespace alembic::engine {

struct Place {
    std::string path;
    std::string label; // the folder's name unless named otherwise
};

class Places {
public:
    explicit Places(std::string state_dir);
    std::vector<Place> list() const;
    bool has(const std::string& path) const;
    // Adds (or renames) one; the caller checks the path is allowed.
    Status add(const std::string& path, const std::string& label);
    Status remove(const std::string& path);

private:
    Status save_locked() const;
    std::string file_;
    mutable std::mutex mutex_;
    std::vector<Place> places_;
};

} // namespace alembic::engine
