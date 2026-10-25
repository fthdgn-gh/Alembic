// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/app.hpp"

#include "app/dialogs.hpp"
#include "app/format.hpp"
#include "app/image.hpp"
#include "app/pages.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

namespace {
constexpr float kHeaderH = 1.1f * R + 2.45f * R + 0.9f * R;
constexpr float kHintsH = 0.7f * R + 1.45f * R + 0.5f * R + 1.0f * R;
}  // namespace

Entry entry_from(const JsonValue& v) {
    Entry e;
    e.name = v["name"].str();
    e.path = v["path"].str();
    e.kind = v["kind"].str("file");
    e.peer = v["peer"].str();
    if (v.has("size")) e.size = v["size"].number(-1);
    e.mtime = v["mtime"].integer();
    e.game = v["game"].boolean();
    e.ftp = v["ftp"].boolean();
    e.shared = v["shared"].boolean();
    return e;
}

std::vector<Hint> Overlay::hints(App&) { return {{Glyph::cross, t("Choose")}, {Glyph::circle, t("Back")}}; }
void Overlay::back(App& app) { app.close(this); }

App::App(Engine& e, Platform p) : engine(e), platform(std::move(p)) {
    // Another engine's paths inside messages ("... exists: /peer/<id>/data/x").
    set_path_namer([this](std::string_view text) {
        std::string s(text), out;
        std::size_t at = 0;
        for (std::size_t p = s.find("/peer/"); p != std::string::npos; p = s.find("/peer/", at)) {
            std::size_t id_end = p + 6;
            while (id_end < s.size() && std::isxdigit(static_cast<unsigned char>(s[id_end]))) ++id_end;
            if (id_end - (p + 6) != 32) {
                out += s.substr(at, id_end - at);
                at = id_end;
                continue;
            }
            std::size_t end = id_end;
            while (end < s.size() && std::string_view(" \t\n'\"),;").find(s[end]) == std::string_view::npos) ++end;
            out += s.substr(at, p - at) + shown_path(s.substr(p, end - p));
            at = end;
        }
        return out + s.substr(at);
    });
    files_ = std::make_unique<FilesPage>();
    queue_ = std::make_unique<QueuePage>();
    deps_ = std::make_unique<DepsPage>();
    settings_ = std::make_unique<SettingsPage>();
    prefs = JsonValue::object();
    std::string saved;
    if (platform.read_file && platform.read_file("prefs.json", &saved)) {
        auto parsed = engine::parse_json(saved);
        if (parsed && parsed->is_object()) prefs = std::move(parsed.value());
    }
    cwd = "/";
    refresh_jobs();
}

App::~App() = default;

// ---------------------------------------------------------------- requests

void App::api(const std::string& method, const std::string& path, const JsonValue& body, Done done, int lane,
              int timeout_ms) {
    engine.call(method, path, body.is_null() ? std::string() : body.dump(), std::move(done), lane, timeout_ms);
}

App::Done App::or_toast(std::function<void(const Reply&)> done) {
    return [this, done = std::move(done)](const Reply& r) {
        if (!r.ok()) {
            toast(t_msg(r.error()), Toast::bad);
            return;
        }
        if (done) done(r);
    };
}

// ---------------------------------------------------------------- state

bool App::any_paired() const {
    for (const auto& e : peers["engines"].items())
        if (e["paired"].boolean()) return true;
    return false;
}

void App::load_info() {
    if (info_busy_) return;
    info_busy_ = true;
    // One after another, as the engine serves one request at a time on the console.
    get("/api/info", [this](const Reply& r) {
        if (!r.ok() || !r.json.is_object()) {
            info_busy_ = false;
            return;
        }
        JsonValue i = r.json;
        if (!i18n_loaded_ && !i18n_busy_) {
            i18n_busy_ = true;
            engine.fetch("/i18n.js", nullptr, [this](int status, std::string body) {
                i18n_busy_ = false;
                if (status == 200 && load_i18n(body)) i18n_loaded_ = true;
            });
        }
        // The app speaks the language the engine keeps for its notifications
        // (the page's, or the console's).
        set_language(i["lang"].str("en"));
        get("/api/fs/space", [this, i](const Reply& sp) {
            get("/api/locations", [this, i, sp](const Reply& loc) {
                get("/api/ftp", [this, i, sp, loc](const Reply& f) {
                    get("/api/peer-drives", [this, i, sp, loc, f](const Reply& pd) {
                        get("/api/peers", [this, i, sp, loc, f, pd](const Reply& p) {
                            info = i;
                            if (sp.ok()) space = sp.json;
                            if (loc.ok()) locations = loc.json;
                            if (f.ok()) ftp = f.json;
                            if (pd.ok()) {
                                // The drive list shows paired engines' drives: ask for it
                                // again when they change (one went offline, or came back).
                                const bool changed = peer_drives.dump() != pd.json.dump();
                                peer_drives = pd.json;
                                if (changed && cwd == "/" && listing_loaded) browse("/");
                            }
                            if (p.ok()) peers = p.json;
                            not_connected.clear();
                            info_busy_ = false;
                            notice_update();
                            if (!first_browse_) {
                                first_browse_ = true;
                                browse("/");
                            }
                        });
                    });
                });
            });
        });
    });
}

void App::load_peers() {
    get("/api/peers", [this](const Reply& r) {
        if (r.ok()) peers = r.json;
    });
}

bool App::is_active(const JsonValue& j) {
    const std::string s = j["state"].str();
    return s == "running" || s == "queued" || s == "paused";
}

std::string App::job_key(const JsonValue& j) {
    const std::string id = std::to_string(j["id"].integer());
    return j["engine"].is_object() ? j["engine"]["id"].str() + ":" + id : id;
}

std::string App::job_path(const JsonValue& j) {
    const std::string id = std::to_string(j["id"].integer());
    if (j["engine"].is_object()) return "/api/peers/" + url_encode(j["engine"]["id"].str()) + "/jobs/" + id;
    return "/api/jobs/" + id;
}

void App::sample(const JsonValue& j) {
    const std::string key = job_key(j);
    const double now = clock_, done = j["done"].number();
    const std::string stage = j["stage"].str();
    auto it = speeds_.find(key);
    if (it != speeds_.end() && it->second.stage == stage && done >= it->second.done && now - it->second.t > 0.5 &&
        (done != it->second.done || now - it->second.t > 10)) {
        const double inst = (done - it->second.done) / (now - it->second.t);
        const double rate = it->second.rate ? it->second.rate * 0.75 + inst * 0.25 : inst;
        it->second = {now, done, rate, stage};
    } else if (it == speeds_.end() || it->second.stage != stage) {
        speeds_[key] = {now, done, 0, stage};
    }
}

double App::rate_of(const JsonValue& j) const {
    auto it = speeds_.find(job_key(j));
    return it != speeds_.end() && it->second.stage == j["stage"].str() ? it->second.rate : 0.0;
}

namespace {

// Another engine's path as this engine names it (jobs.ts fromEngine).
std::string from_engine(const std::string& self_id, const std::string& eid, const std::string& p) {
    if (p.empty()) return p;
    const std::string self = self_id.empty() ? "" : "/peer/" + self_id;
    if (!self.empty() && (p == self || starts_with(p, self + "/"))) {
        std::string rest = p.substr(self.size());
        return rest.empty() ? "/" : rest;
    }
    if (starts_with(p, "/peer/")) return p;
    return "/peer/" + eid + (p[0] == '/' ? "" : "/") + p;
}

JsonValue local_view(const JsonValue& j, const std::string& self_id) {
    JsonValue out = j;
    const std::string id = j["engine"]["id"].str();
    JsonValue spec = j["spec"];
    for (const char* k : {"input", "output", "output_dir"})
        if (spec.has(k)) spec.set(k, from_engine(self_id, id, spec[k].str()));
    out.set("spec", spec);
    if (j["result"].is_object()) {
        JsonValue r = j["result"];
        for (const char* k : {"output", "source_hidden", "smp_source"})
            if (!r[k].str().empty()) r.set(k, from_engine(self_id, id, r[k].str()));
        out.set("result", r);
    }
    if (!j.has("raw")) out.set("raw", j);
    return out;
}

}  // namespace

void App::refresh_jobs(bool remote_now) {
    if (jobs_busy_) return;
    jobs_busy_ = true;
    get(
        "/api/jobs",
        [this, remote_now](const Reply& r) {
            jobs_busy_ = false;
            // Only a failed request means the engine is gone.
            if (!r.ok() || r.json.type() != JsonValue::Type::array) {
                if (answered || start_ == Start::none) not_connected = t("Engine not reachable");
                return;
            }
            const bool first = !answered, back = !not_connected.empty();
            not_connected.clear();
            answered = true;
            start_ = Start::none;
            if (first || back || info.is_null()) load_info();
            std::vector<JsonValue> list = r.json.items();
            // Other engines' jobs: every second while one of them works or
            // the queue is open, else every five seconds.
            bool busy = tab == Tab::queue;
            for (const auto& j : remote_jobs_) busy = busy || is_active(j);
            if (any_paired() && !remote_busy_ && (remote_now || clock_ - remote_at_ > (busy ? 0.9 : 5.0))) {
                remote_at_ = clock_;
                remote_busy_ = true;
                get(
                    "/api/peers/jobs",
                    [this](const Reply& pr) {
                        remote_busy_ = false;
                        if (!pr.ok() || pr.json.type() != JsonValue::Type::array) return;
                        remote_jobs_.clear();
                        const std::string self = info["id"].str();
                        for (const auto& j : pr.json.items()) remote_jobs_.push_back(local_view(j, self));
                    },
                    1);
            } else if (!any_paired()) {
                remote_jobs_.clear();
            }
            for (const auto& j : remote_jobs_) list.push_back(j);
            for (const auto& j : list)
                if (j["state"].str() == "running") sample(j);
            // A toast and a fresh listing when a job finishes (one seen
            // running, or a new one already done), not for an engine's first list.
            auto engine_of = [](const JsonValue& j) { return j["engine"].is_object() ? j["engine"]["id"].str() : ""; };
            const JsonValue* newest = nullptr;
            for (const auto& j : list) {
                if (is_active(j)) continue;
                const std::string k = job_key(j);
                const bool fresh = !known_.count(k) && listed_.count(engine_of(j));
                if (!was_active_.count(k) && !fresh) continue;
                if (!newest || j["finished"].number() > (*newest)["finished"].number()) newest = &j;
            }
            if (newest) {
                const std::string name = base((*newest)["spec"]["input"].str());
                if ((*newest)["state"].str() == "done") toast(t("Finished: {name}", {{"name", name}}), Toast::ok);
                if ((*newest)["state"].str() == "failed") toast(t("Failed: {name}", {{"name", name}}), Toast::bad);
                if (cwd != "/" && overlays_.empty()) refresh_listing();
                load_info();
            }
            was_active_.clear();
            known_.clear();
            for (const auto& j : list) {
                if (is_active(j)) was_active_.insert(job_key(j));
                known_.insert(job_key(j));
                listed_.insert(engine_of(j));
            }
            listed_.insert("");
            jobs = std::move(list);
        },
        1);
}

// ---------------------------------------------------------------- paths

const JsonValue* App::root_of(const std::string& path) const {
    const JsonValue* best = nullptr;
    auto consider = [&](const JsonValue& s) {
        const std::string root = s["root"].str();
        if (root.empty()) return;
        if (path == root || starts_with(path, root + "/"))
            if (!best || root.size() > (*best)["root"].str().size()) best = &s;
    };
    for (const auto& s : space.items()) consider(s);
    for (const auto& s : peer_drives.items()) consider(s);
    return best;
}

const JsonValue* App::peer_drive_of(const std::string& path) const {
    const JsonValue* r = root_of(path);
    return r && !(*r)["engine"].str().empty() ? r : nullptr;
}

const JsonValue* App::ftp_of(const std::string& path) const {
    for (const auto& s : ftp.items()) {
        const std::string p = s["path"].str();
        if (!p.empty() && (path == p || starts_with(path, p + "/"))) return &s;
    }
    return nullptr;
}

std::string App::peer_root(const std::string& path) {
    if (!starts_with(path, "/peer/")) return "";
    const std::size_t slash = path.find('/', 6);
    return slash == std::string::npos ? path : path.substr(0, slash);
}

const JsonValue* App::offline_peer(const std::string& path) const {
    const std::string root = peer_root(path);
    if (root.empty()) return nullptr;
    for (const auto& e : peers["engines"].items())
        if ("/peer/" + e["id"].str() == root && e["paired"].boolean() && e["offline"].boolean()) return &e;
    return nullptr;
}

std::string App::drive_name(const std::string& root) const {
    const JsonValue* f = ftp_of(root);
    if (f && (*f)["path"].str() == root) return (*f)["name"].str();
    for (const auto& d : peer_drives.items())
        if (d["root"].str() == root) return d["engine"].str() + " \xC2\xB7 " + place_label(d["label"].str());
    // A whole engine (one that is offline: its drives are not known).
    if (!root.empty() && peer_root(root) == root) {
        for (const auto& e : peers["engines"].items())
            if ("/peer/" + e["id"].str() == root) return e["name"].str();
        return t("another engine");
    }
    if (root == "/data") return t("Internal");
    auto numbered = [&](const char* prefix, const char* key) -> std::string {
        const std::string p = prefix;
        if (starts_with(root, p) && root.size() > p.size()) {
            const std::string n = root.substr(p.size());
            if (n.find_first_not_of("0123456789") == std::string::npos) return t(key, {{"n", n}});
        }
        return "";
    };
    std::string s = numbered("/mnt/usb", "USB {n}");
    if (!s.empty()) return s;
    s = numbered("/mnt/ext", "Extended {n}");
    if (!s.empty()) return s;
    const std::string b = base(root);
    return b.empty() ? root : b;
}

std::string App::shown_path(const std::string& p) const {
    if (!starts_with(p, "/peer/")) return p;
    const std::string rest = p.substr(6);
    const std::size_t slash = rest.find('/');
    const std::string id = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string name;
    for (const auto& e : peers["engines"].items())
        if (e["id"].str() == id) name = e["name"].str();
    if (name.empty())
        for (const auto& d : peer_drives.items())
            if (d["id"].str() == id) name = d["engine"].str();
    if (name.empty()) name = t("another engine");
    std::string local = slash == std::string::npos ? "/" : rest.substr(slash);
    // A Windows engine's C:/...
    if (local.size() > 3 && local[0] == '/' && local[2] == ':' && local[3] == '/') local = local.substr(1);
    return name + ": " + local;
}

std::string App::here_name() const {
    if (is_ps5()) return "PS5";
    const std::string h = info["host_name"].str();
    return h.empty() ? t("This engine") : h;
}

// ---------------------------------------------------------------- meta and icons

const Meta* App::meta(const std::string& path, std::int64_t mtime, double size) {
    const std::string key = path + "|" + std::to_string(mtime) + "|" + std::to_string(static_cast<long long>(size));
    auto it = meta_.find(key);
    if (it == meta_.end()) {
        meta_[key] = MetaSlot{};
        meta_queue_.push_back(key);
        pump_meta();
        return nullptr;
    }
    return it->second.done && it->second.has ? &it->second.meta : nullptr;
}

bool App::meta_none(const std::string& path, std::int64_t mtime, double size) const {
    const std::string key = path + "|" + std::to_string(mtime) + "|" + std::to_string(static_cast<long long>(size));
    auto it = meta_.find(key);
    return it != meta_.end() && it->second.done && !it->second.has;
}

void App::pump_meta() {
    while (meta_active_ < 2 && !meta_queue_.empty()) {
        const std::string key = meta_queue_.front();
        meta_queue_.erase(meta_queue_.begin());
        const std::string path = key.substr(0, key.find('|'));
        ++meta_active_;
        get(
            url("/api/meta", {{"path", path}}),
            [this, key](const Reply& r) {
                --meta_active_;
                MetaSlot& slot = meta_[key];
                slot.done = true;
                if (r.ok() && r.json.is_object()) {
                    slot.has = true;
                    slot.meta.title = r.json["title"].str();
                    slot.meta.title_id = r.json["title_id"].str();
                    slot.meta.content_id = r.json["content_id"].str();
                    slot.meta.version = r.json["version"].str();
                    slot.meta.icon = r.json["icon"].boolean();
                }
                pump_meta();
            },
            1);
    }
}

std::uint32_t App::icon(const std::string& path, std::int64_t mtime) {
    const std::string key = path + "|" + std::to_string(mtime);
    auto it = icons_.find(key);
    if (it != icons_.end()) return it->second.texture;
    icons_[key] = IconSlot{};
    icon_queue_.push_back({key, path + "|" + std::to_string(mtime)});
    pump_icons();
    return 0;
}

// Two icon fetches at a time; each one that ends starts the next.
void App::pump_icons() {
    while (icons_active_ < 2 && !icon_queue_.empty()) {
        const std::string key = icon_queue_.front().first;
        const std::string spec = icon_queue_.front().second;
        icon_queue_.erase(icon_queue_.begin());
        const std::size_t bar = spec.rfind('|');
        ++icons_active_;
        engine.fetch(
            url("/api/icon", {{"path", spec.substr(0, bar)}, {"v", spec.substr(bar + 1)}}),
            [](int status, std::string& body) {
                Image img;
                if (status == 200 && decode_png(body, 128, &img))
                    body = pack_image(img);
                else
                    body.clear();
            },
            [this, key](int, std::string body) {
                --icons_active_;
                auto slot = icons_.find(key);
                if (slot != icons_.end()) {
                    slot->second.done = true;
                    Image img;
                    if (unpack_image(body, &img) && platform.make_texture)
                        slot->second.texture = platform.make_texture(img.width, img.height, img.rgba.data());
                }
                pump_icons();
            });
    }
}

void App::clear_meta() {
    meta_.clear();
    meta_queue_.clear();
    for (auto& [k, slot] : icons_)
        if (slot.texture && platform.free_texture) platform.free_texture(slot.texture);
    icons_.clear();
    icon_queue_.clear();
}

// ---------------------------------------------------------------- Files

void App::browse(const std::string& path, const std::string& focus_key) {
    const std::string from = cwd;
    get(url("/api/browse", {{"path", path.empty() ? "/" : path}}), [this, from, focus_key, path](const Reply& r) {
        if (!r.ok()) {
            toast(t_msg(r.error()), Toast::bad);
            // Another engine went away while we were on it: back to the drives.
            const std::string peer = peer_root(path);
            if (!peer.empty() && peer_root(cwd) == peer) browse("/");
            return;
        }
        const std::string p = r.json["path"].str("/");
        const bool same = p == from;
        const bool listing_loaded_before = listing_loaded;
        std::vector<Entry> list;
        for (const auto& e : r.json["entries"].items()) list.push_back(entry_from(e));
        // Back from a folder: land on that folder's row.
        std::string key = focus_key;
        if (key.empty() && !same && !from.empty() && dir(from) == p && from != "/") key = from;
        if (key.empty() && same) key = files_->focused_path;
        cwd = p;
        entries = std::move(list);
        listing_loaded = true;
        listing_path = p;
        focus_after = key;
        focus_list = !same || !listing_loaded_before;
    });
}

void App::refresh_listing() {
    if (!cwd.empty()) browse(cwd);
    load_info();
}

// ---------------------------------------------------------------- sections, dialogs, toasts

void App::show_tab(Tab next) {
    if (tab == next) return;
    tab = next;
    if (next == Tab::deps) deps_->load(*this);
    if (next == Tab::settings) settings_->opened(*this);
}

void App::open(std::unique_ptr<Overlay> overlay) {
    overlay->key = next_key_++;
    overlay->opened_frame = frame_no_;
    overlays_.push_back(std::move(overlay));
    if (feedback_hook_) feedback_hook_(hui::audio::Cue::modal_open);
}

void App::close(Overlay* overlay) {
    if (!overlay || overlay->closing) return;
    overlay->closing = true;
    if (feedback_hook_) feedback_hook_(hui::audio::Cue::modal_close);
}

bool App::overlay_alive(int key) const {
    for (const auto& o : overlays_)
        if (o->key == key && !o->closing) return true;
    return false;
}

void App::toast(const std::string& text, Toast kind) {
    toasts_.push_back({text, kind, kind == Toast::bad ? 7.0f : 4.0f, 0.0f});
    if (toasts_.size() > 3) toasts_.erase(toasts_.begin());
}

// ---------------------------------------------------------------- prefs

void App::save_prefs() {
    if (platform.write_file) platform.write_file("prefs.json", prefs.dump());
}

std::vector<std::string> App::recent() const {
    std::vector<std::string> out;
    for (const auto& v : prefs["recent"].items()) out.push_back(v.str());
    return out;
}

void App::remember_folder(const std::string& path) {
    for (const auto& l : locations.items())
        if (l["path"].str() == path) return;
    JsonValue list = JsonValue::array();
    list.push(path);
    int n = 1;
    for (const auto& p : recent())
        if (p != path && n < 5) {
            list.push(p);
            ++n;
        }
    prefs.set("recent", list);
    save_prefs();
}

// ---------------------------------------------------------------- the frame

void App::tick(float dt) {
    clock_ += dt;
    time += dt;
    for (auto& t : toasts_) {
        t.left -= dt;
        t.age += dt;
    }
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const ToastItem& t) { return t.left <= 0; }),
                  toasts_.end());
    jobs_timer_ += dt;
    if (jobs_timer_ >= 1.0f) {
        jobs_timer_ = 0.0f;
        refresh_jobs();
    }
    if (answered) {
        info_timer_ += dt;
        if (info_timer_ >= 15.0f) {
            info_timer_ = 0.0f;
            load_info();
        }
        incoming_timer_ += dt;
        if (incoming_timer_ >= 2.0f) {
            incoming_timer_ = 0.0f;
            watch_incoming();
        }
    }
    // No engine answers: start the one in the app (PS5) once.
    if (!answered && start_ == Start::waiting) {
        start_timer_ += dt;
        if (start_timer_ > 1.5f) {
            if (platform.start_engine) {
                std::string why;
                if (platform.start_engine(&why)) {
                    start_ = Start::sent;
                    start_timer_ = 0.0f;
                } else {
                    start_ = Start::failed;
                    start_error_ = why;
                }
            } else {
                start_ = Start::failed;
            }
        }
    }
    if (restart_ != Restart::none) {
        restart_timer_ += dt;
        if (!not_connected.empty()) restart_gone_ = true;
        // The new engine can take over the port between two polls: a new start time tells too.
        if (!restart_from_.empty() && info.has("started") && info["started"].dump() != restart_from_) restart_gone_ = true;
        restart_poll_ += dt;
        if (restart_ == Restart::waiting && restart_poll_ > 1.0f) {
            restart_poll_ = 0.0f;
            load_info();
        }
        if (restart_ == Restart::start_own && restart_gone_) {
            std::string why;
            if (platform.start_engine && platform.start_engine(&why)) {
                restart_ = Restart::waiting;
            } else {
                restart_ = Restart::none;
                toast(why, Toast::bad);
            }
        } else if (restart_ == Restart::waiting && restart_gone_ && not_connected.empty()) {
            restart_ = Restart::none;
            toast(t("Engine restarted"), Toast::ok);
        } else if (restart_timer_ > 90.0f) {
            restart_ = Restart::none;
            toast(t("The engine did not answer after it was started."), Toast::bad);
        }
    }
    if (start_ == Start::sent) {
        start_timer_ += dt;
        if (start_timer_ > 30.0f) {
            start_ = Start::failed;
            start_error_ = t("The engine did not answer after it was started.");
        }
    }
}

void App::restart_engine() {
    if (restart_ != Restart::none) return;
    post("/api/restart", JsonValue::object(), [this](const Reply& r) {
        restart_timer_ = 0.0f;
        restart_gone_ = false;
        restart_from_ = info.has("started") ? info["started"].dump() : "";
        if (r.ok()) {
            restart_ = Restart::waiting;
            toast(t("Restarting the engine\xE2\x80\xA6"), Toast::ok);
            return;
        }
        // No copy of its payload, or an engine before 0.3.0: stop it and
        // start the app's own, unless that one is older than it.
        const std::string running = info["version"].str();
        if (!platform.start_engine || running.empty() || compare_versions(platform.version, running) < 0) {
            toast(t_msg(r.error()), Toast::bad);
            return;
        }
        post("/api/quit", JsonValue::object(), [this](const Reply& q) {
            if (!q.ok()) {
                toast(t_msg(q.error()), Toast::bad);
                return;
            }
            restart_ = Restart::start_own;
            toast(t("Restarting the engine\xE2\x80\xA6"), Toast::ok);
        });
    });
}

void App::start_own_engine() {
    if (restart_ != Restart::none || !platform.start_engine) return;
    std::string why;
    if (!platform.start_engine(&why)) {
        toast(why, Toast::bad);
        return;
    }
    restart_ = Restart::waiting;
    restart_timer_ = 0.0f;
    restart_gone_ = true;
    restart_from_.clear();
    toast(t("Starting the engine\xE2\x80\xA6"), Toast::ok);
}

void App::frame(Ui& ui) {
    feedback_hook_ = [&ui](hui::audio::Cue c) { ui.play(c); };
    ++frame_no_;
    tick(ui.dt);
    engine.pump();
    DrawList& l = *ui.list;
    // The page's background: a soft light at the top left on --bg.
    l.rounded_rect({0, 0, W, H}, 0.0f, bg);
    l.glow({-0.2f * W, -0.55f * H, 0.8f * W, 0.85f * H}, 0.4f * H, 0.5f * H, rgb(0x1a2340, 0.55f));

    const Rect area{0, kHeaderH, W, H - kHeaderH - kHintsH};
    const bool top_free = overlays_.empty();
    ui.begin_layer(0, top_free);
    if (!answered) {
        draw_connecting(ui);
    } else {
        switch (tab) {
        case Tab::files: files_->build(ui, *this, area); break;
        case Tab::queue: queue_->build(ui, *this, area); break;
        case Tab::deps: deps_->build(ui, *this, area); break;
        case Tab::settings: settings_->build(ui, *this, area); break;
        }
    }
    if (top_free) handle_global(ui);
    ui.end_layer();
    draw_header(ui);

    // Dialogs, bottom to top; only the top one takes input.
    for (std::size_t i = 0; i < overlays_.size(); ++i) {
        Overlay* o = overlays_[i].get();
        const bool active = i + 1 == overlays_.size() && !o->closing && o->opened_frame < frame_no_;
        ui.begin_layer(o->key, active);
        o->build(ui, *this);
        if (active && !o->closing) {
            if (ui.pressed(Action::back)) {
                ui.play(hui::audio::Cue::back);
                o->back(*this);
            } else if (ui.pressed(Action::west)) {
                o->square(*this);
            }
        }
        ui.end_layer();
    }
    // Closed dialogs leave once their frame is over.
    overlays_.erase(std::remove_if(overlays_.begin(), overlays_.end(),
                                   [](const std::unique_ptr<Overlay>& o) { return o->closing; }),
                    overlays_.end());
    draw_hints(ui, top() ? top()->hints(*this) : page_hints());
    draw_toasts(ui);
    feedback_hook_ = nullptr;
}

void App::handle_global(Ui& ui) {
    if (ui.pressed(Action::page_prev) || ui.pressed(Action::page_next)) {
        static const Tab order[] = {Tab::files, Tab::queue, Tab::deps, Tab::settings};
        int i = 0;
        while (order[i] != tab) ++i;
        i = (i + (ui.pressed(Action::page_next) ? 1 : 3)) % 4;
        show_tab(order[i]);
        ui.play(hui::audio::Cue::tab);
        return;
    }
    if (!answered) return;
    if (tab == Tab::files) {
        if (ui.pressed(Action::west)) files_->square(*this);
        if (ui.pressed(Action::back) && cwd != "/") {
            ui.play(hui::audio::Cue::back);
            const JsonValue* r = root_of(cwd);
            const JsonValue* f = ftp_of(cwd);
            const bool at_root = (r && (*r)["root"].str() == cwd) || (f && (*f)["path"].str() == cwd);
            browse(at_root ? "/" : dir(cwd));
        }
    }
}

std::vector<Hint> App::page_hints() {
    if (!answered) return {};
    std::vector<Hint> list;
    if (tab == Tab::files) {
        list = files_->hints(*this);
    } else {
        list = {{Glyph::cross, t("Choose")}};
    }
    list.push_back({Glyph::l1r1, t("Sections")});
    return list;
}

// ---------------------------------------------------------------- header, hints, toasts

void App::draw_header(Ui& ui) {
    DrawList& l = *ui.list;
    const float cy = 1.1f * R + 1.225f * R;
    float x = page_pad;
    // Logo and name (.logo).
    if (platform.logo) l.image(platform.logo, {x, cy - 1.0f * R, 2.0f * R, 2.0f * R}, {0, 0, 1, 1}, {1, 1, 1, 1}, 0.4f * R);
    x += 2.0f * R + 0.65f * R;
    x += draw::text(ui, Face::semibold, "Alembic", x, draw::baseline(cy, 1.3f * R), 1.3f * R, style::text);
    x += 2.0f * R;
    // Tabs between their shoulder keys (.tabs).
    auto key = [&](const char* label) {
        const float s = 0.7f * R;
        const float w = std::max(1.5f * R, draw::measure(ui, Face::semibold, label, s) + 0.7f * R);
        const Rect r{x + 0.5f * R, cy - 0.65f * R, w, 1.3f * R};
        l.bordered_rect(r, 0.35f * R, Color{0, 0, 0, 0}, 1.0f, line2);
        draw::text(ui, Face::semibold, label, r.cx(), draw::baseline(r.cy(), s), s, dim, Align::center);
        x += w + 1.0f * R;
    };
    key("L1");
    int active = 0;
    for (const auto& j : jobs) active += is_active(j) ? 1 : 0;
    struct TabDef {
        Tab id;
        std::string label;
    };
    const TabDef tabs[] = {{Tab::files, t("Files")}, {Tab::queue, t("Queue")}, {Tab::deps, t("Components")},
                           {Tab::settings, t("Settings")}};
    for (const auto& td : tabs) {
        const float s = 1.05f * R;
        const bool on = tab == td.id;
        const float tw = draw::measure(ui, Face::regular, td.label, s);
        float w = tw + 1.8f * R;
        std::string badge;
        float bw = 0.0f;
        if (td.id == Tab::queue && active) {
            badge = std::to_string(active);
            bw = std::max(1.2f * R, draw::measure(ui, Face::semibold, badge, 0.72f * R) + 0.9f * R);
            w += bw + 0.35f * R;
        }
        draw::text(ui, Face::regular, td.label, x + 0.9f * R, draw::baseline(cy, s), s, on ? style::text : muted);
        if (!badge.empty()) {
            const Rect b{x + 0.9f * R + tw + 0.35f * R, cy - 0.55f * R, bw, 1.1f * R};
            l.rounded_rect(b, b.h * 0.5f, accent);
            draw::text(ui, Face::semibold, badge, b.cx(), draw::baseline(b.cy(), 0.72f * R), 0.72f * R, {1, 1, 1, 1},
                       Align::center);
        }
        if (on) l.rounded_rect({x + 0.9f * R, cy + 1.05f * R * 0.725f + 0.45f * R - 0.21f * R, w - 1.8f * R, 0.16f * R},
                               0.1f * R, accent);
        x += w + 0.25f * R;
    }
    key("R1");
    // The engine's status (.status), on the right.
    int running = 0, queued = 0;
    for (const auto& j : jobs) {
        const std::string s = j["state"].str();
        running += s == "running";
        queued += s == "queued" || s == "paused";
    }
    std::string text;
    if (!not_connected.empty())
        text = not_connected;
    else if (!info.is_null())
        text = (running ? t("Working") : t("Idle")) +
               (queued ? " \xC2\xB7 " + t("{n} waiting", {{"n", std::to_string(queued)}}) : "");
    else
        text = answered ? t("Not connected") : t("connecting\xE2\x80\xA6");
    const float s = 0.85f * R;
    const float right = W - page_pad;
    const float tw = draw::measure(ui, Face::regular, text, s);
    draw::text(ui, Face::regular, text, right, draw::baseline(cy, s), s, muted, Align::right);
    const bool off = !not_connected.empty() || (answered && info.is_null());
    const bool live = not_connected.empty() && running;
    const float dx = right - tw - 0.5f * R - 0.275f * R;
    if (live) l.glow({dx - 0.275f * R, cy - 0.275f * R, 0.55f * R, 0.55f * R}, 0.275f * R, 0.6f * R, accent.with_alpha(0.8f));
    l.circle(dx, cy, 0.275f * R, off ? bad : live ? accent : dim);
}

namespace {

// The page's glyphs (Icons.tsx), or a key box for L1/R1 and L2/R2. With no
// list it only measures.
float glyph(Ui& ui, DrawList* l, Glyph g, float x, float cy, bool swap) {
    const float s = 1.35f * R, k = s / 24.0f;
    auto keybox = [&](const char* label, float at) {
        const float ks = 0.72f * R;
        const float w = std::max(1.5f * R, draw::measure(ui, Face::semibold, label, ks) + 0.7f * R);
        if (!l) return w;
        const Rect r{at, cy - 0.68f * R, w, 1.36f * R};
        l->rounded_rect({r.x, r.y + 0.15f * R, r.w, r.h - 0.15f * R}, 0.35f * R, line2);
        l->bordered_rect({r.x, r.y, r.w, r.h - 0.15f * R}, 0.35f * R, bg, 1.0f, line2);
        draw::text(ui, Face::semibold, label, r.cx(), draw::baseline(r.y + (r.h - 0.15f * R) * 0.5f, ks), ks, muted,
                   Align::center);
        return w;
    };
    if (swap && (g == Glyph::cross || g == Glyph::circle)) g = g == Glyph::cross ? Glyph::circle : Glyph::cross;
    switch (g) {
    case Glyph::cross:
        if (l) {
            l->line(x + 6 * k, cy - 6 * k, x + 18 * k, cy + 6 * k, 2.6f * k, glyph_x);
            l->line(x + 18 * k, cy - 6 * k, x + 6 * k, cy + 6 * k, 2.6f * k, glyph_x);
        }
        return s;
    case Glyph::circle:
        if (l) l->ring(x + 12 * k, cy, 6.5f * k, 2.4f * k, glyph_o);
        return s;
    case Glyph::square:
        if (l) l->bordered_rect({x + 6 * k, cy - 6 * k, 12 * k, 12 * k}, 1 * k, Color{0, 0, 0, 0}, 2.3f * k, glyph_s);
        return s;
    case Glyph::triangle:
        if (l) l->triangle({x + 4.5f * k, cy - 7 * k, 15 * k, 13 * k}, teal, 2.3f * k);
        return s;
    case Glyph::dpad:
        if (l) {
            const Color c = rgb(0xc9d1e2, 0.9f);
            l->rounded_rect({x + 9.5f * k, cy - 9 * k, 5 * k, 5.5f * k}, 0.0f, c);
            l->rounded_rect({x + 9.5f * k, cy + 3.5f * k, 5 * k, 5.5f * k}, 0.0f, c);
            l->rounded_rect({x + 3 * k, cy - 2.5f * k, 5.5f * k, 5 * k}, 0.0f, c);
            l->rounded_rect({x + 15.5f * k, cy - 2.5f * k, 5.5f * k, 5 * k}, 0.0f, c);
        }
        return s;
    case Glyph::options: return keybox("OPTIONS", x);
    case Glyph::l1r1: {
        const float a = keybox("L1", x);
        return a + 0.2f * R + keybox("R1", x + a + 0.2f * R);
    }
    case Glyph::l2r2: {
        const float a = keybox("L2", x);
        return a + 0.2f * R + keybox("R2", x + a + 0.2f * R);
    }
    }
    return s;
}

}  // namespace

void App::draw_hints(Ui& ui, const std::vector<Hint>& hints) {
    DrawList& l = *ui.list;
    const float top = H - kHintsH;
    l.rounded_rect({0, top, W, kHintsH}, 0.0f, bg);
    draw::hline(ui, 0, top, W, rgb(0x263044, 0.6f));
    const bool swap = info["enter_button"].str() == "circle";
    const float cy = top + 0.7f * R + 0.25f * R + 0.725f * R;
    const float s = 0.95f * R;
    // Right-aligned: measure first.
    float total = 0.0f;
    std::vector<float> widths;
    for (const auto& h : hints) {
        const float gw = glyph(ui, nullptr, h.glyph, 0, 0, swap);
        const float w = 0.55f * R + gw + 0.45f * R + draw::measure(ui, Face::regular, h.label, s) + 0.55f * R;
        widths.push_back(w);
        total += w + 0.6f * R;
    }
    float x = W - page_pad - total + 0.6f * R;
    for (std::size_t i = 0; i < hints.size(); ++i) {
        const float gw = glyph(ui, &l, hints[i].glyph, x + 0.55f * R, cy, swap);
        draw::text(ui, Face::regular, hints[i].label, x + 0.55f * R + gw + 0.45f * R, draw::baseline(cy, s), s,
                   style::text);
        x += widths[i] + 0.6f * R;
    }
}

void App::draw_toasts(Ui& ui) {
    DrawList& l = *ui.list;
    float bottom = H - 4.3f * R * 1.2f;
    const float s = 0.95f * R;
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        const auto& t = *it;
        const float maxw = 30.0f * R;
        const float tw = std::min(maxw - 2.2f * R - 1.2f * R, draw::measure(ui, Face::regular, t.text, s));
        const float th = draw::paragraph_height(ui, Face::regular, t.text, maxw - 3.4f * R, s, s * 1.45f, 3);
        const float h = th + 1.2f * R;
        const float w = tw + 2.2f * R + 1.2f * R;
        const float rise = std::max(0.0f, 1.0f - t.age / 0.2f);
        const float alpha = std::min(1.0f, t.age / 0.2f) * std::min(1.0f, t.left / 0.3f);
        const Rect r{page_pad, bottom - h + rise * 0.5f * R, w, h};
        l.push_opacity(alpha);
        l.shadow({r.x, r.y + 0.6f * R, r.w, r.h}, h * 0.5f, 1.5f * R, rgb(0x000000, 0.4f));
        l.bordered_rect(r, std::min(h * 0.5f, 1.4f * R), panel2, 1.0f, line2);
        const Color dot = t.kind == Toast::bad ? bad : t.kind == Toast::ok ? ok : accent;
        l.circle(r.x + 1.1f * R + 0.25f * R, r.cy(), 0.25f * R, dot);
        draw::paragraph(ui, Face::regular, t.text, r.x + 1.1f * R + 0.5f * R + 0.7f * R, r.y + 0.6f * R,
                        maxw - 3.4f * R, s, s * 1.45f, style::text, 3);
        l.pop_opacity();
        bottom = r.y - 0.5f * R;
    }
}

void App::draw_connecting(Ui& ui) {
    const float cy = H * 0.42f;
    std::string title, sub;
    if (start_ == Start::failed) {
        title = t("Engine not reachable");
        sub = start_error_.empty()
                  ? t("No engine answers on port {port}. Load alembic.elf with your payload loader, then try again.",
                      {{"port", std::to_string(engine.port())}})
                  : start_error_;
    } else if (start_ == Start::sent) {
        title = t("Starting the engine\xE2\x80\xA6");
        sub = t("The app sent its engine to the ELF loader on port 9021.");
    } else {
        title = t("connecting\xE2\x80\xA6");
    }
    draw::text(ui, Face::semibold, title, W * 0.5f, draw::baseline(cy, 1.6f * R), 1.6f * R, style::text, Align::center);
    if (!sub.empty()) {
        const float w = 40.0f * R;
        draw::paragraph(ui, Face::regular, sub, W * 0.5f - w * 0.5f, cy + 1.6f * R, w, 1.0f * R, 1.45f * R, muted);
    }
    if (start_ == Start::failed) {
        const std::string label = t("Try again");
        const float bw = draw::button_width(ui, label, false);
        if (draw::button(ui, "start:retry", {W * 0.5f - bw * 0.5f, cy + 6.0f * R, bw, 2.55f * R + 2.0f}, label,
                         draw::Button::primary, false, false, kAutofocus)) {
            start_ = Start::waiting;
            start_timer_ = 0.0f;
            start_error_.clear();
        }
    } else {
        // A small spinner.
        const float a = ui.time * 5.0f;
        ui.list->arc(W * 0.5f, cy + 6.0f * R, 1.0f * R, 0.18f * R, a, 4.2f, accent);
    }
}

// ---------------------------------------------------------------- notices

void App::notice_update() {
    const JsonValue& u = info["update"];
    // An engine newer than this app (updated from the page, or by hand):
    // once per engine version and run of the app.
    const std::string engine_version = info["version"].str();
    if (platform.console && !engine_version.empty() && engine_version != mismatch_told_ &&
        compare_versions(engine_version, platform.version) > 0) {
        mismatch_told_ = engine_version;
        toast(u["app_pending"].boolean()
                  ? t("Alembic {version} is installed: close the app to finish updating it.", {{"version", engine_version}})
                  : t("This app is {app}, the engine {engine}: Settings > Updates",
                      {{"app", platform.version}, {"engine", engine_version}}),
              Toast::ok);
    }
    if (!u.is_object() || !u["newer"].boolean()) return;
    const std::string v = u["latest"].str();
    if (v.empty() || v == update_told_ || prefs["updateTold"].str() == v) return;
    update_told_ = v;
    prefs.set("updateTold", v);
    save_prefs();
    toast(t("Alembic {version} is available: Settings > Updates", {{"version", v}}), Toast::ok);
}

void App::watch_incoming() {
    // The app runs on the engine's own console: it may show the code another
    // engine asks for (the page does so only on the engine's computer).
    get(
        "/api/pair/incoming",
        [this](const Reply& r) {
            const JsonValue& p = r.json["pairing"];
            const bool has = r.ok() && p.is_object() && !p["code"].str().empty();
            if (has && !incoming_key_) {
                const std::string code = p["code"].str(), from = p["from"].str();
                incoming_from_ = from;
                ask(
                    *this, t("{name} wants to pair with this engine", {{"name", from}}),
                    [code, from](Ui& ui, App&, float x, float y, float w) -> float {
                        float h = draw::paragraph(ui, Face::regular, t("Enter this code on {name}:", {{"name", from}}), x,
                                                  y, w, 1.0f * R, 1.45f * R, style::text);
                        const float cs = 2.6f * R;
                        draw::text(ui, Face::mono, code, x + w * 0.5f, draw::baseline(y + h + 0.6f * R + cs * 0.7f, cs),
                                   cs, style::text, Align::center, cs * 0.15f);
                        return h + 0.6f * R + cs * 1.45f + 0.6f * R;
                    },
                    t("OK"), false, nullptr, [this] { post("/api/pair/incoming/cancel", JsonValue::object(), nullptr); });
                incoming_key_ = top() ? top()->key : 0;
            } else if (!has && incoming_key_) {
                for (const auto& o : overlays_)
                    if (o->key == incoming_key_) close(o.get());
                incoming_key_ = 0;
                const std::string from = incoming_from_;
                get("/api/peers", [this, from](const Reply& pr) {
                    if (!pr.ok()) return;
                    peers = pr.json;
                    for (const auto& e : peers["engines"].items())
                        if (e["name"].str() == from && e["paired"].boolean())
                            toast(t("Paired with {name}", {{"name", from}}), Toast::ok);
                });
            }
        },
        1);
}

}  // namespace alembic::app
