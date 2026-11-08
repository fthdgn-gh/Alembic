// SPDX-License-Identifier: GPL-3.0-or-later
// The five sections (pages/*): each lays itself out in the area between the
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

class ImagesPage {
public:
    void build(Ui& ui, App& app, const Rect& area);
    void load(App& app, bool refresh);
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
    std::string focused_;               // the focused row's path
    std::vector<std::string> pending_;  // paths with an AMPR job queued from here
    float poll_at_ = 0.0f;
    std::size_t active_before_ = 0;     // jobs running last frame: one ending reads the folders again
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
