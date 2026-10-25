// SPDX-License-Identifier: GPL-3.0-or-later
// Alembic's console app: the engine's web page, drawn natively. The same
// sections (Files, Queue, Components, Settings), dialogs and texts, over the
// same API; the engine does all the work.
#pragma once

#include "app/format.hpp"
#include "app/i18n.hpp"
#include "engine/client.hpp"
#include "ui/draw.hpp"
#include "ui/ui.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace alembic::app {

// What the platform gives the app.
struct Platform {
    // Uploads RGBA pixels as a texture; 0 on failure.
    std::function<std::uint32_t(int w, int h, const std::uint8_t* rgba)> make_texture;
    std::function<void(std::uint32_t)> free_texture;
    // Sends the bundled engine payload to the ELF loader; false (and why) when
    // there is none or the loader does not answer. Empty on a PC.
    std::function<bool(std::string* why)> start_engine;
    // Closes the app the clean way (PS5: the system closes the title).
    std::function<void()> quit;
    // Reads and writes the app's own small files (preferences).
    std::function<bool(const std::string& name, std::string* data)> read_file;
    std::function<bool(const std::string& name, const std::string& data)> write_file;
    std::uint32_t logo = 0;  // the app's icon as a texture
    bool console = false;    // running on a PS5
    std::string version;     // the app's version
};

struct Entry {
    std::string name, path, kind, peer;
    double size = -1;
    std::int64_t mtime = 0;
    bool game = false, ftp = false, shared = false;
    bool has_size() const { return size >= 0; }
};
Entry entry_from(const JsonValue& v);
bool is_convertible(const Entry& e);

struct Meta {
    std::string title, title_id, content_id, version;
    bool icon = false;
};

enum class Tab { files, queue, deps, settings };

enum class Glyph { cross, circle, square, triangle, l1r1, l2r2, dpad, options };
struct Hint {
    Glyph glyph;
    std::string label;
};

class App;

// A dialog over the page (Overlay.tsx): only the top one takes input.
class Overlay {
public:
    virtual ~Overlay() = default;
    virtual void build(Ui& ui, App& app) = 0;
    virtual std::vector<Hint> hints(App& app);
    // ○: closes by default.
    virtual void back(App& app);
    // □, the dialog's second action.
    virtual void square(App&) {}
    int key = 0;
    bool closing = false;
    int opened_frame = 0;  // takes input from the frame after it opened
};

// Buttons of a dialog sheet; see sheet() in dialogs.hpp.
class FilesPage;
class QueuePage;
class DepsPage;
class SettingsPage;

class App {
public:
    App(Engine& engine, Platform platform);
    ~App();

    // One frame: input, then everything drawn into ui.list.
    void frame(Ui& ui);

    // ---- what the engine says (state/engine.ts) ----
    Engine& engine;
    Platform platform;
    JsonValue info;          // /api/info (null until it answers)
    JsonValue space;         // /api/fs/space
    JsonValue locations;     // /api/locations
    JsonValue ftp;           // /api/ftp
    JsonValue peer_drives;   // /api/peer-drives
    JsonValue peers;         // /api/peers
    std::vector<JsonValue> jobs;  // this engine's, then other engines' that involve it
    std::string not_connected;    // why the engine cannot be reached
    bool answered = false;        // it answered once
    bool any_paired() const;
    bool is_ps5() const { return info["platform"].str() == "ps5"; }

    void load_info();
    void refresh_jobs(bool remote_now = false);
    // Settings > Restart the engine (after the question): the engine starts
    // a copy of itself; an engine that can't (none found, or before 0.3.0)
    // is stopped and the app's own engine started, when that is not older.
    void restart_engine();
    // Sends the app's own engine to the ELF loader (Settings, once the engine stopped).
    void start_own_engine();
    bool restarting() const { return restart_ != Restart::none; }
    void load_peers();

    // ---- requests ----
    using Done = std::function<void(const Reply&)>;
    void api(const std::string& method, const std::string& path, const JsonValue& body, Done done, int lane = 0,
             int timeout_ms = 8000);
    void get(const std::string& path, Done done, int lane = 0) { api("GET", path, JsonValue(), std::move(done), lane); }
    void post(const std::string& path, const JsonValue& body, Done done, int lane = 0) {
        api("POST", path, body, std::move(done), lane);
    }
    // Calls `done` only when the request worked; else a toast says why.
    Done or_toast(std::function<void(const Reply&)> done);

    // ---- paths (state/paths.ts) ----
    const JsonValue* root_of(const std::string& path) const;  // a drive of space or peer_drives
    const JsonValue* peer_drive_of(const std::string& path) const;
    const JsonValue* ftp_of(const std::string& path) const;
    std::string drive_name(const std::string& root) const;
    std::string shown_path(const std::string& path) const;
    std::string here_name() const;

    // ---- jobs (state/jobs.ts) ----
    static bool is_active(const JsonValue& j);
    static std::string job_key(const JsonValue& j);
    static std::string job_path(const JsonValue& j);
    double rate_of(const JsonValue& j) const;

    // ---- game info and icons (api/meta.ts) ----
    // The cached info of a path; asks for it when unknown (two at a time).
    // null while loading or when the path has none.
    const Meta* meta(const std::string& path, std::int64_t mtime, double size);
    // True once the engine said this path has no game info.
    bool meta_none(const std::string& path, std::int64_t mtime, double size) const;
    std::uint32_t icon(const std::string& path, std::int64_t mtime);
    void clear_meta();

    // ---- Files (pages/files/browse.ts) ----
    std::string cwd;
    std::vector<Entry> entries;
    bool listing_loaded = false;
    std::string listing_path;
    std::string focus_after;  // the row to focus when the listing arrives
    bool focus_list = false;  // put the focus in the list then
    void browse(const std::string& path, const std::string& focus_key = "");
    void refresh_listing();

    // ---- sections, dialogs, toasts ----
    Tab tab = Tab::files;
    void show_tab(Tab t);
    void open(std::unique_ptr<Overlay> overlay);
    void close(Overlay* overlay);
    Overlay* top() const { return overlays_.empty() ? nullptr : overlays_.back().get(); }
    bool overlay_alive(int key) const;
    enum class Toast { info, ok, bad };
    void toast(const std::string& text, Toast kind = Toast::info);

    // ---- the page's per-browser settings, kept by the app ----
    JsonValue prefs;
    void save_prefs();
    std::vector<std::string> recent() const;
    void remember_folder(const std::string& path);

    // ---- actions shared by pages and dialogs ----
    void open_actions(const Entry& e);
    void open_convert(const Entry& e);
    void open_dest(const Entry* e, const std::string& kind, const std::string& start,
                   std::function<void(const std::string& folder)> picked = nullptr);
    void toggle_hidden(const std::string& path, bool quiet = false);
    void set_place(const std::string& path, bool add);
    void job_actions(const JsonValue& j);
    void clear_history();
    void get_needs(const JsonValue& plan, const std::string& intro, std::function<void(bool ok)> done);

    float time = 0.0f;

private:
    struct ToastItem {
        std::string text;
        Toast kind;
        float left;
        float age;
    };
    struct MetaSlot {
        bool done = false;
        bool has = false;
        Meta meta;
    };
    struct IconSlot {
        bool done = false;
        std::uint32_t texture = 0;
    };
    void pump_meta();
    void pump_icons();
    void draw_header(Ui& ui);
    void draw_hints(Ui& ui, const std::vector<Hint>& hints);
    void draw_toasts(Ui& ui);
    void draw_connecting(Ui& ui);
    std::vector<Hint> page_hints();
    void handle_global(Ui& ui);
    void tick(float dt);
    void sample(const JsonValue& j);
    void watch_incoming();
    void notice_update();

    std::function<void(hui::audio::Cue)> feedback_hook_;  // sounds while a frame runs
    std::vector<std::unique_ptr<Overlay>> overlays_;
    int next_key_ = 1;
    int frame_no_ = 0;
    std::vector<ToastItem> toasts_;
    std::map<std::string, MetaSlot> meta_;
    std::vector<std::string> meta_queue_;
    int meta_active_ = 0;
    std::map<std::string, IconSlot> icons_;
    int icons_active_ = 0;
    std::vector<std::pair<std::string, std::string>> icon_queue_;  // key, path

    // jobs.ts bookkeeping
    struct Speed {
        double t = 0, done = 0, rate = 0;
        std::string stage;
    };
    std::map<std::string, Speed> speeds_;
    std::set<std::string> was_active_, known_, listed_;
    std::vector<JsonValue> remote_jobs_;
    double remote_at_ = -100;
    bool remote_busy_ = false;
    bool jobs_busy_ = false;
    bool info_busy_ = false;

    float jobs_timer_ = 0.0f, info_timer_ = 0.0f, incoming_timer_ = 0.0f, peers_timer_ = 0.0f;
    double clock_ = 0.0;  // seconds since start

    // starting the engine
    enum class Start { waiting, sent, failed, none };
    Start start_ = Start::waiting;
    float start_timer_ = 0.0f;
    std::string start_error_;
    // restarting the engine: until it went away and answers again
    enum class Restart { none, waiting, start_own };
    Restart restart_ = Restart::none;
    float restart_timer_ = 0.0f;
    bool restart_gone_ = false;
    std::string restart_from_;  // the engine's "started" before the restart
    float restart_poll_ = 0.0f;
    bool i18n_loaded_ = false;
    bool i18n_busy_ = false;
    bool first_browse_ = false;
    std::string update_told_;
    std::string mismatch_told_;  // the engine version the app-is-older notice was shown for
    int incoming_key_ = 0;
    std::string incoming_from_;

    std::unique_ptr<FilesPage> files_;
    std::unique_ptr<QueuePage> queue_;
    std::unique_ptr<DepsPage> deps_;
    std::unique_ptr<SettingsPage> settings_;

    friend class FilesPage;
    friend class QueuePage;
    friend class DepsPage;
    friend class SettingsPage;
};

}  // namespace alembic::app
