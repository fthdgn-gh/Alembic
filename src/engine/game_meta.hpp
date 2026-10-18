// SPDX-License-Identifier: GPL-3.0-or-later
// Title, title ID, version and icon of a game (a package, an image or a
// folder), read from its sce_sys/param.json and sce_sys/icon0.png. Results are
// cached in memory and in <state>/cache/meta, keyed by path, size and mtime.
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

private:
    Result<std::string> key_for(const std::string& path) const;
    Status load(const std::string& path, const std::string& key);

    std::string dir_;
};

} // namespace alembic::engine
