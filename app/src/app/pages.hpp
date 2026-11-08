// SPDX-License-Identifier: GPL-3.0-or-later
// The five sections (pages/*): each lays itself out in the area between the
// header and the button hints.
#pragma once

#include "app/app.hpp"

#include <string>
#include <vector>

namespace alembic::app {

// The grid view (tiles.cpp, components/Tile.tsx): columns of tiles at
// least 10.5 rem wide.
struct Grid {
    float x = 0, w = 0, h = 0, gap = 0;
    int cols = 1;
    Rect cell(int i, float top) const {
        return {x + static_cast<float>(i % cols) * (w + gap), top + static_cast<float>(i / cols) * (h + gap), w, h};
    }
    float height(int n) const { return n ? static_cast<float>((n + cols - 1) / cols) * (h + gap) - gap : 0.0f; }
};
Grid grid_of(float x, float w);
struct TileInfo {
    Entry entry;
    const Meta* meta = nullptr;
    std::string badge_state, badge;  // a state pill on the icon ("" for none)
    bool selecting = false, ticked = false, hidden = false, dim = false;
};
// One tile; true when ✕ was pressed on it.
bool tile(Ui& ui, App& app, Id id, const Rect& r, const TileInfo& info);
// A drive, server or other engine's drive as a tile: symbol, name, room
// (`used` 0..1 draws a meter; < 0 none). True when ✕ was pressed on it.
bool drive_tile(Ui& ui, Id id, const Rect& r, draw::Icon icon, const std::string& title, const std::string& room, float used);
// The list and grid buttons ending at `right`; returns their width.
float view_switch(Ui& ui, App& app, const std::string& which, float right, float y, float h);

class FilesPage {
public:
    void build(Ui& ui, App& app, const Rect& area);
    std::vector<Hint> hints(App& app);
    // □ in Files: the focused row's Options.
    void square(App& app);
    std::string focused_path;  // the focused row
private:
    void rail(Ui& ui, App& app, const Rect& area);
    void list(Ui& ui, App& app, const Rect& area);
    bool in_list_ = false;
};

class ImagesPage {
public:
    void build(Ui& ui, App& app, const Rect& area);
    void load(App& app, bool refresh);
    // The tab was opened: read the list, and focus its first game.
    void opened(App& app);
    std::vector<Hint> hints(App& app);
    // □: the focused row's Options, or while selecting the ticked ones' actions.
    void square(App& app);
    // The list's item at `path` (its ampr state), nullptr when none.
    const JsonValue* item(const std::string& path) const;
    // An update applies: a newer build of its kind, or none yet, inside the
    // engine's folders, and not queued from here already.
    bool updatable(const JsonValue& item) const;
    // Queues the update of these (one at once, an unknown build asked first;
    // several after one question). `after` runs once they are queued.
    void update_ampr(App& app, std::vector<JsonValue> list, std::function<void()> after = nullptr);
    void update_all(App& app);
    bool loaded = false;

private:
    void queue(App& app, std::vector<JsonValue> list, std::function<void(int queued)> done);
    std::string target_name(const JsonValue& item) const;
    bool is_pending(const std::string& path) const;
    JsonValue data_;                    // GET /api/images
    JsonValue peers_ = JsonValue::array();  // GET /api/peers/images: the paired engines' lists
    void rebuild_entries(App& app);     // app.image_entries: this engine's rows, then the others' 
    std::string focused_;               // the focused row's path
    std::vector<std::string> pending_;  // paths with an AMPR job queued from here
    float poll_at_ = 0.0f;
    std::size_t active_before_ = 0;     // jobs running last frame: one ending reads the folders again
    bool focus_first_ = true;           // the first game takes the focus once the list is there
};

class QueuePage {
public:
    void build(Ui& ui, App& app, const Rect& area);
};

class DepsPage {
public:
    void build(Ui& ui, App& app, const Rect& area);
    void load(App& app);
    bool loaded = false;

private:
    JsonValue deps_;
    JsonValue status_;     // /api/deps/status: {auto_update, checking}; null before engine 0.4.3
    std::string busy_;     // "<id>:install" while one runs
    float poll_at_ = 0.0f; // while the engine asks GitHub: when to look again
};

class SettingsPage {
public:
    void build(Ui& ui, App& app, const Rect& area);
    void opened(App& app);  // the tab was opened: ask for the update status

private:
    void load_update(App& app);
    JsonValue update_;
    bool update_loaded_ = false;
    float update_timer_ = 0.0f;
    std::string wait_for_;  // the version being started
    float wait_timer_ = 0.0f, wait_total_ = 0.0f;
    std::map<std::string, bool> off_;  // payload files left out
    bool cache_busy_ = false, launcher_busy_ = false;
    float peers_timer_ = 0.0f;
};

}  // namespace alembic::app
