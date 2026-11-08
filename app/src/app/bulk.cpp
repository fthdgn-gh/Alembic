// SPDX-License-Identifier: GPL-3.0-or-later
// Selecting several entries in Files and what can be done to them at once
// (state/selection.ts, pages/files/bulk.tsx). Each action takes the entries
// it applies to and says so when that is not all of them.
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>
#include <map>
#include <memory>

namespace alembic::app {

using namespace style;
using draw::Face;

namespace {

bool is_hidden_name(const std::string& name) { return name.size() > 1 && name[0] == '.'; }

// The names, for a dialog: the first few, then how many more.
std::string names_of(const std::vector<Entry>& list, std::size_t max = 6) {
    std::string out;
    for (std::size_t i = 0; i < list.size() && i < max; ++i) out += (i ? "\n" : "") + list[i].name;
    if (list.size() > max) out += "\n" + t("and {n} more", {{"n", std::to_string(list.size() - max)}});
    return out;
}

std::string failures_text(const std::vector<std::string>& failed) {
    return failed.size() > 1 ? t("{first} (and {n} more)", {{"first", failed[0]}, {"n", std::to_string(failed.size() - 1)}})
                             : failed[0];
}

// Sends one request per entry (all at once) and calls `done` with how many
// worked and what failed, once every answer is in.
struct Tally {
    std::size_t left = 0, ok = 0;
    std::vector<std::string> failed;
};
void each(App&, const std::vector<Entry>& list, std::function<void(const Entry&, App::Done)> send,
          std::function<void(std::size_t ok, const std::vector<std::string>& failed)> done) {
    auto tally = std::make_shared<Tally>();
    tally->left = list.size();
    if (list.empty()) return done(0, {});
    for (const auto& e : list) {
        const std::string name = e.name;
        send(e, [tally, name, done](const Reply& r) {
            if (r.ok())
                ++tally->ok;
            else
                tally->failed.push_back(name + ": " + t_msg(r.error()));
            if (--tally->left == 0) done(tally->ok, tally->failed);
        });
    }
}

// ---- Rename with a format ----

class RenameMany : public Overlay {
public:
    std::vector<Entry> list;
    std::vector<JsonValue> formats;
    int which = 0;
    bool shorten = true;
    std::map<std::string, JsonValue> applied;  // by path, for asked_
    int asked_ = -1;
    bool loading = false, busy = false;
    std::string msg;
    bool err = false;
    Sheet sheet;

    struct Row {
        const Entry* e;
        std::string name, why, note;
    };
    std::vector<Row> rows(App& app) const {
        std::vector<Row> out;
        for (const auto& e : list) {
            Row r{&e, "", "", ""};
            auto it = applied.find(e.path);
            if (it != applied.end()) {
                const JsonValue& a = it->second;
                const bool fit = shorten && !a["fitted"].str().empty();
                r.name = fit ? a["fitted"].str() : a["name"].str();
                r.note = fit ? t("Shortened to fit ShadowMount+") : a["warning"].str().empty() ? "" : t_msg(a["warning"].str());
            }
            out.push_back(r);
        }
        if (asked_ != which || loading) return out;
        for (auto& r : out) {
            int same = 0;
            for (const auto& x : out) same += x.name == r.name ? 1 : 0;
            bool taken = false;
            for (const auto& e : app.entries)
                if (e.name == r.name && std::none_of(list.begin(), list.end(), [&](const Entry& x) { return x.path == e.path; }))
                    taken = true;
            if (r.name.empty())
                r.why = t("This format has no value for it");
            else if (r.name == r.e->name)
                r.why = t("Already named so");
            else if (taken || same > 1)
                r.why = t("Another file would have this name");
        }
        return out;
    }

    void ask_names(App& app) {
        asked_ = which;
        loading = true;
        applied.clear();
        const JsonValue& f = formats[static_cast<std::size_t>(which)];
        JsonValue body = JsonValue::object();
        JsonValue paths = JsonValue::array();
        for (const auto& e : list) paths.push(e.path);
        body.set("paths", paths);
        body.set("pattern", f["pattern"].str());
        body.set("spaces", f["spaces"].str("keep"));
        body.set("letters", f["letters"].str("keep"));
        const int k = key, want = which;
        app.post("/api/names/apply", body, [this, &app, k, want](const Reply& r) {
            if (!app.overlay_alive(k) || want != which) return;
            loading = false;
            if (!r.ok()) {
                msg = t_msg(r.error());
                err = true;
                return;
            }
            for (const auto& a : r.json["results"].items()) applied[a["path"].str()] = a;
        });
    }

    void run(App& app) {
        if (busy || loading) return;
        std::vector<std::pair<std::string, std::string>> todo;
        for (const auto& r : rows(app))
            if (r.why.empty() && !r.name.empty()) todo.push_back({r.e->path, r.name});
        if (todo.empty()) return;
        busy = true;
        auto tally = std::make_shared<Tally>();
        tally->left = todo.size();
        const int k = key;
        // One at a time: a later name may be an earlier one's old name.
        auto step = std::make_shared<std::function<void(std::size_t)>>();
        *step = [this, &app, k, todo, tally, step](std::size_t i) {
            if (i == todo.size()) {
                if (tally->ok) app.toast(plural(static_cast<long long>(tally->ok), "Renamed {n} item", "Renamed {n} items"), App::Toast::ok);
                if (!tally->failed.empty()) app.toast(failures_text(tally->failed), App::Toast::bad);
                if (app.overlay_alive(k)) app.close(this);
                if (tally->ok) {
                    app.stop_selecting();
                    app.refresh_listing();
                }
                return;
            }
            JsonValue body = JsonValue::object();
            body.set("path", todo[i].first);
            body.set("name", todo[i].second);
            const std::string old = base(todo[i].first);
            app.post("/api/fs/rename", body, [tally, step, i, old](const Reply& r) {
                if (r.ok())
                    ++tally->ok;
                else
                    tally->failed.push_back(old + ": " + t_msg(r.error()));
                (*step)(i + 1);
            });
        };
        (*step)(0);
    }

    void build(Ui& ui, App& app) override {
        if (asked_ != which) ask_names(app);
        const std::vector<Row> all = rows(app);
        std::size_t ready = 0;
        for (const auto& r : all) ready += !loading && r.why.empty() && !r.name.empty() ? 1 : 0;
        sheet.begin(ui, plural(static_cast<long long>(list.size()), "Rename {n} game", "Rename {n} games"), "", Sheet::Size::normal, true);
        const float x = sheet.x(), w = sheet.w();
        float y = sheet.content_top();
        std::vector<std::string> labels;
        for (const auto& f : formats) labels.push_back(f["pattern"].str());
        y += choice_row(ui, "rn:format", x, y, w, t("Format"), "", labels, &which);
        y += switch_row(ui, "rn:shorten", x, y, w, t("Shorten names too long for ShadowMount+"), t("Cuts the title until the name fits"),
                        &shorten);
        y += 0.3f * R;
        for (const auto& r : all) {
            draw::hline(ui, x, y, w, line);
            y += 0.4f * R;
            draw::text_fit(ui, Face::mono, r.e->name, x, draw::baseline(y + 0.45f * R, 0.82f * R), 0.82f * R, w, muted);
            y += 0.82f * R * 1.45f;
            const std::string what = loading ? "\xE2\x80\xA6" : !r.why.empty() ? r.why : "\xE2\x86\x92 " + r.name;
            y += draw::paragraph(ui, Face::regular, what, x, y, w, 0.95f * R, 0.95f * 1.45f * R, r.why.empty() ? style::text : muted);
            if (!loading && r.why.empty() && !r.note.empty())
                y += draw::paragraph(ui, Face::regular, r.note, x, y, w, 0.8f * R, 0.8f * 1.45f * R, warn);
            y += 0.35f * R;
        }
        sheet.end_body(ui, y);
        const std::string go = ready ? plural(static_cast<long long>(ready), "Rename {n}", "Rename {n}") : t("Rename");
        if (sheet.button(ui, "rn:go", go, draw::Button::primary, busy || !ready)) run(app);
        if (sheet.button(ui, "rn:cancel", t("Cancel"), draw::Button::normal)) app.close(this);
        sheet.message(ui, msg, err);
        sheet.end(ui);
    }
    std::vector<Hint> hints(App&) override {
        return {{Glyph::cross, t("Choose")}, {Glyph::circle, t("Cancel")}, {Glyph::square, t("Rename")}};
    }
    void square(App& app) override { run(app); }
};

}  // namespace

// ---------------------------------------------------------------- selecting

bool App::is_selected(const std::string& path) const {
    return std::find(selected.begin(), selected.end(), path) != selected.end();
}
void App::toggle_selected(const std::string& path) {
    auto it = std::find(selected.begin(), selected.end(), path);
    if (it != selected.end())
        selected.erase(it);
    else
        selected.push_back(path);
}
void App::start_selecting(const Entry& e) {
    selecting = true;
    selected = {e.path};
}
void App::stop_selecting() {
    selecting = false;
    selected.clear();
}
std::vector<Entry> App::selected_entries() const {
    std::vector<Entry> out;
    for (const auto& e : rows())
        if (is_selected(e.path)) out.push_back(e);
    return out;
}

// ---------------------------------------------------------------- the actions

void App::open_bulk_actions() {
    const std::vector<Entry> all = selected_entries();
    const std::size_t n = all.size();
    std::vector<Choice> items;
    auto add = [&](const std::string& label, const std::string& sub, bool danger, std::function<void()> act) {
        Choice c;
        c.label = label;
        c.sub = sub;
        c.danger = danger;
        c.act = std::move(act);
        items.push_back(std::move(c));
    };
    // "3 of 5": an action that applies to only some of the ticked entries.
    auto some = [n](std::size_t k, const std::string& sub) {
        if (k >= n) return sub;
        const std::string part = t("{n} of {total} selected", {{"n", std::to_string(k)}, {"total", std::to_string(n)}});
        return sub.empty() ? part : part + " \xC2\xB7 " + sub;
    };
    std::vector<Entry> games, images, packages, visible, hidden, local;
    for (const auto& e : all) {
        if (!e.shared) local.push_back(e);
        if (!is_convertible(e) || e.shared) continue;
        games.push_back(e);
        if (e.kind == "fpkg") {
            packages.push_back(e);
            continue;
        }
        images.push_back(e);
        (is_hidden_name(e.name) ? hidden : visible).push_back(e);
    }
    // In Images: the same, less Copy, Move, renaming and plain Hide, plus the
    // update of the AMPR emulator of the ticked ones with a newer build.
    const bool lib = tab == Tab::images;
    auto done = [this, lib] {
        stop_selecting();
        if (lib) refresh_images();
        else refresh_listing();
    };
    if (!games.empty())
        add(t("Convert\xE2\x80\xA6"), some(games.size(), t("To FFPFSC, FFPKG, exFAT or a folder")), false,
            [this, games] { open_convert(games); });
    if (!images.empty() && is_ps5())
        add(t("Install\xE2\x80\xA6"), some(images.size(), t("Converts them to packages, installs them, then deletes the packages")), false,
            [this, images] { open_convert(images, true); });
    if (!packages.empty())
        add(t("Install"), some(packages.size(), t("Hand them to the console's installer")), false, [this, packages, done] {
            if (!is_ps5()) return toast(t("Installing needs the engine running on a PS5"), Toast::bad);
            ask(*this, plural(static_cast<long long>(packages.size()), "Install {n} package?", "Install {n} packages?"),
                text_body("@" + names_of(packages) + "\n\n~" +
                          t("The console installs them in the background, one after another; follow them in Downloads. Keep the "
                            "files until the installs have finished.")),
                t("Install"), false, [this, packages, done] {
                    each(*this, packages,
                         [this](const Entry& e, Done d) {
                             JsonValue body = JsonValue::object();
                             body.set("kind", "install");
                             body.set("input", e.path);
                             post("/api/jobs", body, d);
                         },
                         [this, done](std::size_t ok, const std::vector<std::string>& failed) {
                             if (ok) {
                                 toast(plural(static_cast<long long>(ok), "{n} install queued", "{n} installs queued"), Toast::ok);
                                 refresh_jobs();
                                 done();
                             }
                             if (!failed.empty()) toast(failures_text(failed), Toast::bad);
                         });
                });
        });
    if (lib) {
        std::vector<JsonValue> updatable;
        for (const auto& e : all)
            if (const JsonValue* item = images_->item(e.path); item && images_->updatable(*item)) updatable.push_back(*item);
        if (!updatable.empty())
            add(t("Update the AMPR emulator"), some(updatable.size(), t("The newest build of each one's emulator")), false,
                [this, updatable, done] { images_->update_ampr(*this, updatable, done); });
    }
    if (!images.empty())
        add(t("Add the AMPR emulator\xE2\x80\xA6"), some(images.size(), t("For games that stop at start from an image or folder")), false,
            [this, images, done] {
                // Plans first: which games still need it, and what must be downloaded.
                auto plans = std::make_shared<std::map<std::string, JsonValue>>();
                toast(t("Checking {n} games\xE2\x80\xA6", {{"n", std::to_string(images.size())}}));
                each(*this, images,
                     [this, plans](const Entry& e, Done d) {
                         JsonValue body = JsonValue::object();
                         body.set("kind", "ampr_emu");
                         body.set("input", e.path);
                         const std::string path = e.path;
                         api("POST", "/api/plan", body, [plans, path, d](const Reply& r) {
                             if (r.ok()) (*plans)[path] = r.json;
                             d(r);
                         }, 1, 120000);
                     },
                     [this, images, plans, done](std::size_t, const std::vector<std::string>& failed) {
                         std::vector<Entry> todo;
                         JsonValue needs = JsonValue::array();
                         std::vector<std::string> ids;
                         for (const auto& e : images) {
                             auto it = plans->find(e.path);
                             if (it == plans->end() || it->second["ships_emulator"].boolean()) continue;
                             todo.push_back(e);
                             for (const auto& dep : it->second["needs"].items())
                                 if (std::find(ids.begin(), ids.end(), dep["id"].str()) == ids.end()) {
                                     ids.push_back(dep["id"].str());
                                     needs.push(dep);
                                 }
                         }
                         if (!failed.empty()) toast(failures_text(failed), Toast::bad);
                         const std::size_t have = plans->size() - todo.size();
                         if (todo.empty()) {
                             if (have) toast(t("These games already have the AMPR emulator"), Toast::ok);
                             return;
                         }
                         std::string body = "@" + names_of(todo) + "\n\n~" +
                                            t("Game folders get it in their fakelib; images are not changed: ShadowMount+ gets a "
                                              "backport folder for each.");
                         if (have)
                             body += "\n\n~" + plural(static_cast<long long>(have), "{n} game already has it.", "{n} games already have it.");
                         JsonValue plan = JsonValue::object();
                         plan.set("needs", needs);
                         ask(*this, plural(static_cast<long long>(todo.size()), "Add the AMPR emulator to {n} game?", "Add the AMPR emulator to {n} games?"),
                             text_body(body), t("Add"), false, [this, todo, plan, done] {
                                 get_needs(plan, "This needs {name}, which isn't downloaded yet.", [this, todo, done](bool ok) {
                                     if (!ok) return;
                                     each(*this, todo,
                                          [this](const Entry& e, Done d) {
                                              JsonValue job = JsonValue::object();
                                              job.set("kind", "ampr_emu");
                                              job.set("input", e.path);
                                              post("/api/jobs", job, d);
                                          },
                                          [this, done](std::size_t ok, const std::vector<std::string>& failed) {
                                              if (ok) {
                                                  toast(plural(static_cast<long long>(ok), "{n} job queued", "{n} jobs queued"), Toast::ok);
                                                  refresh_jobs();
                                                  done();
                                              }
                                              if (!failed.empty()) toast(failures_text(failed), Toast::bad);
                                          });
                                 });
                             });
                     });
            });
    // ShadowMount+ skips names that start with ".": one rename each, one rescan.
    // `smp`: images ShadowMount+ uses (it rescans and the toast says so).
    auto hide_many = [this, done](const std::vector<Entry>& list, bool hide, bool smp = true) {
        each(*this, list,
             [this, hide](const Entry& e, Done d) {
                 JsonValue body = JsonValue::object();
                 body.set("path", e.path);
                 body.set("name", hide ? "." + e.name : e.name.substr(1));
                 post("/api/fs/rename", body, d);
             },
             [this, hide, smp, done](std::size_t ok, const std::vector<std::string>& failed) {
                 if (!failed.empty()) toast(failures_text(failed), Toast::bad);
                 if (!ok) return;
                 if (!smp) {
                     const long long k = static_cast<long long>(ok);
                     toast(hide ? plural(k, "{n} hidden", "{n} hidden") : plural(k, "{n} unhidden", "{n} unhidden"), Toast::ok);
                     done();
                     return;
                 }
                 post("/api/smp/scan", JsonValue::object(), [this, hide, ok, done](const Reply& r) {
                     const long long k = static_cast<long long>(ok);
                     toast((hide ? plural(k, "{n} hidden from ShadowMount+", "{n} hidden from ShadowMount+")
                                 : plural(k, "{n} visible to ShadowMount+ again", "{n} visible to ShadowMount+ again")) +
                               (r.ok() ? " \xC2\xB7 " + t("it is rescanning") : std::string()),
                           Toast::ok);
                     done();
                 });
             });
    };
    if (!visible.empty())
        add(t("Hide from ShadowMount+"),
            some(visible.size(), t("Renames them to .name, so ShadowMount+ uses other copies of these games")), false,
            [hide_many, visible] { hide_many(visible, true); });
    if (!hidden.empty())
        add(t("Show to ShadowMount+ again"), some(hidden.size(), t("Renames them back without the leading dot")), false,
            [hide_many, hidden] { hide_many(hidden, false); });
    // Any other file or folder (packages too): hidden by the leading dot alone.
    std::vector<Entry> plain_shown, plain_hidden;
    for (const auto& e : local) {
        if (std::any_of(images.begin(), images.end(), [&](const Entry& i) { return i.path == e.path; })) continue;
        (is_hidden_name(e.name) ? plain_hidden : plain_shown).push_back(e);
    }
    if (!plain_shown.empty() && !lib)
        add(t("Hide"), some(plain_shown.size(), t("Renames them to .name")), false,
            [hide_many, plain_shown] { hide_many(plain_shown, true, false); });
    if (!plain_hidden.empty() && !lib)
        add(t("Unhide"), some(plain_hidden.size(), t("Renames them back without the leading dot")), false,
            [hide_many, plain_hidden] { hide_many(plain_hidden, false, false); });
    if (!games.empty() && !lib)
        add(t("Rename with a format\xE2\x80\xA6"), some(games.size(), t("Names from each game's title, title ID, version and format")),
            false, [this, games] {
                get("/api/names", [this, games](const Reply& r) {
                    if (!r.ok()) return toast(t_msg(r.error()), Toast::bad);
                    std::vector<JsonValue> formats = r.json["formats"].items();
                    if (formats.empty()) return toast(t("No rename formats: add one in Settings > Rename formats"), Toast::bad);
                    auto o = std::make_unique<RenameMany>();
                    o->list = games;
                    o->formats = std::move(formats);
                    open(std::move(o));
                });
            });
    if (n) {
        std::string peer_place;
        for (const auto& l : locations.items())
            if (!l["peer"].str().empty() && peer_place.empty()) peer_place = l["path"].str();
        if (peer_place.empty() && !peer_drives.items().empty()) peer_place = peer_drives.items()[0]["root"].str();
        if (!peer_place.empty() && !local.empty())
            add(t("Send to another engine\xE2\x80\xA6"), some(local.size(), t("Copies as they are, without converting")), false,
                [this, local, peer_place, done] { open_dest_many(local, "copy", peer_place, done); });
        if (!local.empty()) {
            const std::string from = dir(local.front().path);
            if (!lib) {
                add(t("Copy to\xE2\x80\xA6"), some(local.size(), ""), false, [this, local, from, done] { open_dest_many(local, "copy", from, done); });
                add(t("Move to\xE2\x80\xA6"), some(local.size(), ""), false, [this, local, from, done] { open_dest_many(local, "move", from, done); });
            }
            add(t("Delete\xE2\x80\xA6"), some(local.size(), ""), true, [this, local, done] {
                double size = 0;
                bool folders = false;
                for (const auto& e : local) {
                    size += std::max(0.0, e.size);
                    folders = folders || e.kind == "folder";
                }
                const long long k = static_cast<long long>(local.size());
                std::string body = "!" + (folders ? plural(k, "Delete {n} item, folders with everything in them?", "Delete {n} items, folders with everything in them?")
                                                  : plural(k, "Delete {n} file?", "Delete {n} files?")) +
                                   "\n" + names_of(local);
                if (size > 0) body += "\n" + bytes(size);
                body += "\n\n~" + t("This can't be undone.");
                ask(*this, t("Delete"), text_body(body), t("Delete"), true, [this, local, done] {
                    each(*this, local,
                         [this](const Entry& e, Done d) {
                             JsonValue req = JsonValue::object();
                             req.set("path", e.path);
                             api("POST", "/api/fs/delete", req, d, 0, 120000);
                         },
                         [this, done](std::size_t ok, const std::vector<std::string>& failed) {
                             if (ok) {
                                 toast(plural(static_cast<long long>(ok), "Deleted {n} item", "Deleted {n} items"), Toast::ok);
                                 done();
                                 load_info();
                             }
                             if (!failed.empty()) toast(failures_text(failed), Toast::bad);
                         });
                });
            });
        }
    }
    if (n < rows().size())
        add(t("Select all"), "", false, [this] {
            selected.clear();
            for (const auto& e : rows()) selected.push_back(e.path);
        });
    if (n) add(t("Clear the selection"), "", false, [this] { selected.clear(); });
    choose(*this, n ? plural(static_cast<long long>(n), "{n} selected", "{n} selected") : t("Nothing selected"), std::move(items));
}

}  // namespace alembic::app
