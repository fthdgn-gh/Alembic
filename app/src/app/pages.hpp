// SPDX-License-Identifier: GPL-3.0-or-later
// The four sections (pages/*): each lays itself out in the area between the
// header and the button hints.
#pragma once

#include "app/app.hpp"

#include <string>
#include <vector>

namespace alembic::app {

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
    std::string busy_;  // "<id>:install" while one runs
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
