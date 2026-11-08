// SPDX-License-Identifier: GPL-3.0-or-later
// Images (pages/images/ImagesPage.tsx): every image, game folder and package
// in the folders ShadowMount+ scans (hidden ones too), grouped by folder,
// with what Files offers for them except moving and renaming (open_actions
// and the bulk actions with `lib`), and each game's AMPR emulator: an update
// when a newer build of it is out, keeping the game's emulator.
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>
#include <memory>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

namespace {

bool uses_apr(const std::string& s) {
    return s == "current" || s == "update" || s == "missing" || s == "unknown" || s == "not_downloaded";
}

JsonValue spec_of(const JsonValue& i) {
    JsonValue job = JsonValue::object();
    job.set("kind", "ampr_emu");
    job.set("input", i["path"].str());
    job.set("ampr_emu", i["ampr"]["status"].str() == "missing" ? "auto" : "replace");
    job.set("emulator", i["ampr"]["target"].str());
    return job;
}

// A folder as a heading: its drive's name, then the folders below it
// ("Internal · homebrew"; state/paths.ts folderLabel).
std::string folder_label(App& app, const std::string& dir) {
    const JsonValue* r = app.root_of(dir);
    if (!r) return app.shown_path(dir);
    const std::string root = (*r)["root"].str();
    std::string rest = dir.size() > root.size() ? dir.substr(root.size()) : "";
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    std::string shown;
    for (std::size_t at = 0; at <= rest.size();) {
        std::size_t slash = rest.find('/', at);
        if (slash == std::string::npos) slash = rest.size();
        if (slash > at) shown += (shown.empty() ? "" : " \xE2\x80\xBA ") + rest.substr(at, slash - at);
        at = slash + 1;
    }
    return app.drive_name(root) + (shown.empty() ? "" : " \xC2\xB7 " + shown);
}

std::string sort_name(const std::string& name) { return !name.empty() && name[0] == '.' ? name.substr(1) : name; }

}  // namespace

void App::refresh_images() { images_->load(*this, true); }

void ImagesPage::opened(App& app) {
    focus_first_ = true;
    load(app, false);
}

void ImagesPage::load(App& app, bool refresh) {
    app.get(refresh ? "/api/images?refresh=1" : "/api/images", [this, &app](const Reply& r) {
        if (!r.ok()) {
            app.toast(t_msg(r.error()), App::Toast::bad);
            return;
        }
        data_ = r.json;
        loaded = true;
        // The rows, in the order they are drawn: by folder, then by name.
        std::vector<JsonValue> list = data_["images"].items();
        std::stable_sort(list.begin(), list.end(), [](const JsonValue& a, const JsonValue& b) {
            const std::string fa = a["folder"].str(), fb = b["folder"].str();
            return fa != fb ? fa < fb : sort_name(a["name"].str()) < sort_name(b["name"].str());
        });
        JsonValue sorted = JsonValue::array();
        for (auto& i : list) sorted.push(std::move(i));
        data_.set("images", std::move(sorted));
        rebuild_entries(app);
    });
    // The paired engines' lists (engines before 0.5.5 have none: no sections then).
    app.get(refresh ? "/api/peers/images?refresh=1" : "/api/peers/images", [this, &app](const Reply& r) {
        JsonValue peers = JsonValue::array();
        if (r.ok())
            for (const JsonValue& p : r.json.items()) {
                std::vector<JsonValue> list = p["images"].items();
                std::stable_sort(list.begin(), list.end(), [](const JsonValue& a, const JsonValue& b) {
                    const std::string fa = a["folder"].str(), fb = b["folder"].str();
                    return fa != fb ? fa < fb : sort_name(a["name"].str()) < sort_name(b["name"].str());
                });
                JsonValue v = p, sorted = JsonValue::array();
                for (auto& i : list) sorted.push(std::move(i));
                v.set("images", std::move(sorted));
                peers.push(std::move(v));
            }
        peers_ = std::move(peers);
        rebuild_entries(app);
    });
}

void ImagesPage::rebuild_entries(App& app) {
    app.image_entries.clear();
    for (const auto& i : data_["images"].items()) app.image_entries.push_back(entry_from(i));
    for (const auto& p : peers_.items())
        for (const auto& i : p["images"].items()) app.image_entries.push_back(entry_from(i));
}

const JsonValue* ImagesPage::item(const std::string& path) const {
    for (const auto& i : data_["images"].items())
        if (i["path"].str() == path) return &i;
    for (const auto& p : peers_.items())
        for (const auto& i : p["images"].items())
            if (i["path"].str() == path) return &i;
    return nullptr;
}

bool ImagesPage::is_pending(const std::string& path) const {
    return std::find(pending_.begin(), pending_.end(), path) != pending_.end();
}

bool ImagesPage::updatable(const JsonValue& i) const {
    const std::string s = i["ampr"]["status"].str();
    return i["allowed"].boolean() && !is_pending(i["path"].str()) && (s == "update" || s == "missing" || s == "not_downloaded");
}

std::string ImagesPage::target_name(const JsonValue& i) const {
    const std::string id = i["ampr"]["target"].str();
    for (const auto& e : data_["emulators"].items())
        if (e["id"].str() == id) return e["version"].str().empty() ? e["name"].str() : e["name"].str() + " " + e["version"].str();
    return id;
}

// Emulators not downloaded yet are asked for once, for all of them; then a
// job each.
void ImagesPage::queue(App& app, std::vector<JsonValue> list, std::function<void(int)> done) {
    struct State {
        std::vector<JsonValue> list;
        JsonValue needs = JsonValue::array();
        std::size_t asked = 0;
    };
    auto st = std::make_shared<State>();
    st->list = std::move(list);
    auto run_jobs = [this, &app, st, done](bool ok) {
        if (!ok) return;
        auto queued = std::make_shared<int>(0);
        auto left = std::make_shared<std::size_t>(st->list.size());
        for (const JsonValue& i : st->list) {
            const std::string path = i["path"].str();
            app.post("/api/jobs", spec_of(i), [this, &app, path, queued, left, done](const Reply& r) {
                if (r.ok()) {
                    ++*queued;
                    pending_.push_back(path);
                } else {
                    app.toast(base(path) + ": " + t_msg(r.error()), App::Toast::bad);
                }
                if (--*left == 0) {
                    if (*queued) app.refresh_jobs();
                    done(*queued);
                }
            });
        }
    };
    if (st->list.empty()) return done(0);
    for (const JsonValue& i : st->list) {
        app.api("POST", "/api/plan", spec_of(i), [&app, st, run_jobs](const Reply& r) {
            if (r.ok())
                for (const auto& d : r.json["needs"].items()) {
                    bool seen = false;
                    for (const auto& y : st->needs.items()) seen = seen || y["id"].str() == d["id"].str();
                    if (!seen) st->needs.push(d);
                }
            if (++st->asked < st->list.size()) return;
            JsonValue plan = JsonValue::object();
            plan.set("output", "");
            plan.set("needs", st->needs);
            app.get_needs(plan, "This needs {name}, which isn't downloaded yet.", run_jobs);
        }, 1, 120000);
    }
}

void ImagesPage::update_ampr(App& app, std::vector<JsonValue> list, std::function<void()> after) {
    std::vector<JsonValue> todo;
    for (const auto& i : list)
        if (updatable(i) || (i["ampr"]["status"].str() == "unknown" && i["allowed"].boolean())) todo.push_back(i);
    if (todo.empty()) {
        app.toast(t("Every image is up to date"), App::Toast::ok);
        return;
    }
    if (todo.size() == 1) {
        const JsonValue i = todo.front();
        const std::string path = i["path"].str();
        auto go = [this, &app, i, path, after] {
            queue(app, {i}, [&app, path, after](int n) {
                if (n) app.toast(t("Queued: {name}", {{"name", base(path)}}), App::Toast::ok);
                if (n && after) after();
            });
        };
        if (i["ampr"]["status"].str() != "unknown") return go();
        std::string body = "@" + app.shown_path(path) + "\n\n";
        body += t("This game has a libSceAmpr.sprx that is not an official build of either AMPR emulator. Replace it with {new}?",
                  {{"new", target_name(i)}});
        ask(app, t("Replace the AMPR emulator?"), text_body(body), t("Replace"), false, go);
        return;
    }
    // Several: custom builds only one at a time.
    todo.erase(std::remove_if(todo.begin(), todo.end(), [](const JsonValue& i) { return i["ampr"]["status"].str() == "unknown"; }),
               todo.end());
    long long add = 0;
    std::string names;
    for (std::size_t k = 0; k < todo.size(); ++k) {
        if (todo[k]["ampr"]["status"].str() == "missing") ++add;
        if (k < 6) names += (k ? "\n" : "@") + base(todo[k]["path"].str());
    }
    if (todo.size() > 6) names += "\n" + t("and {n} more", {{"n", std::to_string(todo.size() - 6)}});
    std::string body = names + "\n\n~" +
                       t("Each keeps its emulator (ampr_emu or ampr-emu-ng) and gets that one's newest build; a game without one gets the one in use.");
    if (add) body += "\n\n~" + plural(add, "{n} of them has no emulator yet.", "{n} of them have no emulator yet.");
    body += "\n\n~" + t("Game folders are changed in place; images are not changed: ShadowMount+ gets a backport folder for each.");
    const long long n = static_cast<long long>(todo.size());
    ask(app, plural(n, "Update {n} image?", "Update {n} images?"), text_body(body), t("Update"), false, [this, &app, todo, after] {
        queue(app, todo, [&app, after](int queued) {
            if (queued) app.toast(plural(queued, "{n} job queued", "{n} jobs queued"), App::Toast::ok);
            if (queued && after) after();
        });
    });
}

void ImagesPage::update_all(App& app) {
    std::vector<JsonValue> list;
    for (const auto& i : data_["images"].items())
        if (updatable(i)) list.push_back(i);
    update_ampr(app, list);
}

void ImagesPage::square(App& app) {
    if (app.selecting) return app.open_bulk_actions();
    if (const JsonValue* i = item(focused_)) app.open_actions(entry_from(*i), i);
}

std::vector<Hint> ImagesPage::hints(App& app) {
    if (app.selecting)
        return {{Glyph::cross, app.is_selected(focused_) ? t("Untick") : t("Tick")}, {Glyph::square, t("Actions")},
                {Glyph::circle, t("Done")}};
    if (!item(focused_)) return {{Glyph::cross, t("Choose")}};
    return {{Glyph::cross, t("Convert")}, {Glyph::square, t("Options")}};
}

void ImagesPage::build(Ui& ui, App& app, const Rect& area) {
    if (!loaded && data_.is_null()) {
        loaded = true;  // asked once per opening (show_tab asks again)
        load(app, false);
    }
    // While the engine reads the folders, look again every second and a half.
    bool peers_reading = false;
    for (const auto& p : peers_.items()) peers_reading = peers_reading || p["refreshing"].boolean();
    if ((data_["refreshing"].boolean() || peers_reading) && ui.time > poll_at_) {
        poll_at_ = ui.time + 1.5f;
        load(app, false);
    }
    // A job ended (a conversion's output, an emulator put in place): read the
    // folders again; AMPR jobs queued from here stop showing "queued" then.
    {
        std::size_t active = 0;
        bool waiting = false;
        for (const auto& j : app.jobs) {
            if (!App::is_active(j)) continue;
            ++active;
            if (j["spec"]["kind"].str() == "ampr_emu" && is_pending(j["spec"]["input"].str())) waiting = true;
        }
        if (!pending_.empty() && !waiting) pending_.clear();
        if (active < active_before_) load(app, true);
        active_before_ = active;
    }
    const float w = std::min(62.0f * R, area.w - 2.0f * page_pad);
    const float x = area.x + (area.w - w) * 0.5f;
    const std::vector<JsonValue>& list = data_["images"].items();
    int todo = 0;
    for (const auto& i : list) todo += updatable(i) ? 1 : 0;
    const bool refreshing = data_["refreshing"].boolean();
    // ---- the bar, as Files': title and summary, the tools on the right ----
    float y = area.y + 0.2f * R;
    const float cs = 1.35f * R, ss = 0.85f * R;
    {
        const float bh = 0.35f * R * 2.0f + 0.9f * R * 1.45f + 2.0f;
        const float by = y + (cs * 1.45f + 0.2f * R + ss * 1.45f) - bh;
        ui.begin_zone("images-tools", false, false);
        float right = x + w;
        right -= view_switch(ui, app, "images", right, by, bh) + 0.5f * R;
        const std::string again = t("Refresh");
        const float gw = draw::button_width(ui, again, true);
        if (draw::button(ui, "images:again", {right - gw, by, gw, bh}, again, draw::Button::ghost, true, data_.is_null() || refreshing))
            load(app, true);
        right -= gw + 0.5f * R;
        if (todo) {
            const std::string all = t("Update AMPR emulators ({n})", {{"n", std::to_string(todo)}});
            const float aw = draw::button_width(ui, all, true);
            if (draw::button(ui, "images:all", {right - aw, by, aw, bh}, all, draw::Button::normal, true)) update_all(app);
            right -= aw + 0.5f * R;
        }
        ui.end_zone();
        draw::text_fit(ui, Face::semibold, t("Images"), x + 0.35f * R, draw::baseline(y + cs * 0.725f, cs), cs, right - x, style::text);
        y += cs * 1.45f + 0.2f * R;
        std::string summary;
        if (data_.is_null()) {
            summary = t("Looking for images\xE2\x80\xA6");
        } else if (refreshing && list.empty()) {
            summary = t("Reading the games\xE2\x80\xA6");
        } else {
            std::size_t total = list.size();
            for (const auto& p : peers_.items()) total += p["images"].items().size();
            summary = plural(static_cast<long long>(total), "{n} item", "{n} items");
            if (todo) summary += "  \xC2\xB7  " + plural(todo, "{n} AMPR emulator to update", "{n} AMPR emulators to update");
            if (refreshing) summary += "  \xC2\xB7  " + t("Reading the games\xE2\x80\xA6");
        }
        draw::text_fit(ui, Face::regular, summary, x + 0.35f * R, draw::baseline(y + ss * 0.725f, ss), ss, right - x, muted);
        y += ss * 1.45f + 0.8f * R;
    }
    draw::hline(ui, x, y, w, line);
    y += 1.0f;
    // ---- the list ----
    const Rect view{area.x, y, area.w, area.y + area.h - y};
    ui.begin_zone("images", false, true);
    if (focus_first_ && !list.empty()) {
        ui.set_focus(Id("img") / list.front()["path"].str());
        focus_first_ = false;
    }
    const float offset = ui.begin_scroll("images", view);
    y = view.y + 0.5f * R - offset;
    if (data_["smp_update_emulators"].boolean() && todo) {
        const std::string note = t("ShadowMount+'s “update emulators” is on: when a game starts, it puts its own copy in place of a "
                                   "matching emulator. Turn it off in ShadowMount+ for games that should keep the one shown here.");
        const float nh = draw::paragraph_height(ui, Face::regular, note, w - 2.0f * R, 1.0f * R, 1.45f * R) + 1.6f * R;
        ui.list->bordered_rect({x, y, w, nh}, radius, warn.with_alpha(0.08f), 1.0f, warn.with_alpha(0.35f));
        draw::paragraph(ui, Face::regular, note, x + 1.0f * R, y + 0.8f * R, w - 2.0f * R, 1.0f * R, 1.45f * R, style::text);
        y += nh + 0.5f * R;
    }
    DrawList& l = *ui.list;
    bool any_focused = false;
    const bool grid = app.view_mode("images") == "grid";
    // One engine's items, folder by folder (rows or tiles).
    auto draw_items = [&](const std::vector<JsonValue>& list) {
        std::string folder;
        if (grid) {
            // Folder by folder: its heading, then its tiles.
            const Grid g = grid_of(x, w);
            for (std::size_t a = 0; a < list.size();) {
                std::size_t b = a;
                while (b < list.size() && list[b]["folder"].str() == list[a]["folder"].str()) ++b;
                y += section(ui, folder_label(app, list[a]["folder"].str()), x + 0.3f * R, y) + 0.3f * R;
                for (std::size_t k = a; k < b; ++k) {
                    const JsonValue& i = list[k];
                    const Rect r = g.cell(static_cast<int>(k - a), y);
                    const Id id = Id("img") / i["path"].str();
                    TileInfo info;
                    info.entry = entry_from(i);
                    info.meta = ui.visible(r) ? app.meta(info.entry.path, info.entry.mtime, info.entry.size) : nullptr;
                    info.selecting = app.selecting;
                    info.ticked = app.selecting && app.is_selected(info.entry.path);
                    info.hidden = info.dim = i["hidden"].boolean();
                    const std::string status = i["ampr"]["status"].str();
                    if (is_pending(info.entry.path)) {
                        info.badge_state = "queued";
                        info.badge = "queued";
                    } else if (status == "update") {
                        info.badge_state = "running";
                        info.badge = "update available";
                    } else if (status == "missing") {
                        info.badge_state = "failed";
                        info.badge = "no emulator";
                    } else if (status == "unknown") {
                        info.badge_state = "queued";
                        info.badge = "unknown build";
                    } else if (status == "not_downloaded") {
                        info.badge_state = "queued";
                        info.badge = "not downloaded here";
                    }
                    if (tile(ui, app, id, r, info)) {
                        if (app.selecting) app.toggle_selected(info.entry.path);
                        else app.open_convert(info.entry);
                    }
                    if (ui.focused(id)) {
                        focused_ = info.entry.path;
                        any_focused = true;
                    }
                }
                y += g.height(static_cast<int>(b - a)) + 0.4f * R;
                a = b;
            }
        }
        const std::vector<JsonValue> none;
        for (const JsonValue& i : grid ? none : list) {
            const Entry e = entry_from(i);
            if (i["folder"].str() != folder) {
                folder = i["folder"].str();
                y += section(ui, folder_label(app, folder), x + 0.3f * R, y);
            }
            const float h = 0.55f * R * 2.0f + 3.4f * R;
            y += 0.15f * R;
            const Rect r{x, y, w, h};
            const Id id = Id("img") / e.path;
            if (ui.item(id, r, 0, 0.7f * R)) {
                if (app.selecting) app.toggle_selected(e.path);
                else app.open_convert(e);
            }
            const bool f = ui.focused(id);
            if (f) {
                focused_ = e.path;
                any_focused = true;
            }
            const bool ticked = app.selecting && app.is_selected(e.path);
            if (ui.visible(r)) {
                const Meta* m = app.meta(e.path, e.mtime, e.size);
                const float alpha = i["hidden"].boolean() ? 0.6f : 1.0f;
                if (ticked) l.rounded_rect(r, 0.7f * R, accent.with_alpha(0.08f));
                if (f) l.rounded_rect(r, 0.7f * R, accent.with_alpha(0.12f));
                const Rect icon{r.x + 0.75f * R, r.y + 0.55f * R, 3.4f * R, 3.4f * R};
                const std::uint32_t tex = m && m->icon ? app.icon(e.path, e.mtime) : 0;
                draw::ico(ui, icon, e.game ? "game" : e.kind, e.game ? t("GAME") : kind_label(e.kind), tex);
                if (app.selecting) {  // the tick on the icon's corner (.row .tick)
                    const Rect tk{icon.x - 0.35f * R, icon.y - 0.35f * R, 1.3f * R, 1.3f * R};
                    l.rounded_rect(tk, tk.w * 0.5f, ticked ? accent : panel2);
                    if (ticked)
                        draw::text(ui, Face::semibold, "\xE2\x9C\x93", tk.cx(), draw::baseline(tk.cy(), 0.9f * R), 0.9f * R, bg,
                                   Align::center);
                }
                const float text_x = icon.x + icon.w + 1.0f * R;
                // ⋯ (Options), then the AMPR pill or the size.
                float right_x = r.x + r.w - 0.75f * R;
                const float ms = 1.35f * R;
                draw::dots(ui, right_x - 0.75f * R - ms * 0.4f, r.cy(), ms, dim);
                right_x -= 0.75f * R * 2.0f + ms * 0.8f + 1.0f * R;
                const std::string status = i["ampr"]["status"].str();
                std::string pill_state, pill;
                if (is_pending(e.path)) {
                    pill_state = "queued";
                    pill = "queued";
                } else if (status == "update") {
                    pill_state = "running";
                    pill = "update available";
                } else if (status == "missing") {
                    pill_state = "failed";
                    pill = "no emulator";
                } else if (status == "unknown") {
                    pill_state = "queued";
                    pill = "unknown build";
                } else if (status == "not_downloaded") {
                    pill_state = "queued";
                    pill = "not downloaded here";
                }
                if (!pill.empty()) {
                    right_x -= draw::state_pill(ui, pill_state, t(pill), right_x, r.cy()) + 1.0f * R;
                } else if (e.kind != "folder") {
                    const std::string sz = bytes(e.size);
                    const float zs = 0.9f * R;
                    draw::text(ui, Face::regular, sz, right_x, draw::baseline(r.cy(), zs), zs, muted, Align::right);
                    right_x -= draw::measure(ui, Face::regular, sz, zs) + 1.0f * R;
                }
                const float tw = right_x - text_x;
                const float ts = 1.05f * R, ss = 0.82f * R;
                const float t_cy = r.cy() - 0.5f * ss * 1.45f;
                const float s_cy = r.cy() + 0.5f * ts * 1.45f - 0.05f * R;
                draw::text_fit(ui, Face::regular, m && !m->title.empty() ? m->title : e.name, text_x, draw::baseline(t_cy, ts), ts, tw,
                               style::text.with_alpha(alpha));
                float pw = m ? draw::platform_pill(ui, m->platform, text_x, s_cy) : 0.0f;
                if (pw) pw += 0.45f * R;
                pw += draw::fmt_pill(ui, e.game ? "folder" : e.kind, e.game ? t("FOLDER") : kind_label(e.kind), text_x + pw, s_cy);
                if (m && (m->packs == "lz4" || m->packs == "zstd"))
                    pw += 0.45f * R + draw::fmt_pill(ui, "packs", m->packs == "zstd" ? "ZSTD" : "LZ4", text_x + pw + 0.45f * R, s_cy);
                if (i["hidden"].boolean()) pw += 0.45f * R + draw::fmt_pill(ui, "file", t("HIDDEN"), text_x + pw + 0.45f * R, s_cy);
                // The file's name first when the title is the game's (what Rename changes).
                std::string sub = m && !m->title.empty() ? e.name : "";
                if (uses_apr(status)) {
                    if (!sub.empty()) sub += "  \xC2\xB7  ";
                    if (i["ampr"]["emulator"].is_object()) {
                        sub += i["ampr"]["emulator"]["name"].str() + " \xC2\xB7 " +
                              (i["ampr"]["where"].str() == "backport" ? t("in its backport") : t("in the game"));
                        if (status == "update" || status == "not_downloaded") sub += " \xE2\x86\x92 " + target_name(i);
                    } else {
                        sub += status == "unknown" ? t("a libSceAmpr.sprx Alembic doesn't know") : t("no AMPR emulator");
                    }
                } else {
                    std::vector<std::string> bits;
                    sub.clear();
                    if (m && !m->title.empty()) bits.push_back(e.name);
                    if (m && !m->version.empty()) bits.push_back("v" + version_text(m->version));
                    if (m && !m->title_id.empty()) bits.push_back(m->title_id);
                    if (!m) bits.push_back(date(e.mtime));
                    for (std::size_t k = 0; k < bits.size(); ++k) sub += (k ? "  \xC2\xB7  " : "") + bits[k];
                }
                draw::text_fit(ui, Face::regular, sub, text_x + pw + 0.45f * R, draw::baseline(s_cy, ss), ss, tw - pw - 0.45f * R,
                               muted);
            }
            y += h;
        }
    };
    auto message = [&](const std::string& text) {
        draw::text(ui, Face::regular, text, x + w * 0.5f, draw::baseline(y + 2.0f * R, 1.0f * R), 1.0f * R, muted, Align::center);
        y += 4.0f * R;
    };
    // With paired engines, a section per engine: this one first.
    const std::vector<JsonValue>& peers = peers_.items();
    auto heading = [&](const std::string& name) {
        y += 0.8f * R;
        draw::text_fit(ui, Face::semibold, name, x + 0.3f * R, draw::baseline(y + 1.15f * R * 0.725f, 1.15f * R), 1.15f * R, w, style::text);
        y += 1.15f * R * 1.45f + 0.3f * R;
        draw::hline(ui, x, y, w, line);
        y += 0.4f * R;
    };
    if (!peers.empty()) heading(app.here_name());
    if (!data_.is_null() && list.empty() && !data_["refreshing"].boolean())
        message(t("No images, game folders or packages in ShadowMount+'s folders."));
    draw_items(list);
    for (const JsonValue& p : peers) {
        const std::string name = p["engine"]["name"].str(), state = p["state"].str();
        heading(name);
        if (state == "offline") message(t("{engine} is offline", {{"engine", name}}));
        else if (state == "old") message(t("Update {engine} to see its images here", {{"engine", name}}));
        else if (state == "error") message(p["error"].str().empty() ? t("{engine} did not answer", {{"engine", name}}) : p["error"].str());
        else if (p["images"].items().empty())
            message(p["refreshing"].boolean() ? t("Reading the games\xE2\x80\xA6") : t("No images, game folders or packages in ShadowMount+'s folders."));
        else draw_items(p["images"].items());
    }
    if (!any_focused) focused_.clear();
    const JsonValue& dirs = data_["dirs"];
    if (!dirs.items().empty()) {
        std::string text;
        for (const auto& d : dirs.items()) text += (text.empty() ? "" : ", ") + app.shown_path(d.str());
        y += 1.0f * R;
        y += draw::paragraph(ui, Face::regular, t("Folders searched: {list}", {{"list", text}}), x + 0.2f * R, y, w - 0.4f * R,
                             0.9f * R, 1.4f * R, muted);
    }
    y += 2.0f * R;
    ui.end_scroll(y + offset - view.y);
    ui.end_zone();
}

}  // namespace alembic::app
