// SPDX-License-Identifier: GPL-3.0-or-later
// The grid view of Files and Images (components/Tile.tsx) and the list or
// grid switch above them (components/ViewSwitch.tsx).
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>
#include <cmath>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

std::string App::view_mode(const std::string& which) const {
    const std::string v = prefs["view"][which].str();
    if (v == "list" || v == "grid") return v;
    return which == "images" ? "grid" : "list";  // Files starts as a list, Images as a grid
}

void App::set_view_mode(const std::string& which, const std::string& mode) {
    JsonValue v = prefs["view"].is_object() ? prefs["view"] : JsonValue::object();
    v.set(which, mode);
    prefs.set("view", v);
    save_prefs();
}

float view_switch(Ui& ui, App& app, const std::string& which, float right, float y, float h) {
    const float bw = 2.3f * R;
    const std::string mode = app.view_mode(which);
    DrawList& l = *ui.list;
    for (int k = 0; k < 2; ++k) {
        const bool grid = k == 1;
        const Rect r{right - (2 - k) * bw, y, bw, h};
        const bool on = mode == (grid ? "grid" : "list");
        if (draw::button(ui, Id(which) / (grid ? "view-grid" : "view-list"), r, "", on ? draw::Button::normal : draw::Button::ghost, true))
            app.set_view_mode(which, grid ? "grid" : "list");
        const Color ink = on ? style::text : muted;
        const float s = 1.05f * R, x0 = r.cx() - s * 0.5f, y0 = r.cy() - s * 0.5f;
        if (grid) {
            const float c = s * 0.42f, g = s - 2.0f * c;
            for (int i = 0; i < 4; ++i)
                l.rounded_rect({x0 + (i % 2) * (c + g), y0 + (i / 2) * (c + g), c, c}, 0.15f * c, ink);
        } else {
            for (int i = 0; i < 3; ++i) {
                const float ly = y0 + s * (0.17f + 0.33f * i);
                l.rounded_rect({x0, ly - 0.07f * s, 0.14f * s, 0.14f * s}, 0.07f * s, ink);
                l.rounded_rect({x0 + 0.28f * s, ly - 0.05f * s, 0.72f * s, 0.1f * s}, 0.05f * s, ink);
            }
        }
    }
    return 2.0f * bw;
}

Grid grid_of(float x, float w) {
    Grid g;
    g.x = x;
    g.gap = 0.8f * R;
    g.cols = std::max(1, static_cast<int>(std::floor((w + g.gap) / (10.5f * R + g.gap))));
    g.w = (w - g.gap * static_cast<float>(g.cols - 1)) / static_cast<float>(g.cols);
    const float pad = 0.55f * R;
    g.h = pad + (g.w - 2.0f * pad) + 0.55f * R + 0.98f * R * 1.3f * 2.0f + 0.3f * R + 1.2f * R + pad;
    return g;
}

bool tile(Ui& ui, App& app, Id id, const Rect& r, const TileInfo& info) {
    const bool pressed = ui.item(id, r, 0, 0.9f * R);
    const bool f = ui.focused(id);
    if (!ui.visible(r)) return pressed;
    DrawList& l = *ui.list;
    const Entry& e = info.entry;
    const Meta* m = info.meta;
    if (info.ticked) l.rounded_rect(r, 0.9f * R, accent.with_alpha(0.08f));
    if (f) l.rounded_rect(r, 0.9f * R, accent.with_alpha(0.12f));
    const float pad = 0.55f * R;
    const Rect art{r.x + pad, r.y + pad, r.w - 2.0f * pad, r.w - 2.0f * pad};
    const std::string kind = e.kind == "folder" && !e.game ? "folder" : e.kind == "file" ? "file" : e.game ? "game" : e.kind;
    const std::uint32_t tex = m && m->icon ? app.icon(e.path, e.mtime) : 0;
    draw::ico(ui, art, kind, e.game ? t("GAME") : kind_label(e.kind), tex);
    if (info.dim) l.rounded_rect(art, 0.7f * R, bg.with_alpha(0.4f));
    if (!info.badge.empty()) {
        // The pill on the icon's lower edge, on a solid backing.
        const float pw = draw::measure(ui, Face::semibold, draw::upper(t(info.badge)), 0.7f * R) + 1.8f * R;
        const float right = art.x + art.w - 0.35f * R, cy = art.y + art.h - 0.35f * R - 0.55f * R;
        l.rounded_rect({right - pw, cy - 0.6f * R, pw, 1.2f * R}, 0.6f * R, rgb(0x141a26));
        draw::state_pill(ui, info.badge_state, t(info.badge), right, cy);
    }
    if (info.selecting) {  // the tick on the tile's corner
        const Rect tk{r.x + 0.2f * R, r.y + 0.2f * R, 1.5f * R, 1.5f * R};
        l.rounded_rect(tk, tk.w * 0.5f, info.ticked ? accent : bg);
        if (info.ticked)
            draw::text(ui, Face::semibold, "\xE2\x9C\x93", tk.cx(), draw::baseline(tk.cy(), 0.9f * R), 0.9f * R, Color{1, 1, 1, 1},
                       Align::center);
    }
    float y = art.y + art.h + 0.55f * R;
    const float ts = 0.98f * R;
    draw::paragraph(ui, Face::regular, m && !m->title.empty() ? m->title : e.name, art.x, y, art.w, ts, ts * 1.3f,
                    style::text.with_alpha(info.dim ? 0.6f : 1.0f), 2);
    y += ts * 1.3f * 2.0f + 0.3f * R;
    const float cy = y + 0.6f * R;
    float pw = m ? draw::platform_pill(ui, m->platform, art.x, cy) : 0.0f;
    if (pw) pw += 0.45f * R;
    pw += draw::fmt_pill(ui, e.game ? "folder" : e.kind == "folder" ? "folder" : e.kind,
                         e.game || e.kind == "folder" ? t("FOLDER") : kind_label(e.kind), art.x + pw, cy);
    if (m && (m->packs == "lz4" || m->packs == "zstd"))
        pw += 0.45f * R + draw::fmt_pill(ui, "packs", m->packs == "zstd" ? "ZSTD" : "LZ4", art.x + pw + 0.45f * R, cy);
    if (info.hidden) pw += 0.45f * R + draw::fmt_pill(ui, "file", t("HIDDEN"), art.x + pw + 0.45f * R, cy);
    if (e.kind != "folder" && pw + 4.0f * R < art.w)
        draw::text_fit(ui, Face::regular, bytes(e.size), art.x + pw + 0.45f * R, draw::baseline(cy, 0.78f * R), 0.78f * R,
                       art.w - pw - 0.45f * R, muted);
    return pressed;
}

bool drive_tile(Ui& ui, Id id, const Rect& r, draw::Icon icon, const std::string& title, const std::string& room, float used) {
    const bool pressed = ui.item(id, r, 0, 0.9f * R);
    if (!ui.visible(r)) return pressed;
    DrawList& l = *ui.list;
    if (ui.focused(id)) l.rounded_rect(r, 0.9f * R, accent.with_alpha(0.12f));
    const float pad = 0.55f * R;
    const Rect art{r.x + pad, r.y + pad, r.w - 2.0f * pad, r.w - 2.0f * pad};
    Color fill, ink;
    draw::kind_colors("folder", &fill, &ink);
    l.rounded_rect(art, 0.8f * R, fill);
    draw::icon(ui, icon, art.inset(art.w * 0.3f), ink);
    float y = art.y + art.h + 0.55f * R;
    const float ts = 0.98f * R;
    draw::paragraph(ui, Face::regular, title, art.x, y, art.w, ts, ts * 1.3f, style::text, 2);
    y += ts * 1.3f * 2.0f + 0.3f * R;
    if (used >= 0.0f) {
        draw::meter(ui, {art.x, y + 0.1f * R, art.w, 0.28f * R}, used, used > 0.9f);
        y += 0.65f * R;
    }
    draw::text_fit(ui, Face::regular, room, art.x, draw::baseline(y + 0.4f * R, 0.78f * R), 0.78f * R, art.w, muted);
    return pressed;
}

}  // namespace alembic::app
