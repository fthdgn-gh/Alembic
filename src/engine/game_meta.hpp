// SPDX-License-Identifier: GPL-3.0-or-later
// Title, title ID, version and icon of a game (a package, an image or a
// folder), read from its sce_sys/param.json and sce_sys/icon0.png. Results are
// cached in memory and in <state>/cache/meta, keyed by path, size and mtime.
// An entry's .json time stamp is its last use; prune() drops old entries.
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace alembic::engine {

struct GameMeta {
    std::string title;      // en-US when present, else the default language
    std::string title_id;
    std::string content_id;
    std::string version;    // contentVersion
    bool has_icon = false;
};

class GameMetaCache {
public:
    explicit GameMetaCache(std::string state_dir);

    Result<GameMeta> meta(const std::string& path);
    // icon0.png bytes.
    Result<std::vector<std::uint8_t>> icon(const std::string& path);

    struct Stats {
        std::uint64_t games = 0; // entries (one .json each)
        std::uint64_t bytes = 0; // every file in the cache folder
    };
    Stats stats() const;
    // Drops entries not used for `max_age_days`, then the least recently used
    // until the cache is at most `max_bytes`. Returns how many were dropped.
    std::uint64_t prune(std::uint64_t max_age_days = 30, std::uint64_t max_bytes = 100ull << 20);
    // Deletes every entry, on disk and in memory.
    Status clear();

private:
    Result<std::string> key_for(const std::string& path) const;
    Status load(const std::string& path, const std::string& key);

    std::string dir_;
};

} // namespace alembic::engine
