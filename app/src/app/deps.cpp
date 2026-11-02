// SPDX-License-Identifier: GPL-3.0-or-later
// Components (pages/deps/DepsPage.tsx): third-party files the engine
// downloads on request, checked by SHA-256. The AMPR emulators follow their
// projects' latest releases.
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

void DepsPage::load(App& app) {
    app.get("/api/deps", [this, &app](const Reply& r) {
        if (!r.ok()) {
            app.toast(t_msg(r.error()), App::Toast::bad);
            return;
        }
        deps_ = r.json;
        loaded = true;
    });
    app.get("/api/deps/status", [this](const Reply& r) { status_ = r.ok() ? r.json : JsonValue(); });
}

void DepsPage::build(Ui& ui, App& app, const Rect& area) {
    if (!loaded && deps_.is_null()) {
        loaded = true;  // asked once per opening (show_tab asks again)
        load(app);
    }
    const float w = std::min(62.0f * R, area.w - 2.0f * page_pad);
    const float x = area.x + (area.w - w) * 0.5f;
    ui.begin_zone("deps", false, false);
    const float offset = ui.begin_scroll("deps", area);
    float y = area.y - offset + 0.3f * R;
    const bool follows = status_.is_object();
    y += draw::paragraph(ui, Face::regular,
                         follows ? t("Files from other projects that Alembic doesn't ship. Each one is downloaded from the source "
                                     "shown and kept only if its SHA-256 matches. The AMPR emulators follow their projects' latest releases.")
                                 : t("Files from other projects that Alembic doesn't ship. Each one is downloaded from the source "
                                     "shown and kept only if its SHA-256 matches."),
                         x + 0.2f * R, y, w - 0.4f * R, 1.0f * R, 1.45f * R, muted);
    y += 1.0f * R;
    if (follows) {
        // While the engine asks GitHub, look again every two seconds.
        const bool checking = status_["checking"].boolean();
        if (checking && ui.time > poll_at_) {
            poll_at_ = ui.time + 2.0f;
            load(app);
            app.load_info();
        }
        bool on = status_["auto_update"].boolean(true), changed = false;
        y += switch_row(ui, "deps:auto", x, y, w, t("Update automatically"),
                        t("When a newer AMPR emulator is out, the engine downloads it at start. Games keep the one they got "
                          "until you add it again."),
                        &on, &changed);
        if (changed) {
            JsonValue body = JsonValue::object();
            body.set("on", on);
            app.post("/api/deps/auto_update", body, [this, &app](const Reply& r) {
                if (r.ok()) status_ = r.json;
                else app.toast(t_msg(r.error()), App::Toast::bad);
            });
        }
        y += 0.5f * R;
        const float bh = 0.7f * R + 0.9f * R * 1.45f + 2.0f;
        const std::string label = t("Check for updates");
        const float bw = draw::button_width(ui, label, true);
        if (draw::button(ui, "deps:check", {x + w - bw, y, bw, bh}, label, draw::Button::normal, true, checking || !busy_.empty()))
            app.post("/api/deps/check", JsonValue::object(), [this, &app](const Reply& r) {
                if (r.ok()) status_ = r.json;
                else app.toast(t_msg(r.error()), App::Toast::bad);
            });
        if (checking)
            draw::text(ui, Face::regular, t("Checking\xE2\x80\xA6"), x + w - bw - 0.5f * R, draw::baseline(y + bh * 0.5f, 1.0f * R),
                       1.0f * R, muted, Align::right);
        y += bh + 1.0f * R;
    }
    for (const auto& d : deps_.items()) {
        const std::string id = d["id"].str(), state = d["state"].str();
        std::string pill_state = "queued", pill = state;
        if (state == "installed") {
            pill_state = "done";
            pill = "installed";
        } else if (state == "missing") {
            pill = "not installed";
        } else if (state == "partial") {
            pill_state = "failed";
            pill = "incomplete";
        } else if (state == "modified") {
            pill_state = "failed";
            pill = "changed";
        }
        const float pad_x = 1.2f * R, pad_y = 1.0f * R;
        const float iw = w - 2.0f * pad_x;
        std::string files;
        for (const auto& f : d["files"].items()) files += (files.empty() ? "" : "\n") + f["path"].str();
        std::vector<Fact> rows = {{t("Source"), d["source"].str(), ""}};
        const std::string version = d["version"].str(), latest = d["latest"].str(), check_error = d["check_error"].str();
        if (d.has("follows") && (!version.empty() || !latest.empty() || !check_error.empty())) {
            std::string v = version.empty() ? t("not downloaded") : version;
            if (d["update"].boolean()) v += " \xC2\xB7 " + t("{version} is out", {{"version", latest}});
            else if (!version.empty() && version == latest) v += " \xC2\xB7 " + t("the latest");
            if (!check_error.empty()) v += "\n" + t("Couldn't check for updates: {error}", {{"error", t_msg(check_error)}});
            rows.push_back({t("Version"), v, ""});
        }
        rows.push_back({t("License"), d["license"].str(), ""});
        rows.push_back({t("Files"), files, "mono"});
        // Measure: head, purpose, facts, buttons.
        const float purpose_h = draw::paragraph_height(ui, Face::regular, t(d["purpose"].str()), iw, 1.0f * R, 1.45f * R);
        DrawList scratch;
        DrawList* real = ui.list;
        ui.list = &scratch;
        const float facts_h = facts(ui, 0, 0, iw, rows);
        ui.list = real;
        const float bh = 0.7f * R + 0.9f * R * 1.45f + 2.0f;
        const float h = pad_y * 2.0f + 1.05f * R * 1.45f + 0.5f * R + purpose_h + 0.8f * R + facts_h + 0.8f * R + bh;
        const Rect card{x, y, w, h};
        if (ui.visible(card)) {
            ui.list->bordered_rect(card, radius, panel, 1.0f, line);
            const float hy = y + pad_y + 1.05f * R * 0.725f;
            float sw = draw::state_pill(ui, pill_state, t(pill), x + w - pad_x, hy);
            if (d["in_use"].boolean()) sw += 0.75f * R + draw::state_pill(ui, "running", t("in use"), x + w - pad_x - sw - 0.75f * R, hy);
            draw::text_fit(ui, Face::semibold, t(d["label"].str()), x + pad_x, draw::baseline(hy, 1.05f * R), 1.05f * R,
                           iw - sw - 1.0f * R, style::text);
            float yy = y + pad_y + 1.05f * R * 1.45f + 0.5f * R;
            yy += draw::paragraph(ui, Face::regular, t(d["purpose"].str()), x + pad_x, yy, iw, 1.0f * R, 1.45f * R,
                                  style::text);
            yy += 0.8f * R;
            facts(ui, x + pad_x, yy, iw, rows);
        }
        // Buttons, right-aligned, with what is happening on their left.
        float right = x + w - pad_x;
        const float by = y + h - pad_y - bh;
        const bool busy = !busy_.empty();
        auto run = [this, &app, d](const std::string& action) {
            const std::string did = d["id"].str();
            busy_ = did + ":" + action;
            app.post("/api/deps/" + did + "/" + action, JsonValue::object(), [this, &app, d, action](const Reply& r) {
                const std::string name = t(d["label"].str());
                if (r.ok() && action == "install" && !r.json["version"].str().empty())
                    app.toast(t("{name} {version} installed", {{"name", name}, {"version", r.json["version"].str()}}), App::Toast::ok);
                else if (r.ok())
                    app.toast(t(action == "install" ? "{name} installed"
                                : action == "use"   ? "{name} is now in use"
                                                    : "{name} removed",
                                {{"name", name}}),
                              App::Toast::ok);
                else
                    app.toast(t_msg(r.error()), App::Toast::bad);
                app.load_info();
                busy_.clear();
                load(app);
            }, 0);
        };
        if (state != "missing") {
            const std::string label = t("Remove");
            const float bw = draw::button_width(ui, label, true);
            if (draw::button(ui, Id("dep:rm") / id, {right - bw, by, bw, bh}, label, draw::Button::normal, true, busy))
                run("remove");
            right -= bw + 0.5f * R;
        }
        if (state != "installed") {
            const std::string label = state == "missing" ? t("Download ({size})", {{"size", bytes(d["bytes"].number())}})
                                                         : t("Download again");
            const float bw = draw::button_width(ui, label, true);
            if (draw::button(ui, Id("dep:get") / id, {right - bw, by, bw, bh}, label, draw::Button::primary, true, busy))
                run("install");
            right -= bw + 0.5f * R;
        }
        if (!d["group"].str().empty() && state == "installed" && !d["in_use"].boolean()) {
            const std::string label = t("Use this one");
            const float bw = draw::button_width(ui, label, true);
            if (draw::button(ui, Id("dep:use") / id, {right - bw, by, bw, bh}, label, draw::Button::normal, true, busy))
                run("use");
            right -= bw + 0.5f * R;
        }
        if (d["update"].boolean() && state == "installed") {
            const std::string label = t("Update to {version}", {{"version", latest}});
            const float bw = draw::button_width(ui, label, true);
            if (draw::button(ui, Id("dep:update") / id, {right - bw, by, bw, bh}, label, draw::Button::primary, true, busy))
                run("install");
            right -= bw + 0.5f * R;
        }
        if (starts_with(busy_, id + ":") && busy_ != id + ":use") {
            const bool installing = busy_.size() > 7 && busy_.substr(busy_.size() - 7) == "install";
            draw::text(ui, Face::regular, installing ? t("Downloading\xE2\x80\xA6") : t("Removing\xE2\x80\xA6"), right,
                       draw::baseline(by + bh * 0.5f, 1.0f * R), 1.0f * R, muted, Align::right);
        }
        y += h + 0.75f * R;
    }
    y += 2.0f * R;
    ui.end_scroll(y + offset - area.y);
    ui.end_zone();
}

}  // namespace alembic::app
