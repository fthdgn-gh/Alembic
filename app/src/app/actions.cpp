// SPDX-License-Identifier: GPL-3.0-or-later
// What can be done to files and jobs (pages/files/actions.tsx,
// pages/queue/jobActions.ts) and the dialogs behind it: the folder picker
// (dialogs/FolderPicker.tsx) and Convert (dialogs/ConvertDialog.tsx).
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>
#include <map>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

void job_text(App& app, const JsonValue& j, std::string* title, std::string* to);
std::string job_cp_text(const JsonValue& j);

namespace {

bool is_hidden_name(const std::string& name) { return name.size() > 1 && name[0] == '.'; }

std::string short_place(std::string label) {
    const std::size_t hb = label.find(" \xC2\xB7 homebrew");
    if (hb != std::string::npos) label = label.substr(0, hb) + " \xC2\xB7 homebrew";
    return place_label(label);
}

}  // namespace

// ---------------------------------------------------------------- Options

void App::open_actions(const Entry& e) {
    if (cwd == "/") return;
    const bool conv = is_convertible(e);
    std::vector<Choice> items;
    auto add = [&](std::string label, std::string sub, bool danger, std::function<void()> act) {
        Choice c;
        c.label = std::move(label);
        c.sub = std::move(sub);
        c.danger = danger;
        c.act = std::move(act);
        items.push_back(std::move(c));
    };
    if (e.kind == "folder" && e.path != cwd) add(t("Open"), "", false, [this, e] { browse(e.path); });
    if (conv) add(t("Convert\xE2\x80\xA6"), t("To FFPFSC, FFPKG, exFAT or a folder"), false, [this, e] { open_convert(e); });
    if (e.kind == "fpkg")
        add(t("Install"), t("Hand it to the console's installer"), false, [this, e] {
            if (!is_ps5()) {
                toast(t("Installing needs the engine running on a PS5"), Toast::bad);
                return;
            }
            ask(*this, t("Install this package?"),
                text_body("@" + shown_path(e.path) + "\n\n~" +
                          t("The console installs it in the background; follow it in Downloads. Keep the file until the "
                            "install has finished.")),
                t("Install"), false, [this, e] {
                    JsonValue body = JsonValue::object();
                    body.set("kind", "install");
                    body.set("input", e.path);
                    post("/api/jobs", body, or_toast([this](const Reply&) {
                        toast(t("Install queued"), Toast::ok);
                        refresh_jobs();
                    }));
                });
        });
    if (conv && e.kind != "fpkg")
        add(t("Add the AMPR emulator\xE2\x80\xA6"), t("For games that stop at start from an image or folder"), false, [this, e] {
            JsonValue req = JsonValue::object();
            req.set("kind", "ampr_emu");
            req.set("input", e.path);
            api("POST", "/api/plan", req, or_toast([this, e](const Reply& r) {
                    const JsonValue plan = r.json;
                    if (plan["ships_emulator"].boolean()) {
                        toast(t("This game already has the AMPR emulator"), Toast::ok);
                        return;
                    }
                    const bool folder = e.kind == "folder";
                    std::string body = "@" + shown_path(e.path) + "\n\n";
                    body += plan.has("imports_ampr") && !plan["imports_ampr"].boolean()
                                ? t("This game does not seem to use the console's APR, so it may not need the emulator. It does no harm to add it.")
                                : t("This game reads its files through the console's APR, which doesn't work when it runs from an image or folder. ampr_emu's libSceAmpr.sprx stands in for it.");
                    body += "\n\n~";
                    body += folder ? t("It goes into the game folder's fakelib (and ampr_emu.index, if the game has none).")
                                   : t("The image is not changed: ShadowMount+ gets a backport folder for this title (the game's fakelib files plus the emulator), which it lays over the game when it starts.");
                    ask(*this, t("Add the AMPR emulator?"), text_body(body), t("Add"), false, [this, e, plan] {
                        get_needs(plan, "This needs {name}, which isn't downloaded yet.", [this, e](bool ok) {
                            if (!ok) return;
                            JsonValue job = JsonValue::object();
                            job.set("kind", "ampr_emu");
                            job.set("input", e.path);
                            post("/api/jobs", job, or_toast([this, e](const Reply&) {
                                toast(t("Queued: {name}", {{"name", e.name}}), Toast::ok);
                                refresh_jobs();
                            }));
                        });
                    });
                }),
                1, 120000);
        });
    if (conv && e.kind != "fpkg") {
        if (is_hidden_name(e.name))
            add(t("Show to ShadowMount+ again"), t("Renames it back to {name}", {{"name", e.name.substr(1)}}), false,
                [this, e] { toggle_hidden(e.path); });
        else
            add(t("Hide from ShadowMount+"),
                t("Renames it to .{name}, so ShadowMount+ uses another copy of this game", {{"name", e.name}}), false,
                [this, e] { toggle_hidden(e.path); });
    }
    // Places (this engine's or a paired one's): shared with every paired engine.
    if (e.kind == "folder") {
        bool is_place = false;
        for (const auto& l : locations.items()) is_place = is_place || (l["custom"].boolean() && l["path"].str() == e.path);
        if (is_place)
            add(t("Remove from Places"), "", false, [this, e] { set_place(e.path, false); });
        else
            add(t("Add to Places"), t("Shows it in Places and Save to, here and on paired engines"), false,
                [this, e] { set_place(e.path, true); });
    }
    // A paired engine's usual destination, for a copy without converting.
    std::string peer_place;
    for (const auto& l : locations.items())
        if (!l["peer"].str().empty() && peer_place.empty()) peer_place = l["path"].str();
    if (peer_place.empty() && !peer_drives.items().empty()) peer_place = peer_drives.items()[0]["root"].str();
    if (!peer_place.empty() && !e.shared)
        add(t("Send to another engine\xE2\x80\xA6"), t("A copy as it is, without converting"), false,
            [this, e, peer_place] { open_dest(&e, "copy", peer_place); });
    add(t("Copy to\xE2\x80\xA6"), "", false, [this, e] { open_dest(&e, "copy", ""); });
    add(t("Move to\xE2\x80\xA6"), "", false, [this, e] { open_dest(&e, "move", ""); });
    add(t("Rename\xE2\x80\xA6"), "", false, [this, e] {
        ask_text(*this, t("Rename"), e.name, t("Rename"), [this, e](const std::string& name) {
            if (name.empty() || name == e.name) return;
            JsonValue body = JsonValue::object();
            body.set("path", e.path);
            body.set("name", name);
            post("/api/fs/rename", body, or_toast([this, e, name](const Reply&) {
                toast(t("Renamed to {name}", {{"name", name}}), Toast::ok);
                browse(cwd, dir(e.path) + "/" + name);
            }));
        });
    });
    add(t("Delete\xE2\x80\xA6"), "", true, [this, e] {
        std::string body = "!" + std::string(e.kind == "folder" ? t("Delete this folder and everything in it?")
                                                                : t("Delete this file?")) +
                           "\n" + shown_path(e.path);
        if (e.has_size()) body += "\n" + bytes(e.size);
        body += "\n\n~" + t("This can't be undone.");
        ask(*this, t("Delete"), text_body(body), t("Delete"), true, [this, e] {
            JsonValue req = JsonValue::object();
            req.set("path", e.path);
            api("POST", "/api/fs/delete", req, or_toast([this, e](const Reply&) {
                    toast(t("Deleted {name}", {{"name", e.name}}), Toast::ok);
                    browse(cwd);
                    load_info();
                }),
                0, 120000);
        });
    });
    choose(*this, e.name, std::move(items));
}

void App::set_place(const std::string& path, bool add) {
    JsonValue body = JsonValue::object();
    body.set("path", path);
    body.set("remove", !add);
    post("/api/places", body, or_toast([this, add](const Reply& r) {
        locations = r.json;
        toast(add ? t("Added to Places") : t("Removed from Places"), Toast::ok);
    }));
}

// ShadowMount+ skips names that start with "." and keeps one source per game
// while that source exists: hiding the old one makes it switch.
void App::toggle_hidden(const std::string& path, bool quiet) {
    const std::string n = base(path);
    const std::string name = is_hidden_name(n) ? n.substr(1) : "." + n;
    JsonValue body = JsonValue::object();
    body.set("path", path);
    body.set("name", name);
    post("/api/fs/rename", body, or_toast([this, path, name, quiet](const Reply&) {
        post("/api/smp/scan", JsonValue::object(), [this, path, name, quiet](const Reply& r) {
            const bool scanned = r.ok();
            if (!quiet) {
                if (is_hidden_name(name))
                    toast(scanned ? t("Hidden from ShadowMount+; it is rescanning") : t("Hidden from ShadowMount+"),
                          Toast::ok);
                else
                    toast(scanned ? t("Visible to ShadowMount+ again; it is rescanning") : t("Visible to ShadowMount+ again"),
                          Toast::ok);
            }
            if (tab == Tab::files && cwd == dir(path)) browse(cwd, dir(path) + "/" + name);
        });
    }));
}

void App::get_needs(const JsonValue& plan, const std::string& intro, std::function<void(bool)> done) {
    std::vector<JsonValue> needs = plan["needs"].items();
    if (needs.empty()) {
        done(true);
        return;
    }
    // One component at a time, each asked for.
    struct Step {
        static void run(App& app, std::vector<JsonValue> rest, std::string intro, std::function<void(bool)> done) {
            if (rest.empty()) {
                done(true);
                return;
            }
            const JsonValue dep = rest.front();
            rest.erase(rest.begin());
            const std::string label = t(dep["label"].str());
            Body body = [dep, intro, label](Ui& ui, App&, float x, float y, float w) -> float {
                float h = draw::paragraph(ui, Face::regular, t(intro, {{"name", label}}), x, y, w, 1.0f * R, 1.45f * R,
                                          style::text);
                h += 0.8f * R;
                h += facts(ui, x, y + h, w,
                           {{t("Why"), t(dep["purpose"].str()), ""},
                            {t("Source"), dep["source"].str(), ""},
                            {t("Size"), bytes(dep["bytes"].number()), ""}});
                h += 0.8f * R;
                h += draw::paragraph(ui, Face::regular, t("It's checked against a pinned SHA-256 before it's saved."), x,
                                     y + h, w, 1.0f * R, 1.45f * R, muted);
                return h;
            };
            ask(
                app, t("Download {name}?", {{"name", label}}), body, t("Download and continue"), false,
                [&app, dep, rest, intro, done, label] {
                    app.toast(t("Downloading {name}\xE2\x80\xA6", {{"name", label}}));
                    app.api(
                        "POST", "/api/deps/" + dep["id"].str() + "/install", JsonValue::object(),
                        [&app, rest, intro, done, label](const Reply& r) {
                            if (!r.ok()) {
                                app.toast(t_msg(r.error()), App::Toast::bad);
                                done(false);
                                return;
                            }
                            app.toast(t("{name} downloaded", {{"name", label}}), App::Toast::ok);
                            app.load_info();
                            run(app, rest, intro, done);
                        },
                        0, 600000);
                },
                [done] { done(false); });
        }
    };
    Step::run(*this, needs, intro, std::move(done));
}

// ---------------------------------------------------------------- the folder picker

namespace {

class Picker : public Overlay {
public:
    bool has_entry = false;
    Entry entry;
    std::string kind;  // copy, move, pick
    std::string start;
    std::function<void(const std::string&)> picked;
    JsonValue data;  // the listing
    std::string msg;
    bool err = false;
    std::string name;
    bool over = false;
    bool loaded_once = false;
    Sheet sheet;

    void load(App& app, const std::string& path) {
        const int k = key;
        app.get(url("/api/browse", {{"path", path.empty() ? "/" : path}}), [this, &app, k](const Reply& r) {
            if (!app.overlay_alive(k)) return;
            if (!r.ok()) {
                msg = t_msg(r.error());
                err = true;
                return;
            }
            msg.clear();
            err = false;
            data = r.json;
            fresh = true;
        });
    }
    bool fresh = false;

    std::string cur() const { return data.is_object() ? data["path"].str() : ""; }

    void go(App& app) {
        const std::string c = cur();
        if (c.empty() || c == "/") return;
        app.close(this);
        if (kind == "pick") {
            if (picked) picked(c);
            return;
        }
        std::string n = name;
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (n.empty() || n.find('/') != std::string::npos) {
            app.toast(t("Enter a plain name"), App::Toast::bad);
            return;
        }
        JsonValue body = JsonValue::object();
        body.set("kind", kind);
        body.set("input", entry.path);
        body.set("output", c + "/" + n);
        body.set("overwrite", over);
        const std::string k = kind;
        app.post("/api/jobs", body, app.or_toast([&app, k](const Reply&) {
            app.toast(k == "copy" ? t("Copy queued") : t("Move queued"), App::Toast::ok);
            app.refresh_jobs();
        }));
        if (picked) picked(c);
    }

    void build(Ui& ui, App& app) override {
        if (!loaded_once) {
            loaded_once = true;
            load(app, start);
        }
        const bool pick = kind == "pick";
        const std::string title = pick ? t("Choose a folder")
                                       : t(kind == "copy" ? "Copy {name}" : "Move {name}", {{"name", entry.name}});
        sheet.begin(ui, title, "", Sheet::Size::normal, true);
        const float x = sheet.x(), w = sheet.w();
        float y = sheet.content_top();
        const std::string c = cur();
        // Places by engine (chips).
        if (data.is_object()) {
            std::vector<std::pair<std::string, std::vector<const JsonValue*>>> groups;
            for (const auto& l : app.locations.items()) {
                if (!l["exists"].boolean()) continue;
                const std::string who = l["peer"].str().empty() ? app.here_name() : l["engine"].str();
                auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == who; });
                if (it == groups.end()) {
                    groups.push_back({who, {}});
                    it = groups.end() - 1;
                }
                it->second.push_back(&l);
            }
            const float cs = 0.85f * R, ch = cs * 1.45f + 0.6f * R + 2.0f;
            for (const auto& g : groups) {
                float cx = x;
                if (groups.size() > 1) {
                    draw::text_fit(ui, Face::semibold, g.first, x, draw::baseline(y + ch * 0.5f, 0.78f * R), 0.78f * R,
                                   5.0f * R, muted);
                    cx += std::max(5.0f * R, draw::measure(ui, Face::semibold, g.first, 0.78f * R)) + 0.5f * R;
                }
                for (const JsonValue* l : g.second) {
                    const std::string label = short_place((*l)["peer"].str().empty() ? (*l)["label"].str() : (*l)["place"].str());
                    const float cw = draw::measure(ui, Face::regular, label, cs) + 1.6f * R + 2.0f;
                    if (cx + cw > x + w) {
                        cx = x + (groups.size() > 1 ? 5.5f * R : 0.0f);
                        y += ch + 0.4f * R;
                    }
                    const Rect r{cx, y, cw, ch};
                    const bool on = c == (*l)["path"].str();
                    ui.list->bordered_rect(r, ch * 0.5f, field, 1.0f, on ? accent : line2);
                    draw::text(ui, Face::regular, label, r.cx(), draw::baseline(r.cy(), cs), cs, on ? accent : style::text,
                               Align::center);
                    if (ui.item(Id("chip") / (*l)["path"].str(), r, kPill)) load(app, (*l)["path"].str());
                    cx += cw + 0.35f * R;
                }
                y += ch + 0.55f * R;
            }
            y += 0.1f * R;
        }
        // Crumbs.
        {
            const float cs = 1.0f * R;
            std::vector<std::pair<std::string, std::string>> parts = {{"/", t("Drives")}};
            if (!c.empty() && c != "/") {
                const JsonValue* r = app.root_of(c);
                const JsonValue* f = r ? nullptr : app.server_of(c);
                const std::string root = r ? (*r)["root"].str() : f ? (*f)["path"].str() : c;
                parts.push_back({root, app.drive_name(root)});
                std::string acc = root, rest = c.substr(std::min(root.size(), c.size()));
                std::size_t at = 0;
                while (at < rest.size()) {
                    std::size_t slash = rest.find('/', at);
                    if (slash == std::string::npos) slash = rest.size();
                    if (slash > at) {
                        acc += "/" + rest.substr(at, slash - at);
                        parts.push_back({acc, rest.substr(at, slash - at)});
                    }
                    at = slash + 1;
                }
            }
            float cx = x;
            const float cy = y + cs * 0.725f + 0.1f * R;
            std::size_t first = parts.size() > 4 ? parts.size() - 4 : 0;
            for (std::size_t i = first; i < parts.size(); ++i) {
                if (i > first || first > 0) {
                    draw::chevron(ui, cx + 0.1f * R + cs * 0.2f, cy, cs, 1, dim);
                    cx += 0.2f * R + cs * 0.4f;
                }
                const float tw = std::min(14.0f * R, draw::measure(ui, Face::semibold, parts[i].second, cs));
                const Rect r{cx, cy - cs * 0.725f - 0.1f * R, tw + 0.7f * R, cs * 1.45f + 0.2f * R};
                draw::text_fit(ui, Face::semibold, parts[i].second, cx + 0.35f * R, draw::baseline(cy, cs), cs, tw,
                               i + 1 == parts.size() ? style::text : muted);
                if (ui.item(Id("pcrumb") / parts[i].first, r, 0, 0.45f * R)) load(app, parts[i].first);
                cx += r.w;
                if (cx > x + w) break;
            }
            y += cs * 1.45f + 0.2f * R + 0.5f * R;
        }
        // The folders (.picker).
        {
            const float list_h = 18.0f * R;
            const Rect box{x, y, w, list_h};
            ui.list->bordered_rect(box, 0.8f * R, Color{0, 0, 0, 0}, 1.0f, line);
            std::vector<JsonValue> folders;
            for (const auto& e : data["entries"].items())
                if (e["kind"].str() == "folder") folders.push_back(e);
            ui.begin_zone("picker", true, false);
            if (fresh && !folders.empty()) {
                ui.set_focus(Id("pick") / folders.front()["path"].str());
                ui.scroll_to_top("picker");
                fresh = false;
            } else if (fresh) {
                fresh = false;
            }
            const float off = ui.begin_scroll("picker", box.inset(1.0f));
            float ry = box.y + 0.3f * R - off;
            if (data.is_object() && folders.empty())
                draw::text(ui, Face::regular, c == "/" ? t("No drives.") : t("No folders here."), box.cx(),
                           draw::baseline(ry + 1.2f * R + 0.725f * R, 1.0f * R), 1.0f * R, muted, Align::center);
            for (const auto& f : folders) {
                const std::string fp = f["path"].str();
                const float rh = 0.4f * R * 2.0f + 2.4f * R;
                ry += 0.15f * R;
                const Rect r{box.x + 0.3f * R, ry, box.w - 0.6f * R, rh};
                const Id id = Id("pick") / fp;
                if (ui.visible(r)) {
                    if (ui.focused(id)) ui.list->rounded_rect(r, 0.7f * R, accent.with_alpha(0.12f));
                    const Rect ic{r.x + 0.6f * R, r.y + 0.4f * R, 2.4f * R, 2.4f * R};
                    Color fill, ink;
                    draw::kind_colors("folder", &fill, &ink);
                    ui.list->rounded_rect(ic, 0.5f * R, fill);
                    draw::icon(ui, c == "/" ? draw::Icon::drive : draw::Icon::folder, ic.inset(ic.w * 0.225f), ink);
                    draw::text_fit(ui, Face::regular, c == "/" ? app.drive_name(fp) : f["name"].str(),
                                   ic.x + ic.w + 1.0f * R, draw::baseline(r.cy(), 1.05f * R), 1.05f * R,
                                   r.w - ic.w - 2.6f * R, style::text);
                }
                if (ui.item(id, r, 0, 0.7f * R)) load(app, fp);
                ry += rh + 0.15f * R;
            }
            ui.end_scroll(ry + off - box.y + 0.3f * R);
            ui.end_zone();
            y += list_h;
        }
        if (!pick) {
            y -= 0.4f * R;
            field_row(ui, app, "pick:name", x, &y, w, t("Name"), &name, "");
            y += 0.5f * R;
            y += switch_row(ui, "pick:over", x, y, w, t("Replace if it already exists"), "", &over);
        } else {
            y += 0.6f * R;
        }
        // Where it goes and how much room there is.
        std::string space;
        if (data.is_object()) {
            const JsonValue* r = c.empty() || c == "/" ? nullptr : app.root_of(c);
            const JsonValue* srv = r ? nullptr : app.server_of(c);
            if (srv)
                space = App::on_server_text(*srv) + "  \xC2\xB7  " + c;
            else if (r)
                space = t("{size} free on {drive}", {{"size", bytes((*r)["free"].number())},
                                                     {"drive", app.drive_name((*r)["root"].str())}}) +
                        (has_entry && entry.has_size() ? " \xC2\xB7 " + t("needs {size}", {{"size", bytes(entry.size)}}) : "") +
                        "  \xC2\xB7  " + app.shown_path(c);
            else
                space = t("Pick a drive and folder.");
        }
        y += draw::paragraph(ui, Face::regular, space, x, y, w, 0.88f * R, 0.88f * 1.45f * R, muted);
        sheet.end_body(ui, y);
        const std::string go_label = pick ? t("Use this folder") : kind == "copy" ? t("Copy here") : t("Move here");
        if (sheet.button(ui, "pick:go", go_label, draw::Button::primary, !data.is_object() || c == "/")) go(app);
        if (sheet.button(ui, "pick:cancel", t("Cancel"), draw::Button::normal)) app.close(this);
        sheet.message(ui, msg, err);
        sheet.end(ui);
    }
    std::vector<Hint> hints(App&) override {
        const std::string go_label = kind == "pick" ? t("Use this folder") : kind == "copy" ? t("Copy here") : t("Move here");
        return {{Glyph::cross, t("Open folder")}, {Glyph::circle, t("Cancel")}, {Glyph::square, go_label}};
    }
    void square(App& app) override { go(app); }
};

}  // namespace

void App::open_dest(const Entry* e, const std::string& kind, const std::string& start,
                    std::function<void(const std::string&)> picked) {
    auto p = std::make_unique<Picker>();
    if (e) {
        p->has_entry = true;
        p->entry = *e;
        p->name = e->name;
    }
    p->kind = kind;
    p->start = !start.empty() ? start : e ? dir(e->path) : "/";
    p->picked = std::move(picked);
    open(std::move(p));
}

// ---------------------------------------------------------------- Convert

namespace {

struct Format {
    const char* value;
    const char* label;
    const char* hint;
};
const Format kFormats[] = {
    {"ffpfsc", "FFPFSC", "Compressed image, smallest"},
    {"ffpkg", "FFPKG", "UFS2 image, ShadowMount+'s pick"},
    {"ffpfs", "FFPFS", "PFS image, uncompressed (experimental in ShadowMount+)"},
    {"exfat", "exFAT", "Plain image, fastest to load"},
    {"folder", "Folder", "Loose files"},
    {"fpkg", "FPKG", "Debug package to install, uncompressed"},
};

class Convert : public Overlay {
public:
    Entry entry;
    std::string to = "ffpfsc";
    std::string dest;
    JsonValue inspect;
    std::string problem;
    int level = 1;  // index: 1 / 6 / 9
    int profile = 0;
    int lz4_level = 1;  // index: 1 / 9 / 12
    bool lz4 = false, unpack = false, ampr = true, verify = true, del = false, hide = false, over = false;
    std::string name;
    int where = 0;  // index into on-choices
    JsonValue plan;
    std::string plan_key, requested_key, pending_key;
    std::string plan_err;
    float pending_t = 0.0f, waited = 0.0f;
    bool waiting = false;
    std::string msg;
    bool msg_err = false, busy = false;
    bool started = false;
    Sheet sheet;

    std::vector<std::pair<std::string, std::string>> on_choices(App& app) const {
        std::vector<std::pair<std::string, std::string>> out = {{"", t("This engine")}};
        for (const auto& e : app.peers["engines"].items())
            if (e["paired"].boolean() && e["reachable"].boolean()) out.push_back({e["id"].str(), e["name"].str()});
        return out;
    }

    void start(App& app) {
        started = true;
        const int k = key;
        app.api("GET", url("/api/inspect", {{"path", entry.path}}), JsonValue(),
                [this, &app, k](const Reply& r) {
                    if (!app.overlay_alive(k)) return;
                    if (r.ok())
                        inspect = r.json;
                    else
                        problem = t_msg(r.error());
                },
                1, 600000);
    }

    JsonValue spec(App& app, std::string* key_out) {
        const bool packed = inspect["lz4_packed"].boolean();
        const bool lz4_on = lz4 && !packed, unpack_on = unpack && packed;
        const bool del_on = !entry.shared && del;
        const std::string same = entry.game ? "folder" : entry.kind;
        std::vector<std::string> offered;
        for (const auto& f : kFormats)
            if (f.value != same || lz4_on || unpack_on) offered.push_back(f.value);
        const std::string target = std::find(offered.begin(), offered.end(), to) != offered.end() ? to : offered.front();
        const bool hide_hidden = entry.kind == "fpkg" || is_hidden_name(entry.name) || del_on || target == "fpkg" || entry.shared;
        static const int levels[] = {1, 6, 9};
        static const int lz4_levels[] = {1, 9, 12};
        std::vector<std::string> profiles = {"auto"};
        for (const auto& p : app.info["lz4_profiles"].items()) profiles.push_back(p["id"].str());
        profiles.push_back("scan");
        JsonValue s = JsonValue::object();
        s.set("input", entry.path);
        s.set("to", target);
        s.set("output_dir", dest);
        std::string n = name;
        while (!n.empty() && n.back() == ' ') n.pop_back();
        s.set("output_name", n);
        s.set("level", levels[std::clamp(level, 0, 2)]);
        s.set("lz4", lz4_on);
        s.set("lz4_unpack", unpack_on);
        s.set("lz4_profile", profiles[static_cast<std::size_t>(std::clamp(profile, 0, static_cast<int>(profiles.size()) - 1))]);
        s.set("lz4_level", lz4_levels[std::clamp(lz4_level, 0, 2)]);
        s.set("delete_source", del_on);
        s.set("verify", verify);
        s.set("overwrite", over);
        s.set("ampr_emu", ampr ? "auto" : "off");
        s.set("hide_source", !hide_hidden && hide && !del_on);
        if (key_out) *key_out = s.dump();
        return s;
    }

    void go(App& app) {
        std::string key_now;
        JsonValue s = spec(app, &key_now);
        const bool ready = plan.is_object() && plan_key == key_now && plan_err.empty();
        if (busy || !ready || !problem.empty()) return;
        busy = true;
        const auto choices = on_choices(app);
        const bool where_hidden = choices.size() < 2 || entry.shared;
        const std::string on = where_hidden ? "" : choices[static_cast<std::size_t>(std::clamp(where, 0, static_cast<int>(choices.size()) - 1))].first;
        const std::string engine_name =
            on.empty() ? "" : choices[static_cast<std::size_t>(std::clamp(where, 0, static_cast<int>(choices.size()) - 1))].second;
        const int k = key;
        auto submit = [this, &app, s, on, engine_name, k] {
            if (!app.overlay_alive(k)) return;
            const std::string path = on.empty() ? "/api/jobs" : "/api/peers/" + url_encode(on) + "/jobs";
            app.api("POST", path, s, [this, &app, s, on, engine_name, k](const Reply& r) {
                if (!app.overlay_alive(k)) return;
                busy = false;
                if (!r.ok()) {
                    msg = t_msg(r.error());
                    msg_err = true;
                    return;
                }
                app.prefs.set("dest", s["output_dir"].str());
                app.prefs.set("convertOn", on);
                app.save_prefs();
                app.close(this);
                const std::string out = base(r.json["spec"]["output"].str());
                app.toast(engine_name.empty() ? t("Queued: {name}", {{"name", out}})
                                              : t("Queued on {engine}: {name}", {{"engine", engine_name}, {"name", out}}),
                          App::Toast::ok);
                app.refresh_jobs(true);
            }, 0, 60000);
        };
        // Components are the converting engine's own: only this engine's are fetched from here.
        if (on.empty()) {
            app.get_needs(plan, "This conversion needs {name}, which isn't downloaded yet.", [this, &app, submit, k](bool ok) {
                if (!app.overlay_alive(k)) return;
                if (!ok) {
                    busy = false;
                    msg = t("Not queued: a component is needed.");
                    msg_err = false;
                    return;
                }
                submit();
            });
        } else {
            submit();
        }
    }

    void pick_dest(App& app) {
        std::vector<Choice> items;
        const int k = key;
        auto set = [this, &app, k](const std::string& v) {
            if (!app.overlay_alive(k)) return;
            dest = v;
            if (!v.empty()) app.remember_folder(v);
        };
        if (!entry.shared) {
            Choice c;
            c.label = t("Next to the source");
            c.selected = dest.empty();
            c.act = [set] { set(""); };
            items.push_back(c);
        }
        const std::string here = app.here_name();
        std::vector<std::pair<std::string, std::vector<Choice>>> groups = {{here, {}}};
        auto group_of = [&](const std::string& who) -> std::vector<Choice>& {
            for (auto& g : groups)
                if (g.first == who) return g.second;
            groups.push_back({who, {}});
            return groups.back().second;
        };
        auto on_engine = [&app](const std::string& p) {
            const std::string s = app.shown_path(p);
            const std::size_t i = s.find(": ");
            return starts_with(p, "/peer/") && i != std::string::npos ? s.substr(i + 2) : s;
        };
        for (const auto& l : app.locations.items()) {
            const bool peer = !l["peer"].str().empty();
            Choice c;
            c.label = short_place(peer ? l["place"].str() : l["label"].str());
            c.sub = on_engine(l["path"].str());
            c.selected = dest == l["path"].str();
            const std::string p = l["path"].str();
            c.act = [set, p] { set(p); };
            group_of(peer ? l["engine"].str() : here).push_back(c);
        }
        for (const auto& p : app.recent()) {
            if (!app.root_of(p)) continue;
            bool listed = false;
            for (const auto& l : app.locations.items()) listed = listed || l["path"].str() == p;
            if (listed) continue;
            const JsonValue* pd = app.peer_drive_of(p);
            Choice c;
            c.label = base(p);
            c.sub = on_engine(p);
            c.selected = dest == p;
            c.act = [set, p] { set(p); };
            group_of(pd ? (*pd)["engine"].str() : here).push_back(c);
        }
        int shown = 0;
        for (const auto& g : groups) shown += g.second.empty() ? 0 : 1;
        for (auto& g : groups) {
            if (g.second.empty()) continue;
            if (shown > 1) {
                Choice h;
                h.heading = true;
                h.label = g.first;
                items.push_back(h);
            }
            for (auto& c : g.second) items.push_back(std::move(c));
        }
        Choice more;
        more.label = t("Choose a folder\xE2\x80\xA6");
        const std::string from = dest.empty() ? dir(entry.path) : dest;
        more.act = [&app, set, from] { app.open_dest(nullptr, "pick", from, set); };
        items.push_back(more);
        choose(app, t("Save to"), std::move(items));
    }

    void build(Ui& ui, App& app) override {
        if (!started) start(app);
        // The plan for the current choices, asked 150 ms after they change.
        std::string key_now;
        JsonValue s = spec(app, &key_now);
        if (key_now != pending_key) {
            pending_key = key_now;
            pending_t = 0.0f;
        }
        pending_t += ui.dt;
        if (waiting) waited += ui.dt;
        if (pending_key != requested_key && pending_t > 0.15f) {
            requested_key = pending_key;
            waiting = true;
            waited = 0.0f;
            const int k = key;
            const std::string want = requested_key;
            app.api("POST", "/api/plan", s, [this, &app, k, want](const Reply& r) {
                if (!app.overlay_alive(k)) return;
                if (want != requested_key) return;  // the choices changed meanwhile
                waiting = false;
                waited = 0.0f;
                if (!r.ok()) {
                    plan_err = t_msg(r.error());
                    return;
                }
                plan = r.json;
                plan_key = want;
                plan_err.clear();
                msg.clear();
            }, 1, 600000);
        }
        const bool plan_ready = plan.is_object() && plan_key == key_now && plan_err.empty();
        const bool packed = inspect["lz4_packed"].boolean();
        const std::string target = s["to"].str();
        const bool lz4_on = s["lz4"].boolean(), unpack_on = s["lz4_unpack"].boolean();
        const bool del_on = s["delete_source"].boolean();
        const bool hide_hidden = entry.kind == "fpkg" || is_hidden_name(entry.name) || del_on || target == "fpkg" || entry.shared;
        const bool ampr_hidden = target == "fpkg" || lz4_on;

        sheet.begin(ui, t("Convert"), "", Sheet::Size::wide, true);
        const float x = sheet.x(), w = sheet.w();
        const float gap = 1.75f * R;
        const float lw = (w - gap) * 1.25f / 2.25f, rw = w - gap - lw;
        float y = sheet.content_top();
        // ---- left: format and options ----
        y += section(ui, t("Convert to"), x + 0.1f * R, y, 0.2f);
        {
            const std::string same = entry.game ? "folder" : entry.kind;
            std::vector<const Format*> offered;
            for (const auto& f : kFormats)
                if (f.value != same || lz4_on || unpack_on) offered.push_back(&f);
            const float tile_w = (lw - 1.0f * R) / 3.0f;
            for (std::size_t row = 0; row * 3 < offered.size(); ++row) {
                float th = 0.0f;
                for (std::size_t c = row * 3; c < std::min(offered.size(), row * 3 + 3); ++c)
                    th = std::max(th, 1.0f * R * 1.45f +
                                          draw::paragraph_height(ui, Face::regular, t(offered[c]->hint), tile_w - 1.8f * R,
                                                                 0.8f * R, 0.8f * 1.45f * R));
                const float h = th + 1.4f * R + 2.0f;
                for (std::size_t c = row * 3; c < std::min(offered.size(), row * 3 + 3); ++c) {
                    const Format& f = *offered[c];
                    const Rect r{x + static_cast<float>(c - row * 3) * (tile_w + 0.5f * R), y, tile_w, h};
                    const bool sel = target == f.value;
                    ui.list->bordered_rect(r, 0.75f * R, sel ? accent.with_alpha(0.12f) : field, 1.0f, sel ? accent : line2);
                    const float bw = draw::text(ui, Face::semibold, f.label, r.x + 0.9f * R,
                                                draw::baseline(r.y + 0.7f * R + 0.725f * R, 1.0f * R), 1.0f * R, style::text);
                    if (sel)
                        draw::text(ui, Face::regular, " \xE2\x9C\x93", r.x + 0.9f * R + bw,
                                   draw::baseline(r.y + 0.7f * R + 0.725f * R, 1.0f * R), 1.0f * R, accent);
                    draw::paragraph(ui, Face::regular, t(f.hint), r.x + 0.9f * R, r.y + 0.7f * R + 1.45f * R,
                                    tile_w - 1.8f * R, 0.8f * R, 0.8f * 1.45f * R, muted);
                    if (ui.item(Id("fmt") / f.value, r, sel ? kAutofocus : 0u, 0.75f * R)) {
                        to = f.value;
                        ui.play(hui::audio::Cue::select);
                    }
                }
                y += h + 0.5f * R;
            }
        }
        y += section(ui, t("Options"), x + 0.1f * R, y, 0.6f);
        if (target == "ffpfsc")
            y += choice_row(ui, "cv:level", x, y, lw, t("Compression"), t("Level 9 matches PS5PKGTool byte for byte"),
                            {t("1 \xC2\xB7 fastest"), t("6 \xC2\xB7 balanced"), t("9 \xC2\xB7 smallest")}, &level);
        if (!packed) y += switch_row(ui, "cv:lz4", x, y, lw, t("Pack into LZ4 asset packs"), t("For ampr_emu games"), &lz4);
        if (lz4_on) {
            const float ix = x + 1.25f * R + 0.9f * R + 2.0f, iw = lw - (ix - x);
            const float top = y;
            std::vector<std::string> profiles = {t("Automatic")};
            for (const auto& p : app.info["lz4_profiles"].items()) profiles.push_back(p["label"].str());
            profiles.push_back(t("Scan (untested)"));
            y += choice_row(ui, "cv:profile", ix, y, iw, t("Profile"), t("Automatic uses a console-tested profile when one matches"),
                            profiles, &profile);
            y += choice_row(ui, "cv:lz4level", ix, y, iw, t("LZ4 level"), "",
                            {t("1 \xC2\xB7 fast"), t("9 \xC2\xB7 default"), t("12 \xC2\xB7 smallest")}, &lz4_level);
            const JsonValue& rt = app.info["lz4_runtime"];
            if (rt.is_object() && !rt["present"].boolean())
                y += draw::paragraph(ui, Face::regular,
                                     t("No ampr_emu runtime yet: only games that ship it can be packed.") + " " +
                                         t("Get it in Components"),
                                     ix, y, iw, 0.88f * R, 0.88f * 1.45f * R, warn) + 0.5f * R;
            else if (rt.is_object() && !rt["pinned"].boolean())
                y += draw::paragraph(ui, Face::regular, t("The runtime file is not the pinned ampr_emu 0.4.2.1 build."), ix, y, iw,
                                     0.88f * R, 0.88f * 1.45f * R, warn) + 0.5f * R;
            ui.list->rounded_rect({x + 1.25f * R, top, 2.0f, y - top - 0.5f * R}, 0.0f, line);
        }
        if (packed)
            y += switch_row(ui, "cv:unpack", x, y, lw, t("Unpack LZ4 asset packs"),
                            t("Turns the packs back into ordinary files; ampr_emu's runtime stays in fakelib"), &unpack);
        if (target == "ffpfsc" && (lz4_on || (packed && !unpack_on)))
            y += draw::paragraph(ui, Face::regular,
                                 (packed ? t("This game is already LZ4-packed.") + " " : std::string()) +
                                     t("FFPFSC loads slower than exFAT here: the console undoes zlib, then LZ4, and zlib saves little on LZ4 data."),
                                 x, y, lw, 0.88f * R, 0.88f * 1.45f * R, warn) + 0.5f * R;
        if (!ampr_hidden)
            y += switch_row(ui, "cv:ampr", x, y, lw, t("Add the AMPR emulator when the game needs it"),
                            t("Games that use the console's APR stop at start from an image or folder without it"), &ampr);
        if (!entry.shared)
            y += switch_row(ui, "cv:del", x, y, lw, t("Delete the source when done"),
                            entry.kind == "fpkg" || entry.kind == "ffpkg"
                                ? t("Frees {size} after a successful conversion", {{"size", bytes(entry.size)}})
                                : t("Frees its space after a successful conversion"),
                            &del);
        if (del_on) {
            const float ix = x + 1.25f * R + 0.9f * R + 2.0f;
            const float top = y;
            y += switch_row(ui, "cv:verify", ix, y, lw - (ix - x), t("Check the output first"),
                            t("Compares every file before deleting; recommended"), &verify);
            ui.list->rounded_rect({x + 1.25f * R, top, 2.0f, y - top - 0.5f * R}, 0.0f, line);
        }
        if (!hide_hidden) {
            bool changed = false;
            y += switch_row(ui, "cv:hide", x, y, lw, t("Hide the source from ShadowMount+"),
                            t("Renames it to .name when done, so ShadowMount+ switches to the new image"), &hide, &changed);
            if (changed) {
                app.prefs.set("hideSource", hide);
                app.save_prefs();
            }
        }
        y += section(ui, t("Output"), x + 0.1f * R, y, 0.6f);
        {
            std::string label = t("Next to the source");
            if (!dest.empty()) {
                label = app.shown_path(dest);
                for (const auto& l : app.locations.items())
                    if (l["path"].str() == dest) label = place_label(l["label"].str());
            }
            bool pressed = false;
            y += link_row(ui, "cv:dest", x, y, lw, t("Save to"), "", label, &pressed);
            if (pressed) pick_dest(app);
            y -= 1.1f * R + 0.72f * R * 1.45f + 0.45f * R;  // the field's label is the row above
            field_row(ui, app, "cv:name", x, &y, lw, "", &name, plan.is_object() ? base(plan["output"].str()) : "");
            y += 0.5f * R;
        }
        y += switch_row(ui, "cv:over", x, y, lw, t("Replace the output if it exists"), "", &over);
        {
            const auto choices = on_choices(app);
            if (choices.size() > 1 && !entry.shared) {
                std::vector<std::string> labels;
                for (const auto& c : choices) labels.push_back(c.second);
                y += choice_row(ui, "cv:on", x, y, lw, t("Convert on"),
                                t("The engine that does the work; the source and the output can be on any paired engine"),
                                labels, &where);
            }
        }
        const float left_bottom = y;
        // ---- right: the source and the output ----
        {
            const float sx = x + lw + gap, top = sheet.content_top();
            const float px = sx + 1.1f * R, pw = rw - 2.2f * R;
            float yy = top + 1.0f * R;
            const Meta* m = app.meta(entry.path, entry.mtime, entry.size);
            const Rect ic{px, yy, 3.6f * R, 3.6f * R};
            const std::string kind = entry.kind == "folder" && !entry.game ? "folder" : entry.game ? "game" : entry.kind;
            const std::uint32_t tex = m && m->icon ? app.icon(entry.path, entry.mtime) : 0;
            // The panel goes under: drawn after with the height known? Draw a
            // background first with last frame's height.
            ui.list->bordered_rect({sx, top, rw, side_h}, 0.9f * R, side, 1.0f, line);
            static const std::map<std::string, std::string> labels = {
                {"fpkg", "PKG"}, {"exfat", "EXFAT"}, {"ffpfsc", "FFPFSC"}, {"ffpkg", "FFPKG"}, {"ffpfs", "FFPFS"}};
            const auto lab = labels.find(entry.kind);
            draw::ico(ui, ic, kind, entry.game ? t("GAME") : lab != labels.end() ? lab->second : "", tex);
            const float hx = ic.x + ic.w + 0.9f * R, hw = pw - ic.w - 0.9f * R;
            draw::text_fit(ui, Face::semibold, m && !m->title.empty() ? m->title : entry.name, hx,
                           draw::baseline(ic.cy() - 0.7f * R, 1.05f * R), 1.05f * R, hw, style::text);
            if (m) {
                // The platform pill, then the file name under the title.
                float px = draw::platform_pill(ui, m->platform, hx, ic.cy() + 0.75f * R);
                if (px) px += 0.45f * R;
                if (!m->title.empty())
                    draw::text_fit(ui, Face::mono, entry.name, hx + px, draw::baseline(ic.cy() + 0.75f * R, 0.85f * R),
                                   0.85f * R, hw - px, muted);
            }
            yy += 3.6f * R + 0.6f * R;
            std::vector<Fact> rows;
            if (!problem.empty())
                rows.push_back({t("Problem"), problem, "err"});
            else if (!inspect.is_object())
                rows.push_back({t("Reading"), "\xE2\x80\xA6", ""});
            else {
                rows.push_back({t("Format"), inspect["format"].str() + (packed ? " \xC2\xB7 " + t("LZ4-packed") : ""), ""});
                rows.push_back({t("Title ID"), inspect["title_id"].str().empty() ? "\xE2\x80\x94" : inspect["title_id"].str(), ""});
                rows.push_back({t("Contents"), plural(inspect["files"].integer(), "{n} file", "{n} files") + " \xC2\xB7 " +
                                                   bytes(inspect["bytes"].number()), ""});
                rows.push_back({t("LZ4 profile"), inspect["lz4_profile"].str() == "scan" ? t("no tested profile")
                                                                                        : inspect["lz4_profile"].str(), ""});
            }
            yy += facts(ui, px, yy, pw, rows);
            yy += 0.9f * R;
            yy += section(ui, t("Output"), px + 0.1f * R, yy, 0.4f) - 0.2f * R;
            if (plan.is_object()) {
                const std::string out = plan["output"].str();
                yy += draw::paragraph(ui, Face::mono, app.shown_path(out), px, yy, pw, 0.85f * R, 0.85f * 1.45f * R, style::text);
                const JsonValue* r = app.root_of(out);
                const double need = inspect["bytes"].number();
                std::string room;
                if (r)
                    room = t("{size} free on {drive}", {{"size", bytes((*r)["free"].number())},
                                                        {"drive", app.drive_name((*r)["root"].str())}}) +
                           (need > 0 ? " \xC2\xB7 " + t("the game is {size} unpacked", {{"size", bytes(need)}}) : "");
                else if (const JsonValue* srv = app.server_of(out))
                    room = App::on_server_text(*srv);
                yy += draw::paragraph(ui, Face::regular, room, px, yy, pw, 0.88f * R, 0.88f * 1.45f * R, muted);
                if (!ampr_hidden && plan["ampr_needed"].boolean())
                    yy += 0.5f * R + draw::paragraph(ui, Face::regular,
                                                     ampr ? t("This game uses the console's APR and ships no emulator: ampr_emu goes into its fakelib.")
                                                          : t("This game uses the console's APR and ships no emulator: without it, it will likely stop at start."),
                                                     px, yy + 0.5f * R, pw, 0.88f * R, 0.88f * 1.45f * R, warn);
                std::string needs;
                for (const auto& d : plan["needs"].items())
                    needs += (needs.empty() ? "" : ", ") + t(d["label"].str()) + " (" + bytes(d["bytes"].number()) + ")";
                if (!needs.empty())
                    yy += 0.5f * R + draw::paragraph(ui, Face::regular,
                                                     t("Needs {list}. You'll be asked to download it when you add this to the queue.",
                                                       {{"list", needs}}),
                                                     px, yy + 0.5f * R, pw, 0.88f * R, 0.88f * 1.45f * R, warn);
                std::string warns;
                for (const auto& wv : plan["warnings"].items()) warns += (warns.empty() ? "" : " ") + t_msg(wv.str());
                if (!warns.empty())
                    yy += 0.5f * R + draw::paragraph(ui, Face::regular, warns, px, yy + 0.5f * R, pw, 0.88f * R,
                                                     0.88f * 1.45f * R, warn);
            }
            side_h = yy + 1.0f * R - top;
            y = std::max(left_bottom, top + side_h);
        }
        sheet.end_body(ui, y);
        if (sheet.button(ui, "cv:go", t("Add to queue"), draw::Button::primary, busy || !plan_ready || !problem.empty()))
            go(app);
        if (sheet.button(ui, "cv:cancel", t("Cancel"), draw::Button::normal)) app.close(this);
        std::string foot = plan_err;
        bool foot_err = !plan_err.empty() || msg_err;
        if (foot.empty()) foot = msg;
        if (foot.empty() && waiting && !plan_ready && waited >= 2.0f) {
            const std::string n = std::to_string(static_cast<int>(waited));
            if (waited < 60.0f)
                foot = t("Reading the source\xE2\x80\xA6 ({n} s)", {{"n", n}});
            else {
                foot = t("The engine hasn't answered for {n} s. It may be busy with the disk; this keeps waiting, or cancel and try again.",
                         {{"n", n}});
                foot_err = true;
            }
        }
        sheet.message(ui, foot, foot_err);
        sheet.end(ui);
    }
    float side_h = 10.0f * R;

    std::vector<Hint> hints(App&) override {
        return {{Glyph::dpad, t("Move, change")}, {Glyph::cross, t("Choose")}, {Glyph::circle, t("Cancel")},
                {Glyph::square, t("Add to queue")}};
    }
    void square(App& app) override { go(app); }
};

}  // namespace

void App::open_convert(const Entry& e) {
    auto c = std::make_unique<Convert>();
    c->entry = e;
    c->to = e.kind == "ffpfsc" ? "ffpkg" : "ffpfsc";
    std::string d = prefs["dest"].str();
    // A shared file has no folder on the console: it goes to a place here.
    if (e.shared && d.empty() && !locations.items().empty()) d = locations.items()[0]["path"].str();
    if (!d.empty() && !root_of(d) && !server_of(d)) d.clear();
    c->dest = d;
    c->hide = prefs["hideSource"].boolean();
    const std::string on = prefs["convertOn"].str();
    const auto choices = c->on_choices(*this);
    for (std::size_t i = 0; i < choices.size(); ++i)
        if (choices[i].first == on) c->where = static_cast<int>(i);
    open(std::move(c));
}

// ---------------------------------------------------------------- jobs

void App::job_actions(const JsonValue& j) {
    const JsonValue r = j["result"];
    const std::string state = j["state"].str();
    std::vector<Choice> items;
    auto add = [&](std::string label, std::string sub, bool danger, std::function<void()> act) {
        Choice c;
        c.label = std::move(label);
        c.sub = std::move(sub);
        c.danger = danger;
        c.act = std::move(act);
        items.push_back(std::move(c));
    };
    // Its place among the waiting jobs of the same engine's queue.
    auto engine_of = [](const JsonValue& x) { return x["engine"].is_object() ? x["engine"]["id"].str() : ""; };
    std::vector<const JsonValue*> waiting;
    for (const auto& x : jobs)
        if ((x["state"].str() == "queued" || x["state"].str() == "paused") && engine_of(x) == engine_of(j))
            waiting.push_back(&x);
    int pos = -1;
    for (std::size_t i = 0; i < waiting.size(); ++i)
        if (job_key(*waiting[i]) == job_key(j)) pos = static_cast<int>(i);
    const std::string path = job_path(j);
    auto act = [this](const std::string& method, const std::string& p, JsonValue body) {
        api(method, p, body, [this](const Reply& rep) {
            if (!rep.ok()) toast(t_msg(rep.error()), Toast::bad);
            refresh_jobs();
        });
    };
    const std::string cp = job_cp_text(j);
    if (state == "running")
        add(t("Pause"),
            j["keeps_progress"].boolean() ? t("Keeps its progress; other jobs run meanwhile")
                                          : t("This kind of job can't keep its progress: it starts over when resumed"),
            false, [act, path] { act("POST", path + "/pause", JsonValue::object()); });
    if (state == "paused")
        add(t("Resume"), cp.empty() ? t("Starts from the beginning") : t("Continues from {p}", {{"p", cp}}), false,
            [act, path] { act("POST", path + "/resume", JsonValue::object()); });
    if (state == "queued")
        add(t("Hold"), t("Keep it waiting until you resume it"), false,
            [act, path] { act("POST", path + "/pause", JsonValue::object()); });
    std::string title, to;
    if (pos > 0) {
        std::string other, ignore;
        job_text(*this, *waiting[static_cast<std::size_t>(pos - 1)], &other, &ignore);
        add(t("Move up"), t("Run it before {name}", {{"name", other}}), false, [act, path] {
            JsonValue b = JsonValue::object();
            b.set("direction", -1);
            act("POST", path + "/move", b);
        });
    }
    if (pos >= 0 && pos < static_cast<int>(waiting.size()) - 1) {
        std::string other, ignore;
        job_text(*this, *waiting[static_cast<std::size_t>(pos + 1)], &other, &ignore);
        add(t("Move down"), t("Run it after {name}", {{"name", other}}), false, [act, path] {
            JsonValue b = JsonValue::object();
            b.set("direction", 1);
            act("POST", path + "/move", b);
        });
    }
    if (is_active(j))
        add(t("Cancel"), state == "running" || j["checkpoint"].is_object() ? t("Its partial output is deleted") : "", true,
            [act, path] { act("POST", path + "/cancel", JsonValue::object()); });
    if (state == "failed" || state == "cancelled" || state == "interrupted")
        add(t("Retry"), cp.empty() ? "" : t("Continues from {p}", {{"p", cp}}), false,
            [act, path] { act("POST", path + "/retry", JsonValue::object()); });
    if (state == "done" && !r["output"].str().empty()) {
        const std::string out = r["output"].str();
        add(t("Show in Files"), "", false, [this, out] {
            show_tab(Tab::files);
            browse(dir(out), out);
        });
    }
    if (!r["smp_source"].str().empty()) {
        const std::string p = r["smp_source"].str();
        add(t("Hide {name} from ShadowMount+", {{"name", base(p)}}), t("So ShadowMount+ switches to the new image"), false,
            [this, p] { toggle_hidden(p); });
    }
    if (!r["source_hidden"].str().empty()) {
        const std::string p = r["source_hidden"].str();
        add(t("Show the source to ShadowMount+ again"), t("Renames it back to {name}", {{"name", base(p).substr(1)}}), false,
            [this, p] { toggle_hidden(p); });
    }
    if (!is_active(j))
        add(t("Remove from history"), cp.empty() ? "" : t("Also deletes its partial output"), false,
            [act, path] { act("DELETE", path, JsonValue()); });
    if (items.empty()) return;
    job_text(*this, j, &title, &to);
    choose(*this, title, std::move(items), to);
}

void App::clear_history() {
    std::vector<std::string> theirs;
    for (const auto& j : jobs)
        if (j["engine"].is_object() && !is_active(j)) theirs.push_back(job_path(j));
    post("/api/jobs/clear", JsonValue::object(), [this, theirs](const Reply& r) {
        if (!r.ok()) toast(t_msg(r.error()), Toast::bad);
        for (const auto& p : theirs)
            api("DELETE", p, JsonValue(), [this](const Reply& rr) {
                if (!rr.ok()) toast(t_msg(rr.error()), Toast::bad);
            });
        refresh_jobs(true);
    });
}

}  // namespace alembic::app
