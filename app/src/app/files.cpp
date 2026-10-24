// SPDX-License-Identifier: GPL-3.0-or-later
// Files (pages/files/*): drives and places on the left, the folder's
// contents on the right.
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

namespace {

const char* kind_label(const std::string& kind) {
    if (kind == "fpkg") return "PKG";
    if (kind == "exfat") return "EXFAT";
    if (kind == "ffpfsc") return "FFPFSC";
    if (kind == "ffpkg") return "FFPKG";
    if (kind == "ffpfs") return "FFPFS";
    if (kind == "folder") return "FOLDER";
    if (kind == "file") return "FILE";
    return "";
}

}  // namespace

bool is_convertible(const Entry& e) {
    return e.kind == "fpkg" || e.kind == "exfat" || e.kind == "ffpfsc" || e.kind == "ffpkg" || e.kind == "ffpfs" ||
           e.game;
}

// ---------------------------------------------------------------- rail

void FilesPage::rail(Ui& ui, App& app, const Rect& area) {
    DrawList& l = *ui.list;
    const std::string& c = app.cwd;
    auto within = [&](const std::string& root) { return c == root || starts_with(c, root + "/"); };
    ui.begin_zone("rail", true, true);
    const float offset = ui.begin_scroll("rail", area);
    float y = area.y + 0.25f * R - offset;
    const float x = area.x + 0.3f * R, w = area.w - 0.6f * R;
    bool first_head = true;
    auto h3 = [&](const std::string& text) {
        const float s = 0.72f * R;
        y += (first_head ? 0.4f : 1.0f) * R;
        first_head = false;
        draw::caps(ui, text, x + 0.55f * R, draw::baseline(y + s * 0.725f, s), s, dim);
        y += s * 1.45f + 0.5f * R;
    };
    auto h4 = [&](const std::string& text) {
        const float s = 0.78f * R;
        y += 0.7f * R;
        draw::text_fit(ui, Face::semibold, text, x + 0.55f * R, draw::baseline(y + s * 0.725f, s), s, w - 1.1f * R, muted);
        y += s * 1.45f + 0.3f * R;
    };
    // One rail entry; extra: meter and free space, or a note.
    auto item = [&](const std::string& key, bool on, draw::Icon icon, const std::string& name, double free, double total,
                    const std::string& note, const std::string& go) {
        const float pad_y = 0.55f * R;
        float h = 1.0f * R * 1.45f + 2.0f * pad_y;
        const bool meter = total > 0;
        if (meter) h += 0.4f * R + 0.28f * R;
        const bool has_note = meter || !note.empty();
        if (has_note) h += 0.2f * R + 0.75f * R * 1.45f;
        const Rect r{x, y, w, h};
        const Id id = Id("rail") / key;
        if (ui.visible(r)) {
            if (on) {
                l.rounded_rect(r, 0.6f * R, accent.with_alpha(0.08f));
                l.rounded_rect({r.x, r.y + 0.6f * R, 0.18f * R, r.h - 1.2f * R}, 0.1f * R, accent);
            }
            const float cy = y + pad_y + 0.725f * R;
            const Color ink = on || ui.focused(id) ? style::text : muted;
            draw::icon(ui, icon, {x + 0.6f * R, cy - 0.575f * R, 1.15f * R, 1.15f * R}, ink.with_alpha(0.85f));
            draw::text_fit(ui, Face::regular, name, x + 0.6f * R + 1.75f * R, draw::baseline(cy, 1.0f * R), 1.0f * R,
                           w - 2.95f * R, ink);
            float yy = y + pad_y + 1.45f * R;
            if (meter) {
                yy += 0.4f * R;
                draw::meter(ui, {x + 0.6f * R + 1.75f * R, yy, w - 1.2f * R - 1.75f * R, 0.28f * R},
                            static_cast<float>(1.0 - free / total), free < 20e9);
                yy += 0.28f * R;
            }
            if (has_note) {
                yy += 0.2f * R;
                const std::string text = meter ? t("{size} free", {{"size", bytes(free)}}) : note;
                draw::text_fit(ui, Face::regular, text, x + 0.6f * R + 1.75f * R, draw::baseline(yy + 0.725f * 0.75f * R, 0.75f * R),
                               0.75f * R, w - 2.95f * R, dim);
            }
        }
        if (ui.item(id, r, 0, 0.6f * R)) app.browse(go);
        y += h;
    };

    h3(t("Drives"));
    for (const auto& s : app.space.items()) {
        const std::string root = s["root"].str();
        const bool removable = root.find("usb") != std::string::npos || root.find("ext") != std::string::npos;
        item("r:" + root, within(root), removable ? draw::Icon::usb : draw::Icon::drive, app.drive_name(root),
             s["free"].number(), s["total"].number(), "", root);
    }
    for (const auto& s : app.ftp.items()) {
        const std::string host = s["host"].str();
        const long long port = s["port"].integer(21);
        const JsonValue* in = c.empty() ? nullptr : app.ftp_of(c);
        item("r:" + s["path"].str(), in && (*in)["path"].str() == s["path"].str(), draw::Icon::server, s["name"].str(),
             0, 0, "FTP \xC2\xB7 " + host + (port == 21 ? "" : ":" + std::to_string(port)), s["path"].str());
    }
    if (!app.peer_drives.items().empty()) h3(t("Other engines"));
    for (const auto& s : app.peer_drives.items()) {
        const std::string root = s["root"].str();
        item("r:" + root, within(root), draw::Icon::server, app.drive_name(root), s["free"].number(),
             s["total"].number(), "", root);
    }
    // Places: this engine's, recent folders, then each paired engine's.
    std::vector<const JsonValue*> places, theirs;
    std::set<std::string> peer_roots;
    for (const auto& d : app.peer_drives.items()) peer_roots.insert(d["root"].str());
    for (const auto& loc : app.locations.items()) {
        if (!loc["exists"].boolean()) continue;
        const std::string p = loc["path"].str();
        if (!loc["peer"].str().empty()) {
            if (!peer_roots.count(p)) theirs.push_back(&loc);
            continue;
        }
        if (loc["ftp"].boolean()) continue;
        bool is_drive = false;
        for (const auto& s : app.space.items()) is_drive = is_drive || s["root"].str() == p;
        if (!is_drive) places.push_back(&loc);
    }
    std::vector<std::string> recent;
    for (const auto& p : app.recent()) {
        if (!app.root_of(p) || app.peer_drive_of(p)) continue;
        bool listed = false;
        for (const auto* l2 : places) listed = listed || (*l2)["path"].str() == p;
        if (!listed && recent.size() < 4) recent.push_back(p);
    }
    auto short_label = [](std::string label) {
        const std::size_t hb = label.find(" \xC2\xB7 homebrew");
        if (hb != std::string::npos) label = label.substr(0, hb) + " \xC2\xB7 homebrew";
        if (label == "Internal storage") label = "Internal";
        return place_label(label);
    };
    if (!places.empty() || !theirs.empty() || !recent.empty()) h3(t("Places"));
    if (!theirs.empty() && (!places.empty() || !recent.empty())) h4(app.here_name());
    for (const auto* p : places)
        item("p:" + (*p)["path"].str(), c == (*p)["path"].str(), draw::Icon::place, short_label((*p)["label"].str()), 0,
             0, "", (*p)["path"].str());
    for (const auto& p : recent) item("c:" + p, c == p, draw::Icon::clock, base(p), 0, 0, "", p);
    std::vector<std::string> engines;
    for (const auto* p : theirs) {
        const std::string id = (*p)["peer"].str();
        if (std::find(engines.begin(), engines.end(), id) == engines.end()) engines.push_back(id);
    }
    for (const auto& id : engines) {
        bool head = false;
        for (const auto* p : theirs) {
            if ((*p)["peer"].str() != id) continue;
            if (!head) {
                h4((*p)["engine"].str());
                head = true;
            }
            item("p:" + (*p)["path"].str(), c == (*p)["path"].str(), draw::Icon::place, short_label((*p)["place"].str()),
                 0, 0, "", (*p)["path"].str());
        }
    }
    y += 1.0f * R;
    ui.end_scroll(y + offset - area.y);
    ui.end_zone();
}

// ---------------------------------------------------------------- list

void FilesPage::list(Ui& ui, App& app, const Rect& area) {
    DrawList& l = *ui.list;
    const bool top = app.cwd == "/";
    // ---- bar1: crumbs, summary, tools ----
    float y = area.y + 0.2f * R;
    const float cs = 1.35f * R;
    // Tools on the right.
    {
        float right = area.x + area.w;
        const float bh = 0.35f * R * 2.0f + 0.9f * R * 1.45f + 2.0f;
        const float by = y + (cs * 1.45f + 0.2f * R + 0.85f * R * 1.45f) - bh;
        ui.begin_zone("tools", false, false);
        auto tool = [&](const std::string& id, const std::string& label, draw::Button kind, bool disabled) {
            const float w = draw::button_width(ui, label, true);
            const Rect r{right - w, by, w, bh};
            right -= w + 0.5f * R;
            return draw::button(ui, Id("tool") / id, r, label, kind, true, disabled);
        };
        if (tool("refresh", t("Refresh"), draw::Button::ghost, false)) app.refresh_listing();
        if (tool("mkdir", t("New folder"), draw::Button::normal, top)) {
            const std::string where = app.cwd;
            ask_text(app, t("New folder"), t("New folder"), t("Create"), [&app, where](const std::string& name) {
                if (name.empty()) return;
                JsonValue body = JsonValue::object();
                body.set("path", where + "/" + name);
                app.post("/api/fs/mkdir", body, app.or_toast([&app, where, name](const Reply&) {
                    app.browse(where, where + "/" + name);
                }));
            });
        }
        if (tool("here", draw::kMore, draw::Button::normal, top)) {
            Entry e;
            e.path = app.cwd;
            e.name = base(app.cwd);
            e.kind = "folder";
            app.open_actions(e);
        }
        ui.end_zone();
        // Crumbs: Drives › drive › folder › ...
        ui.begin_zone("crumbs", false, false);
        std::vector<std::pair<std::string, std::string>> parts = {{"/", t("Drives")}};
        if (!top) {
            const JsonValue* r = app.root_of(app.cwd);
            const JsonValue* f = r ? nullptr : app.ftp_of(app.cwd);
            const std::string root = r ? (*r)["root"].str() : f ? (*f)["path"].str() : app.cwd;
            parts.push_back({root, app.drive_name(root)});
            std::string acc = root;
            std::string rest = app.cwd.substr(std::min(root.size(), app.cwd.size()));
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
        float cx = area.x;
        const float max_x = right - 1.0f * R;
        // Too many to fit: the first ones give way (the last ones matter).
        std::size_t first = 0;
        for (;;) {
            float wsum = 0.0f;
            for (std::size_t i = first; i < parts.size(); ++i)
                wsum += std::min(18.0f * R, draw::measure(ui, Face::semibold, parts[i].second, cs)) + 0.7f * R + 1.3f * R;
            if (wsum <= max_x - area.x || first + 1 >= parts.size()) break;
            ++first;
        }
        const float cy = y + cs * 0.725f;
        for (std::size_t i = first; i < parts.size(); ++i) {
            if (i > first || first > 0) {
                draw::chevron(ui, cx + 0.1f * R + cs * 0.2f, cy, cs, 1, dim);
                cx += 0.2f * R + cs * 0.4f;
            }
            const float tw = std::min(18.0f * R, draw::measure(ui, Face::semibold, parts[i].second, cs));
            const Rect r{cx, cy - cs * 0.725f - 0.1f * R, tw + 0.7f * R, cs * 1.45f + 0.2f * R};
            draw::text_fit(ui, Face::semibold, parts[i].second, cx + 0.35f * R, draw::baseline(cy, cs), cs, tw,
                           i + 1 == parts.size() ? style::text : muted);
            if (ui.item(Id("crumb") / parts[i].first, r, 0, 0.45f * R)) app.browse(parts[i].first);
            cx += r.w;
        }
        ui.end_zone();
        y += cs * 1.45f + 0.2f * R;
        // Summary.
        std::string summary;
        if (top) {
            summary = t("Pick a drive");
        } else if (app.listing_loaded) {
            int files = 0, folders = 0;
            double total = 0;
            for (const auto& e : app.entries) {
                if (e.kind == "folder")
                    ++folders;
                else {
                    ++files;
                    total += std::max(0.0, e.size);
                }
            }
            std::vector<std::string> bits;
            if (folders) bits.push_back(plural(folders, "{n} folder", "{n} folders"));
            if (files) bits.push_back(plural(files, "{n} file", "{n} files") + " \xC2\xB7 " + bytes(total));
            const JsonValue* r = app.root_of(app.cwd);
            if (r && ((*r)["total"].number() > 0 || (*r)["engine"].str().empty()))
                bits.push_back(t("{size} free on {drive}", {{"size", bytes((*r)["free"].number())},
                                                            {"drive", app.drive_name((*r)["root"].str())}}));
            else if (app.ftp_of(app.cwd))
                bits.push_back(t("On an FTP server"));
            for (std::size_t i = 0; i < bits.size(); ++i) summary += (i ? "  \xC2\xB7  " : "") + bits[i];
            if (summary.empty()) summary = t("Empty");
        }
        const float ss = 0.85f * R;
        draw::text_fit(ui, Face::regular, summary, area.x + 0.35f * R, draw::baseline(y + 0.2f * R + ss * 0.725f, ss), ss,
                       right - area.x - 1.0f * R, muted);
        y += 0.2f * R + ss * 1.45f + 0.8f * R;
    }
    draw::hline(ui, area.x, y, area.w, line);
    y += 1.0f;
    // ---- the rows ----
    const Rect view{area.x - 0.6f * R, y, area.w + 1.2f * R, area.y + area.h - y};
    ui.begin_zone("list", true, true);
    // A new listing: the focus goes to the row asked for, or the first.
    if (app.focus_list && app.listing_loaded) {
        ui.scroll_to_top("list");
        std::string key = app.focus_after;
        bool found = false;
        for (const auto& e : app.entries) found = found || e.path == key;
        if (!found && !app.entries.empty()) key = app.entries.front().path;
        if (!app.entries.empty()) ui.set_focus(Id("row") / key);
        app.focus_list = false;
        focused_path = key;
    }
    const float offset = ui.begin_scroll("list", view);
    float ry = y + 0.5f * R - offset;
    const float rx = area.x + 0.3f * R - 0.6f * R + 0.6f * R, rw = area.w - 0.6f * R;
    if (app.listing_loaded && app.entries.empty()) {
        draw::text(ui, Face::regular, t("This folder is empty."), area.x + area.w * 0.5f,
                   draw::baseline(ry + 3.0f * R + 0.725f * R, 1.0f * R), 1.0f * R, muted, Align::center);
        ry += 6.0f * R + 1.45f * R;
    }
    in_list_ = false;
    for (const Entry& e : app.entries) {
        const float h = 0.55f * R * 2.0f + 3.4f * R;
        ry += 0.15f * R;
        const Rect r{rx, ry, rw, h};
        const Id id = Id("row") / e.path;
        const bool f = ui.focused(id);
        if (f) {
            focused_path = e.path;
            in_list_ = true;
        }
        if (ui.visible(r)) {
            const Meta* m = (!top && is_convertible(e)) ? app.meta(e.path, e.mtime, e.size) : nullptr;
            if (f) l.rounded_rect(r, 0.7f * R, accent.with_alpha(0.12f));
            const Rect icon{r.x + 0.75f * R, r.y + 0.55f * R, 3.4f * R, 3.4f * R};
            const float text_x = icon.x + icon.w + 1.0f * R;
            float right_x = r.x + r.w - 0.75f * R;
            if (top) {
                Color fill, ink;
                draw::kind_colors("folder", &fill, &ink);
                l.rounded_rect(icon, 0.7f * R, fill);
                const JsonValue* fs = app.ftp_of(e.path);
                const bool usb = e.path.find("usb") != std::string::npos || e.path.find("ext") != std::string::npos;
                draw::icon(ui, fs ? draw::Icon::server : usb ? draw::Icon::usb : draw::Icon::drive,
                           icon.inset(icon.w * 0.225f), ink);
            } else {
                const std::string kind = e.kind == "folder" && !e.game ? "folder"
                                         : e.kind == "file"           ? "file"
                                         : e.game                     ? "game"
                                                                      : e.kind;
                const std::uint32_t tex = m && m->icon ? app.icon(e.path, e.mtime) : 0;
                draw::ico(ui, icon, kind, e.game ? t("GAME") : kind_label(e.kind), tex);
                // ⋯ (more) and the size.
                const float ms = 1.35f * R;
                draw::dots(ui, right_x - 0.75f * R - ms * 0.4f, r.cy(), ms, dim);
                right_x -= 0.75f * R * 2.0f + ms * 0.8f + 1.0f * R;
                if (e.kind != "folder") {
                    const std::string sz = bytes(e.size);
                    const float zs = 0.9f * R;
                    draw::text(ui, Face::regular, sz, right_x, draw::baseline(r.cy(), zs), zs, muted, Align::right);
                    right_x -= draw::measure(ui, Face::regular, sz, zs) + 1.0f * R;
                }
            }
            const float tw = right_x - text_x;
            const float ts = 1.05f * R, ss = 0.82f * R;
            const float t_cy = r.cy() - 0.5f * ss * 1.45f;
            const float s_cy = r.cy() + 0.5f * ts * 1.45f - 0.05f * R;
            std::string title = top ? app.drive_name(e.path) : (m && !m->title.empty() ? m->title : e.name);
            draw::text_fit(ui, Face::regular, title, text_x, draw::baseline(t_cy, ts), ts, tw, style::text);
            if (m) {
                const float pw = draw::fmt_pill(ui, e.game ? "folder" : e.kind, e.game ? t("FOLDER") : kind_label(e.kind),
                                                text_x, s_cy);
                std::vector<std::string> bits;
                if (!m->title.empty()) bits.push_back(e.name);
                if (!m->version.empty()) bits.push_back("v" + version_text(m->version));
                if (!m->title_id.empty()) bits.push_back(m->title_id);
                std::string sub;
                for (std::size_t i = 0; i < bits.size(); ++i) sub += (i ? "  \xC2\xB7  " : "") + bits[i];
                draw::text_fit(ui, Face::regular, sub, text_x + pw + 0.45f * R, draw::baseline(s_cy, ss), ss,
                               tw - pw - 0.45f * R, muted);
            } else {
                std::string plain;
                if (top) {
                    const JsonValue* rt = app.root_of(e.path);
                    const JsonValue* fs = app.ftp_of(e.path);
                    if (rt)
                        plain = t("{free} free of {total}",
                                  {{"free", bytes((*rt)["free"].number())}, {"total", bytes((*rt)["total"].number())}});
                    else if (fs)
                        plain = "FTP \xC2\xB7 " + (*fs)["user"].str() + "@" + (*fs)["host"].str() + ":" +
                                std::to_string((*fs)["port"].integer(21));
                    else
                        plain = e.path;
                } else {
                    std::vector<std::string> bits;
                    if (e.kind != "folder") bits.push_back(bytes(e.size));
                    const std::string d = date(e.mtime);
                    if (!d.empty()) bits.push_back(d);
                    for (std::size_t i = 0; i < bits.size(); ++i) plain += (i ? "  \xC2\xB7  " : "") + bits[i];
                }
                draw::text_fit(ui, Face::regular, plain, text_x, draw::baseline(s_cy, ss), ss, tw, muted);
            }
        }
        if (ui.item(id, r, 0, 0.7f * R)) {
            if (top || (e.kind == "folder" && !e.game))
                app.browse(e.path);
            else if (is_convertible(e))
                app.open_convert(e);
            else
                app.open_actions(e);
        }
        ry += h + 0.15f * R;
    }
    ry += 1.5f * R;
    ui.end_scroll(ry + offset - y);
    ui.end_zone();
}

void FilesPage::build(Ui& ui, App& app, const Rect& area) {
    const Rect inner{area.x + page_pad, area.y, area.w - 2.0f * page_pad, area.h};
    rail(ui, app, {inner.x, inner.y, 15.0f * R, inner.h});
    list(ui, app, {inner.x + 15.0f * R + 1.75f * R, inner.y, inner.w - 15.0f * R - 1.75f * R, inner.h});
}

std::vector<Hint> FilesPage::hints(App& app) {
    const Entry* e = nullptr;
    for (const auto& x : app.entries)
        if (x.path == focused_path) e = &x;
    std::vector<Hint> list;
    list.push_back({Glyph::cross, e ? (e->kind == "folder" && !e->game ? t("Open")
                                       : is_convertible(*e)             ? t("Convert")
                                                                        : t("Options"))
                                    : t("Choose")});
    if (app.cwd != "/") list.push_back({Glyph::circle, t("Back")});
    if (e && app.cwd != "/") list.push_back({Glyph::square, t("Options")});
    return list;
}

void FilesPage::square(App& app) {
    if (app.cwd == "/" || !in_list_) return;
    for (const auto& e : app.entries)
        if (e.path == focused_path) {
            app.open_actions(e);
            return;
        }
}

}  // namespace alembic::app
