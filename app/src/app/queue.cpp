// SPDX-License-Identifier: GPL-3.0-or-later
// The queue (pages/queue/*): jobs running or waiting, then the finished ones.
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>
#include <cmath>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

namespace {

bool is_conv_kind(const std::string& k) {
    return k == "fpkg" || k == "exfat" || k == "ffpfsc" || k == "ffpkg" || k == "ffpfs";
}
const char* kind_label(const std::string& kind) {
    if (kind == "fpkg") return "PKG";
    if (kind == "exfat") return "EXFAT";
    if (kind == "ffpfsc") return "FFPFSC";
    if (kind == "ffpkg") return "FFPKG";
    if (kind == "ffpfs") return "FFPFS";
    return "";
}

std::string join(const std::vector<std::string>& bits) {
    std::string out;
    for (const auto& b : bits) {
        if (b.empty()) continue;
        if (!out.empty()) out += "  \xC2\xB7  ";
        out += b;
    }
    return out;
}

// Where a waiting job continues from, in percent (-1 without a checkpoint).
int cp_pct(const JsonValue& j) {
    const JsonValue& cp = j["checkpoint"];
    if (!cp.is_object() || cp["total"].number() <= 0) return -1;
    return static_cast<int>(std::floor(100.0 * cp["done"].number() / cp["total"].number()));
}
std::string cp_text(const JsonValue& j) {
    const int p = cp_pct(j);
    return p < 0 ? "" : t("{n}%", {{"n", std::to_string(p)}});
}

}  // namespace

// What a job is, in words: its title and where it goes (jobText.ts).
void job_text(App& app, const JsonValue& j, std::string* title, std::string* to) {
    const JsonValue& s = j["spec"];
    const std::string kind = s["kind"].str();
    *title = base(s["input"].str());
    // A dump: its game, and where it goes (only the folder until a next-game dump takes a game).
    if (!s["game"].str().empty()) {
        *title = app.dump_title(j);
        *to = "\xE2\x86\x92 " + app.shown_path(!s["output"].str().empty() ? s["output"].str() : s["output_dir"].str());
        return;
    }
    if (kind == "copy" || kind == "move")
        *to = t(kind == "copy" ? "Copy to {path}" : "Move to {path}", {{"path", app.shown_path(s["output"].str())}});
    else if (kind == "install")
        *to = t("Install on this console");
    else if (kind == "ampr_emu")
        *to = j["result"]["output"].str().empty()
                  ? t("Add the AMPR emulator")
                  : t("AMPR emulator into {path}", {{"path", app.shown_path(j["result"]["output"].str())}});
    else
        *to = "\xE2\x86\x92 " + app.shown_path(s["output"].str());
}
std::string job_cp_text(const JsonValue& j) { return cp_text(j); }

namespace {

// The job's steps (JobCard.tsx Steps), as a line that wraps: done ones
// ticked, the current one with its own percentage, later ones grey, where a
// failed job stopped. Drawn only when `draw_it`; returns its height.
float steps_line(Ui& ui, const JsonValue& j, float x, float y, float w, bool draw_it) {
    const std::vector<JsonValue>& list = j["steps"].items();
    const int at = static_cast<int>(j["step"].integer(-1));
    const std::string state = j["state"].str();
    const double done = j["done"].number(), total = j["total"].number();
    const float s = 0.85f * R, lh = 1.6f * R, mark = 1.25f * R, gap = 1.1f * R;
    float cx = x, cy = y + lh * 0.5f;
    for (std::size_t i = 0; i < list.size(); ++i) {
        const int ii = static_cast<int>(i);
        const bool is_done = ii < at, cur = ii == at, failed = cur && state == "failed";
        const std::string label = t(list[i]["label"].str());
        const std::string pct = cur && state == "running" && total > 0
                                    ? t("{n}%", {{"n", std::to_string(static_cast<int>(std::floor(100.0 * done / total)))}})
                                    : std::string();
        const Face face = cur && !failed ? Face::semibold : Face::regular;
        const float lw = draw::measure(ui, face, label, s);
        const float pw = pct.empty() ? 0.0f : 0.4f * R + draw::measure(ui, Face::regular, pct, s);
        const float iw = mark + 0.45f * R + lw + pw;
        if (cx > x && cx + iw > x + w) {  // wrap
            cx = x;
            cy += lh;
        }
        if (draw_it) {
            const Rect m{cx, cy - mark * 0.5f, mark, mark};
            const Color ring = is_done ? ok : failed ? bad : cur ? accent : line2;
            const bool filled = cur && !failed && state == "running";
            ui.list->bordered_rect(m, mark * 0.5f, filled ? accent : Color{0, 0, 0, 0}, 1.0f, ring);
            const std::string mk = is_done ? "\xE2\x9C\x93" : failed ? "\xC3\x97" : std::to_string(i + 1);
            const float ms = 0.7f * R;
            draw::text(ui, Face::regular, mk, m.cx(), draw::baseline(cy, ms), ms,
                       filled ? Color{1, 1, 1, 1} : is_done ? ok : failed ? bad : cur ? accent : dim, Align::center);
            const Color ink = failed ? bad : cur ? style::text : is_done ? muted : dim;
            draw::text(ui, face, label, cx + mark + 0.45f * R, draw::baseline(cy, s), s, ink);
            if (!pct.empty()) draw::text(ui, Face::regular, pct, cx + mark + 0.45f * R + lw + 0.4f * R, draw::baseline(cy, s), s, accent);
        }
        cx += iw + gap;
    }
    return cy + lh * 0.5f - y;
}

// One card (JobCard.tsx); returns its height.
float card(Ui& ui, App& app, const JsonValue& j, float x, float y, float w) {
    DrawList& l = *ui.list;
    const JsonValue& s = j["spec"];
    const JsonValue& r = j["result"];
    const std::string state = j["state"].str();
    std::string title, to;
    job_text(app, j, &title, &to);
    // The game's info, from the input, or from the output once the input is gone.
    // A dump's game comes by title ID instead (this engine's games only).
    const std::string input = s["input"].str(), output = r["output"].str();
    const std::string game = s["game"].str();
    const bool next = game == "next";
    const JsonValue* g = !game.empty() && !next && !j["engine"].is_object() ? app.game_info(game) : nullptr;
    const Meta* m = game.empty() ? app.meta(input, 0, 0) : nullptr;
    std::string meta_from = input;
    if (game.empty() && !m && state == "done" && !output.empty() && output != input && app.meta_none(input, 0, 0)) {
        m = app.meta(output, 0, 0);
        meta_from = output;
    }
    const double done = j["done"].number(), total = j["total"].number();
    const double pct = total > 0 ? std::min(100.0, 100.0 * done / total) : 0.0;
    // The bar is the whole job when the engine says how far (engines from 0.6.6).
    const bool has_overall = j.has("overall") && !j["steps"].items().empty();
    const double overall = has_overall ? j["overall"].number() / 10.0 : 0.0;
    const double bar_pct = has_overall ? overall : pct;
    // Steps worth listing: more than one, while it waits or runs, or the step a job failed in.
    const bool show_steps = j["steps"].items().size() > 1 && (App::is_active(j) || (state == "failed" && j["step"].integer(-1) >= 0));
    std::string detail;
    if (state == "running") {
        const double rate = app.rate_of(j);
        detail = join({t(j["stage"].str()), has_overall ? t("{n}%", {{"n", fixed(overall, 1)}}) : total > 0 ? t("{n}%", {{"n", fixed(pct, 1)}}) : "",
                       rate > 1 ? bytes(rate) + "/s" : "",
                       rate > 1 && total > 0 ? t("{time} left", {{"time", duration((total - done) / rate)}}) : "",
                       j.has("elapsed") ? t("{time} so far", {{"time", duration(j["elapsed"].number())}}) : ""});
    } else if (state == "done") {
        detail = join({t_method(r["method"].str()), r["output_bytes"].number() > 0 ? bytes(r["output_bytes"].number()) : "",
                       j.has("elapsed") ? duration(j["elapsed"].number()) : "",
                       r["source_deleted"].boolean() ? t("source deleted") : ""});
    } else if (state == "queued" && !game.empty() && !j["stage"].str().empty()) {
        // A dump waits for its game (or its game closed): the engine says which.
        detail = join({t(j["stage"].str()), cp_text(j).empty() ? "" : t("Continues from {p}", {{"p", cp_text(j)}})});
    } else if (state == "queued") {
        detail = cp_text(j).empty() ? t("Waiting for the job before it")
                                    : t("Waiting \xC2\xB7 continues from {p}", {{"p", cp_text(j)}});
    } else if (state == "paused") {
        detail = cp_text(j).empty() ? t(j["stage"].str().empty() ? "Paused" : j["stage"].str())
                                    : t("Paused at {p} \xC2\xB7 resume to continue", {{"p", cp_text(j)}});
    } else {
        detail = j["finished"].integer() ? date(j["finished"].integer()) : "";
    }
    std::vector<std::string> tags;
    if (j["engine"].is_object()) tags.push_back(t("on {engine}", {{"engine", j["engine"]["name"].str()}}));
    const std::string kind = s["kind"].str();
    if (kind == "convert") {
        tags.push_back(s["to"].str());
        if (s["to"].str() == "ffpfsc") tags.push_back(t("level {n}", {{"n", std::to_string(s["level"].integer())}}));
        if (s["lz4"].boolean()) tags.push_back("LZ4 \xC2\xB7 " + s["lz4_profile"].str());
        if (s["zstd"].boolean())
            tags.push_back("zstd \xC2\xB7 " + t("level {n}", {{"n", std::to_string(s["zstd_level"].integer(5))}}));
        if (s["delete_source"].boolean())
            tags.push_back(s["verify"].boolean() ? t("check, then delete source") : t("delete source"));
    }
    // Lines under the progress: results, notes, the error.
    struct Line {
        std::string text;
        Color color;
    };
    std::vector<Line> lines;
    if (!r["ampr_emulator"].str().empty())
        lines.push_back({t("AMPR emulator added: {path}", {{"path", app.shown_path(r["ampr_emulator"].str())}}), muted});
    if (!r["source_hidden"].str().empty())
        lines.push_back({t("Source hidden from ShadowMount+ as {name}", {{"name", base(r["source_hidden"].str())}}), muted});
    if (!r["smp_source"].str().empty())
        lines.push_back({t("ShadowMount+ still uses {path} for this game. Hide it (\xE2\x8B\xAF on this job) to switch to the new one.",
                           {{"path", app.shown_path(r["smp_source"].str())}}),
                         warn});
    if (r["lz4"].is_object())
        lines.push_back({t("LZ4: {a} of {b} files packed", {{"a", std::to_string(r["lz4"]["files_packed"].integer())},
                                                            {"b", std::to_string(r["lz4"]["files_total"].integer())}}) +
                             " \xC2\xB7 " + bytes(r["lz4"]["packed_bytes"].number()) + " \xE2\x86\x92 " +
                             bytes(r["lz4"]["volume_bytes"].number()),
                         muted});
    if (r["zstd"].is_object())
        lines.push_back({t("zstd: {a} of {b} files packed", {{"a", std::to_string(r["zstd"]["files_packed"].integer())},
                                                             {"b", std::to_string(r["zstd"]["files_total"].integer())}}) +
                             " \xC2\xB7 " + bytes(r["zstd"]["packed_bytes"].number()) + " \xE2\x86\x92 " +
                             bytes(r["zstd"]["volume_bytes"].number()),
                         muted});
    if (!r["source_kept"].str().empty())
        lines.push_back({t("Source kept: {why}", {{"why", t(r["source_kept"].str())}}), warn});
    {
        std::string notes;
        for (const auto& n : r["lz4"]["profile"]["notes"].items()) notes += (notes.empty() ? "" : " ") + t_msg(n.str());
        if (!notes.empty()) lines.push_back({notes, warn});
        std::string warns;
        for (const auto& n : r["warnings"].items()) warns += (warns.empty() ? "" : " ") + t_msg(n.str());
        if (!warns.empty()) lines.push_back({warns, warn});
    }
    if (!j["error"].str().empty() && state != "cancelled") lines.push_back({t_msg(j["error"].str()), bad});

    const float pad_x = 1.2f * R, pad_y = 1.0f * R;
    const float inner_w = w - 2.0f * pad_x;
    const float ls = 0.88f * R, lh = ls * 1.45f;
    // Height first.
    float h = pad_y * 2.0f + 3.4f * R;
    const bool bar = state == "running" || (!cp_text(j).empty() && App::is_active(j));
    if (bar) h += 0.8f * R + 0.55f * R;
    const float steps_h = show_steps ? steps_line(ui, j, x + pad_x, 0.0f, inner_w, false) : 0.0f;
    if (show_steps) h += 0.6f * R + steps_h;
    h += 0.6f * R + std::max(lh, 0.72f * R + 0.55f * R);
    for (const auto& ln : lines) h += 0.5f * R + draw::paragraph_height(ui, Face::regular, ln.text, inner_w, ls, lh);
    const Rect rc{x, y, w, h};
    const Id id = Id("job") / App::job_key(j);
    // Opened on this job (a game's dump, from Images or Files).
    if (!app.focus_job.empty() && app.focus_job == App::job_key(j)) {
        ui.set_focus(id);
        app.focus_job.clear();
    }
    if (ui.visible(rc)) {
        l.bordered_rect(rc, radius, panel, 1.0f, line);
        // Head: icon, title, sub, destination, state.
        const Rect icon{x + pad_x, y + pad_y, 3.4f * R, 3.4f * R};
        const std::string ikind = kind == "convert" ? s["to"].str() : kind;
        const bool conv = is_conv_kind(ikind);
        const std::uint32_t tex = !game.empty() ? (g && (*g)["icon"].boolean() ? app.game_icon(game) : 0)
                                  : m && m->icon ? app.icon(meta_from, 0) : 0;
        if (!game.empty())
            draw::ico(ui, icon, "game", t("GAME"), tex);
        else
            draw::ico(ui, icon, conv ? ikind : "folder", kind_label(ikind), tex);
        // A dump's line under the title: version and title ID (none for the next game).
        const std::string game_sub =
            game.empty() || next ? std::string()
                                 : join({g && !(*g)["version"].str().empty() ? "v" + version_text((*g)["version"].str()) : "", game});
        const bool has_sub = game.empty() ? m != nullptr : !game_sub.empty();
        const std::string st_label = t(state);
        const float sw = draw::state_pill(ui, state, st_label, x + w - pad_x, icon.cy());
        const float tx = icon.x + icon.w + 1.0f * R, tw = x + w - pad_x - sw - 0.75f * R - tx;
        const float ts = 1.05f * R, ss = 0.85f * R;
        float ty = icon.cy() - (ts * 1.45f + (has_sub ? ss * 1.45f : 0) + ss * 1.45f) * 0.5f;
        draw::text_fit(ui, Face::semibold, m && !m->title.empty() ? m->title : title, tx,
                       draw::baseline(ty + ts * 0.725f, ts), ts, tw, style::text);
        ty += ts * 1.45f;
        if (!game_sub.empty()) {
            draw::text_fit(ui, Face::regular, game_sub, tx, draw::baseline(ty + ss * 0.725f, ss), ss, tw, style::text.with_alpha(0.8f));
            ty += ss * 1.45f;
        } else if (m) {
            const std::string sub = join({base(input), m->version.empty() ? "" : "v" + version_text(m->version), m->title_id});
            float px = draw::platform_pill(ui, m->platform, tx, ty + ss * 0.725f);
            if (px) px += 0.45f * R;
            draw::text_fit(ui, Face::regular, sub, tx + px, draw::baseline(ty + ss * 0.725f, ss), ss, tw - px,
                           style::text.with_alpha(0.8f));
            ty += ss * 1.45f;
        }
        draw::text_fit(ui, Face::mono, to, tx, draw::baseline(ty + ss * 0.725f, ss * 0.95f), ss * 0.95f, tw, muted);
        float yy = y + pad_y + 3.4f * R;
        if (bar) {
            yy += 0.8f * R;
            const Rect pb{x + pad_x, yy, inner_w, 0.55f * R};
            if (state == "running")
                draw::progress(ui, pb, static_cast<float>(bar_pct / 100.0), total <= 0 && !has_overall);
            else
                draw::progress(ui, pb, static_cast<float>(cp_pct(j)) / 100.0f, false, 0.5f);
            yy += 0.55f * R;
        }
        if (show_steps) {
            yy += 0.6f * R;
            yy += steps_line(ui, j, x + pad_x, yy, inner_w, true);
        }
        yy += 0.6f * R;
        const float row_h = std::max(lh, 0.72f * R + 0.55f * R);
        // Tags on the right, the detail on the left.
        float right = x + w - pad_x;
        for (auto it = tags.rbegin(); it != tags.rend(); ++it) right -= draw::tag(ui, *it, right, yy + row_h * 0.5f) + 0.35f * R;
        draw::text_fit(ui, Face::regular, detail, x + pad_x, draw::baseline(yy + row_h * 0.5f, ls), ls,
                       right - x - pad_x - 0.5f * R, muted);
        yy += row_h;
        for (const auto& ln : lines) {
            yy += 0.5f * R;
            yy += draw::paragraph(ui, Face::regular, ln.text, x + pad_x, yy, inner_w, ls, lh, ln.color);
        }
    }
    if (ui.item(id, rc, 0, radius)) app.job_actions(j);
    return h;
}

}  // namespace

void QueuePage::build(Ui& ui, App& app, const Rect& area) {
    const float w = std::min(62.0f * R, area.w - 2.0f * page_pad);
    const float x = area.x + (area.w - w) * 0.5f;
    ui.begin_zone("queue", false, false);
    const float offset = ui.begin_scroll("queue", {area.x, area.y, area.w, area.h});
    float y = area.y - offset;
    auto empty_card = [&](const std::string& text) {
        const float h = 6.0f * R + 1.45f * R + 2.0f;
        const Rect r{x, y, w, h};
        if (ui.visible(r)) {
            ui.list->bordered_rect(r, radius, panel, 1.0f, line);
            draw::text_fit(ui, Face::regular, text, x + w * 0.5f, draw::baseline(r.cy(), 1.0f * R), 1.0f * R, w - 2.0f * R,
                           muted, Align::center);
        }
        y += h + 0.75f * R;
    };
    auto head = [&](const std::string& text) {
        y += 1.0f * R;
        const float s = 0.72f * R;
        draw::caps(ui, text, x + 0.2f * R, draw::baseline(y + 1.0f * R, s), s, dim);
    };
    head(t("Active"));
    y += 2.0f * R + 0.6f * R;
    bool any = false;
    for (const auto& j : app.jobs) {
        if (!App::is_active(j)) continue;
        any = true;
        y += card(ui, app, j, x, y, w) + 0.75f * R;
    }
    if (!any) empty_card(t("Nothing running. Pick a package in Files and choose Convert."));
    head(t("History"));
    {
        const std::string label = t("Clear history");
        const float bw = draw::button_width(ui, label, true);
        const float bh = 0.7f * R + 0.9f * R * 1.45f + 2.0f;
        if (draw::button(ui, "queue:clear", {x + w - bw - 0.2f * R, y + 1.0f * R - bh * 0.5f, bw, bh}, label,
                         draw::Button::ghost, true))
            app.clear_history();
    }
    y += 2.0f * R + 0.6f * R;
    any = false;
    for (auto it = app.jobs.rbegin(); it != app.jobs.rend(); ++it) {
        if (App::is_active(*it)) continue;
        any = true;
        y += card(ui, app, *it, x, y, w) + 0.75f * R;
    }
    if (!any) empty_card(t("No finished jobs."));
    y += 2.0f * R;
    ui.end_scroll(y + offset - area.y);
    ui.end_zone();
}

}  // namespace alembic::app
