// SPDX-License-Identifier: GPL-3.0-or-later
// Settings (pages/settings/*): the console shortcut, the app, Places, other
// engines, FTP servers, the engine itself, updates and the cache.
#include "app/dialogs.hpp"
#include "app/pages.hpp"

#include <algorithm>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

namespace {

constexpr float kSmallH = 0.7f * R + 0.9f * R * 1.45f + 2.0f;

// A card's frame; the content is drawn first, so the frame is laid under it
// with the height the content had last frame.
struct Card {
    float x, y, w, pad_x = 1.2f * R, pad_y = 1.0f * R;
    float inner_x() const { return x + pad_x; }
    float inner_w() const { return w - 2.0f * pad_x; }
};

class CardHeights {
public:
    float& operator[](const std::string& k) { return h_[k]; }

private:
    std::map<std::string, float> h_;
};
CardHeights& heights() {
    static CardHeights h;
    return h;
}

// Opens a card at y: draws its frame (last frame's height) and returns the
// content's top.
float card_begin(Ui& ui, const std::string& key, float x, float y, float w) {
    const float h = heights()[key];
    if (h > 0 && ui.visible({x, y, w, h})) ui.list->bordered_rect({x, y, w, h}, radius, panel, 1.0f, line);
    return y + 1.0f * R;
}
float card_end(const std::string& key, float top, float y) {
    heights()[key] = y + 1.0f * R - (top - 1.0f * R);
    return y + 1.0f * R + 0.75f * R;
}

// A row in a card (.ftprow): icon, name, mono sub, note, and buttons from the right.
struct RowButton {
    std::string key, label;
    draw::Button kind;
    std::function<void()> act;
    bool disabled = false;
};
float ftprow(Ui& ui, float x, float y, float w, draw::Icon icon, const std::string& name, const std::string& sub,
             const std::string& note, Color note_color, const std::vector<RowButton>& buttons, bool first) {
    const float pad = 0.6f * R;
    float th = 1.0f * R * 1.45f + 0.85f * R * 1.45f;
    if (!note.empty()) th += 0.85f * R * 1.45f;
    const float h = std::max(th, kSmallH) + 2.0f * pad;
    if (!first) draw::hline(ui, x, y, w, rgb(0xffffff, 0.06f));
    const float cy = y + h * 0.5f;
    draw::icon(ui, icon, {x, cy - 0.65f * R, 1.3f * R, 1.3f * R}, style::text.with_alpha(0.85f));
    float right = x + w;
    for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
        const float bw = draw::button_width(ui, it->label, true);
        if (draw::button(ui, Id("row") / it->key, {right - bw, cy - kSmallH * 0.5f, bw, kSmallH}, it->label, it->kind,
                         true, it->disabled) &&
            it->act)
            it->act();
        right -= bw + 0.8f * R;
    }
    const float tx = x + 1.3f * R + 0.8f * R, tw = right - tx;
    float ty = cy - th * 0.5f;
    draw::text_fit(ui, Face::regular, name, tx, draw::baseline(ty + 0.725f * R, 1.0f * R), 1.0f * R, tw, style::text);
    ty += 1.45f * R;
    draw::text_fit(ui, Face::mono, sub, tx, draw::baseline(ty + 0.85f * 0.725f * R, 0.8f * R), 0.8f * R, tw, muted);
    ty += 0.85f * R * 1.45f;
    if (!note.empty())
        draw::text_fit(ui, Face::regular, note, tx, draw::baseline(ty + 0.85f * 0.725f * R, 0.85f * R), 0.85f * R, tw,
                       note_color);
    return h;
}

// The buttons row of a card (.btns): right-aligned.
float btns(Ui& ui, float x, float y, float w, const std::vector<RowButton>& buttons) {
    float right = x + w;
    y += 0.8f * R;
    for (auto it = buttons.rbegin(); it != buttons.rend(); ++it) {
        const float bw = draw::button_width(ui, it->label, true);
        if (draw::button(ui, Id("btn") / it->key, {right - bw, y, bw, kSmallH}, it->label, it->kind, true, it->disabled) &&
            it->act)
            it->act();
        right -= bw + 0.5f * R;
    }
    return 0.8f * R + kSmallH;
}

// ---- forms ----

class FtpForm : public Overlay {
public:
    std::vector<Hint> hints(App&) override { return {{Glyph::cross, t("Choose")}, {Glyph::circle, t("Cancel")}}; }
    JsonValue server;  // null: a new one
    std::string name, host, port = "21", user = "anonymous", pass;
    std::string msg;
    bool err = false, busy = false;
    Sheet sheet;

    void build(Ui& ui, App& app) override {
        const bool edit = server.is_object();
        sheet.begin(ui, edit ? t("Edit {name}", {{"name", server["name"].str()}}) : t("Add an FTP server"), "",
                    Sheet::Size::narrow, true);
        float y = sheet.content_top() - 1.1f * R;
        field_row(ui, app, "ftp:name", sheet.x(), &y, sheet.w(), t("Name"), &name, t("My PC"));
        field_row(ui, app, "ftp:host", sheet.x(), &y, sheet.w(), t("Address"), &host, "192.168.1.10");
        field_row(ui, app, "ftp:port", sheet.x(), &y, sheet.w(), t("Port"), &port, "", true);
        field_row(ui, app, "ftp:user", sheet.x(), &y, sheet.w(), t("User"), &user, "");
        field_row(ui, app, "ftp:pass", sheet.x(), &y, sheet.w(), t("Password"), &pass, edit ? t("Unchanged") : "", false,
                  true);
        sheet.end_body(ui, y);
        if (sheet.button(ui, "ftp:save", edit ? t("Save") : t("Add"), draw::Button::primary, busy)) save(app);
        if (sheet.button(ui, "ftp:cancel", t("Cancel"), draw::Button::normal)) app.close(this);
        sheet.message(ui, msg, err);
        sheet.end(ui);
    }
    void save(App& app) {
        auto trim = [](std::string s) {
            while (!s.empty() && s.back() == ' ') s.pop_back();
            while (!s.empty() && s.front() == ' ') s.erase(0, 1);
            return s;
        };
        if (trim(host).empty()) {
            msg = t("Enter the server's address");
            err = true;
            return;
        }
        busy = true;
        msg = t("Connecting\xE2\x80\xA6");
        err = false;
        JsonValue body = JsonValue::object();
        body.set("name", trim(name).empty() ? trim(host) : trim(name));
        body.set("host", trim(host));
        const int p = std::atoi(port.c_str());
        body.set("port", p > 0 ? p : 21);
        body.set("user", trim(user).empty() ? std::string("anonymous") : trim(user));
        // Editing: an empty password keeps the saved one.
        if (!server.is_object() || !pass.empty()) body.set("password", pass);
        const int k = key;
        const bool edit = server.is_object();
        auto done = [&app, k, edit, this](const Reply& r) {
            if (!app.overlay_alive(k)) return;
            if (!r.ok()) {
                msg = t_msg(r.error());
                err = true;
                busy = false;
                return;
            }
            app.toast(edit ? t("FTP server saved") : t("FTP server added"), App::Toast::ok);
            app.close(this);
            app.load_info();
        };
        if (edit)
            app.api("PUT", "/api/ftp/" + url_encode(server["id"].str()), body, done, 0, 20000);
        else
            app.api("POST", "/api/ftp", body, done, 0, 20000);
    }
};

class AddEngineForm : public Overlay {
public:
    std::vector<Hint> hints(App&) override { return {{Glyph::cross, t("Choose")}, {Glyph::circle, t("Cancel")}}; }
    std::string host, port = "5940", msg;
    bool err = false, busy = false;
    Sheet sheet;
    void build(Ui& ui, App& app) override {
        sheet.begin(ui, t("Add an engine"), "", Sheet::Size::narrow, true);
        float y = sheet.content_top() - 1.1f * R;
        field_row(ui, app, "peer:host", sheet.x(), &y, sheet.w(), t("Address"), &host, "192.168.1.10");
        field_row(ui, app, "peer:port", sheet.x(), &y, sheet.w(), t("Port"), &port, "", true);
        sheet.end_body(ui, y);
        if (sheet.button(ui, "peer:add", t("Add"), draw::Button::primary, busy)) {
            if (host.empty()) {
                msg = t("Enter the engine's address");
                err = true;
            } else {
                busy = true;
                msg = t("Connecting\xE2\x80\xA6");
                err = false;
                JsonValue body = JsonValue::object();
                body.set("address", host);
                const int p = std::atoi(port.c_str());
                body.set("port", p > 0 ? p : 5940);
                const int k = key;
                app.api("POST", "/api/peers", body, [&app, k, this](const Reply& r) {
                    if (!app.overlay_alive(k)) return;
                    if (!r.ok()) {
                        msg = t_msg(r.error());
                        err = true;
                        busy = false;
                        return;
                    }
                    app.peers = r.json;
                    app.toast(t("Engine added"), App::Toast::ok);
                    app.close(this);
                }, 0, 20000);
            }
        }
        if (sheet.button(ui, "peer:cancel", t("Cancel"), draw::Button::normal)) app.close(this);
        sheet.message(ui, msg, err);
        sheet.end(ui);
    }
};

class PairCodeForm : public Overlay {
public:
    std::vector<Hint> hints(App&) override { return {{Glyph::cross, t("Choose")}, {Glyph::circle, t("Cancel")}}; }
    std::string id, name, code, msg;
    bool err = false, busy = false, done = false;
    Sheet sheet;
    void build(Ui& ui, App& app) override {
        sheet.begin(ui, t("Pair with {name}", {{"name", name}}), "", Sheet::Size::narrow, true);
        float y = sheet.content_top();
        y += draw::paragraph(ui, Face::regular,
                             t("Enter the code {name} shows. On a PS5 it is a notification on the screen; on a computer, "
                               "it is in the engine's window, or in its page opened on that computer.",
                               {{"name", name}}),
                             sheet.x(), y, sheet.w(), 1.0f * R, 1.45f * R, style::text);
        y -= 0.4f * R;
        field_row(ui, app, "pair:code", sheet.x(), &y, sheet.w(), t("Code"), &code, "123 456", true);
        sheet.end_body(ui, y);
        if (sheet.button(ui, "pair:ok", t("Pair"), draw::Button::primary, busy)) pair(app);
        if (sheet.button(ui, "pair:cancel", t("Cancel"), draw::Button::normal)) back(app);
        sheet.message(ui, msg, err);
        sheet.end(ui);
    }
    void pair(App& app) {
        std::string digits;
        for (char c : code)
            if (c >= '0' && c <= '9') digits += c;
        if (digits.size() != 6) {
            msg = t("Enter the six-digit code");
            err = true;
            return;
        }
        busy = true;
        msg = t("Connecting\xE2\x80\xA6");
        err = false;
        JsonValue body = JsonValue::object();
        body.set("code", code);
        const int k = key;
        app.api("POST", "/api/peers/" + url_encode(id) + "/pair/confirm", body, [&app, k, this](const Reply& r) {
            if (!app.overlay_alive(k)) return;
            if (r.ok()) {
                app.peers = r.json;
                done = true;
                app.close(this);
                app.toast(t("Paired with {name}", {{"name", name}}), App::Toast::ok);
                return;
            }
            msg = t_msg(r.error());
            err = true;
            busy = false;
            // Too many wrong codes end the attempt there: close this too.
            app.get("/api/peers", [&app, k, this](const Reply& pr) {
                if (!pr.ok() || !app.overlay_alive(k)) return;
                app.peers = pr.json;
                bool waiting = false;
                for (const auto& e : app.peers["engines"].items())
                    waiting = waiting || (e["id"].str() == id && e["waiting_for_code"].boolean());
                if (!waiting) {
                    done = true;
                    app.close(this);
                }
            });
        }, 0, 20000);
    }
    void back(App& app) override {
        if (!done)
            app.api("DELETE", "/api/peers/" + url_encode(id) + "/pair/session", JsonValue(), [&app](const Reply& r) {
                if (r.ok()) app.peers = r.json;
            });
        app.close(this);
    }
};

void code_dialog(App& app, const JsonValue& e) {
    auto f = std::make_unique<PairCodeForm>();
    f->id = e["id"].str();
    f->name = e["name"].str();
    app.open(std::move(f));
}

}  // namespace

void SettingsPage::opened(App& app) {
    load_update(app);
    app.load_peers();
}

void SettingsPage::load_update(App& app) {
    app.get("/api/update", [this](const Reply& r) {
        if (r.ok()) {
            update_ = r.json;
            update_loaded_ = true;
        }
    });
}

void SettingsPage::build(Ui& ui, App& app, const Rect& area) {
    const JsonValue& i = app.info;
    // Other engines come and go: asked again every few seconds.
    peers_timer_ += ui.dt;
    if (peers_timer_ > 4.0f) {
        peers_timer_ = 0.0f;
        if (app.peers["enabled"].boolean() || !app.peers["engines"].items().empty()) app.load_peers();
    }
    if (!update_loaded_ && update_.is_null()) {
        update_loaded_ = true;
        load_update(app);
    }
    // While an update works, ask again every second.
    if (update_.is_object() && update_["state"].str("idle") != "idle" && wait_for_.empty()) {
        update_timer_ += ui.dt;
        if (update_timer_ > 1.0f) {
            update_timer_ = 0.0f;
            load_update(app);
        }
    }
    // After the hand-off: the new engine answers with the new version.
    if (!wait_for_.empty()) {
        wait_timer_ += ui.dt;
        wait_total_ += ui.dt;
        if (wait_timer_ > 2.0f) {
            wait_timer_ = 0.0f;
            app.get("/api/info", [this, &app](const Reply& r) {
                if (r.ok() && r.json["version"].str() == wait_for_) {
                    wait_for_.clear();
                    app.load_info();
                    load_update(app);
                    app.toast(t("Updated to {version}", {{"version", r.json["version"].str()}}), App::Toast::ok);
                }
            });
        }
    }

    const float w = std::min(62.0f * R, area.w - 2.0f * page_pad);
    const float x = area.x + (area.w - w) * 0.5f;
    ui.begin_zone("settings", false, false);
    const float offset = ui.begin_scroll("settings", area);
    float y = area.y - offset;
    auto head = [&](const std::string& text) {
        y += 1.0f * R;
        draw::caps(ui, text, x + 0.2f * R, draw::baseline(y + 0.36f * R + 0.3f * R, 0.72f * R), 0.72f * R, dim);
        y += 0.72f * R * 1.45f + 0.6f * R;
    };

    // ---- Console: the Media shortcut ----
    head(t("Console"));
    {
        const JsonValue& l = i["launcher"];
        if (!(l.is_object() && l["supported"].boolean())) {
            bool pressed = false;
            y += link_row(ui, "s:launcher", x, y, w, t("Shortcut in Media"),
                          l.is_object() && app.is_ps5() ? t_status(l["status"].str("not available"))
                                                        : t("Only when the engine runs on a PS5."),
                          "", &pressed);
        } else {
            bool on = l["installed"].boolean(), changed = false;
            y += switch_row(ui, "s:launcher", x, y, w, t("Shortcut in Media"),
                            t("Adds Alembic to the Media tab of the home screen; it opens this page in the console's browser"),
                            &on, &changed);
            if (changed) {
                JsonValue body = JsonValue::object();
                body.set("enabled", on);
                app.post("/api/launcher", body, app.or_toast([&app, on](const Reply& r) {
                    app.info = r.json;
                    app.toast(on ? t("Added to Media on the home screen") : t("Shortcut removed"), App::Toast::ok);
                }));
            }
            const float top = card_begin(ui, "launcher", x, y, w);
            float yy = top;
            const std::string status = l["status"].str();
            const bool failed = status.find("failed") != std::string::npos || status.find("not available") != std::string::npos;
            yy += facts(ui, x + 1.2f * R, yy, w - 2.4f * R,
                        {{t("Status"), t_status(status), failed ? "err" : ""}, {t("Title ID"), l["title_id"].str(), "mono"}});
            yy += btns(ui, x + 1.2f * R, yy, w - 2.4f * R,
                       {{"again", t("Register again"), draw::Button::normal,
                         [this, &app] {
                             launcher_busy_ = true;
                             JsonValue body = JsonValue::object();
                             body.set("enabled", true);
                             body.set("again", true);
                             app.post("/api/launcher", body, [this, &app](const Reply& r) {
                                 if (r.ok()) {
                                     app.info = r.json;
                                     app.toast(t("Shortcut registered again; check Media on the home screen"), App::Toast::ok);
                                 } else {
                                     app.toast(t_msg(r.error()), App::Toast::bad);
                                 }
                                 launcher_busy_ = false;
                                 app.load_info();
                             });
                         },
                         launcher_busy_}});
            y = card_end("launcher", top, yy);
        }
    }

    // ---- The app ----
    head(t("App"));
    {
        const auto& langs = languages();
        std::vector<std::string> labels;
        int index = 0;
        for (std::size_t k = 0; k < langs.size(); ++k) {
            labels.push_back(langs[k].second);
            if (langs[k].first == language()) index = static_cast<int>(k);
        }
        bool changed = false;
        const std::string hint = language_drawable(language()) ? "" : t("The app shows English for this language for now");
        y += choice_row(ui, "s:lang", x, y, w, t("Language"), hint, labels, &index, &changed);
        if (changed) {
            const std::string code = langs[static_cast<std::size_t>(index)].first;
            set_language(code);
            JsonValue body = JsonValue::object();
            body.set("lang", code);
            app.post("/api/lang", body, nullptr);
            app.info.set("lang", code);
        }
        if (app.platform.quit) {
            bool pressed = false;
            y += link_row(ui, "s:close", x, y, w, t("Close Alembic"),
                          t("The engine keeps running and finishes its jobs; the page and the app can be opened again"),
                          "", &pressed);
            if (pressed) app.platform.quit();
        }
    }

    // ---- Places ----
    head(t("Places"));
    {
        const float top = card_begin(ui, "places", x, y, w);
        float yy = top;
        bool any = false, first = true;
        for (const auto& l : app.locations.items()) {
            if (!l["custom"].boolean()) continue;
            any = true;
            const std::string p = l["path"].str();
            yy += ftprow(ui, x + 1.2f * R, yy, w - 2.4f * R, draw::Icon::place,
                         l["place"].str().empty() ? l["label"].str() : l["place"].str(), app.shown_path(p), "", muted,
                         {{"place:rm:" + p, t("Remove"), draw::Button::danger, [&app, p] { app.set_place(p, false); }}},
                         first);
            first = false;
        }
        if (!any)
            yy += draw::paragraph(ui, Face::regular,
                                  t("No places yet: in Files, open a folder's Options and choose Add to Places. They show "
                                    "here and on paired engines."),
                                  x + 1.2f * R, yy, w - 2.4f * R, 1.0f * R, 1.45f * R, muted);
        y = card_end("places", top, yy);
    }

    // ---- Other engines ----
    head(t("Other engines"));
    {
        const JsonValue& p = app.peers;
        bool on = p["enabled"].boolean(), changed = false;
        y += switch_row(ui, "s:discovery", x, y, w, t("Find engines on this network"),
                        t("Announces this engine on the local network every few seconds and lists the other Alembic "
                          "engines that do the same. Off by default."),
                        &on, &changed);
        if (changed) {
            JsonValue body = JsonValue::object();
            body.set("enabled", on);
            app.post("/api/peers/discovery", body, app.or_toast([&app](const Reply& r) { app.peers = r.json; }));
        }
        const float top = card_begin(ui, "engines", x, y, w);
        float yy = top;
        const float ix = x + 1.2f * R, iw = w - 2.4f * R;
        if (on && p.has("listening") && !p["listening"].boolean())
            yy += draw::paragraph(ui, Face::regular,
                                  t("The discovery port could not be opened; another program may be using it."), ix, yy,
                                  iw, 1.0f * R, 1.45f * R, bad) +
                  0.5f * R;
        if (p["engines"].items().empty())
            yy += draw::paragraph(ui, Face::regular,
                                  on ? t("No other engines found yet.")
                                     : t("Switch on finding engines, or add one by its address."),
                                  ix, yy, iw, 1.0f * R, 1.45f * R, muted) +
                  0.5f * R;
        bool first = true;
        const long long ours = i["protocol"].integer(1);
        for (const auto& e : p["engines"].items()) {
            const std::string id = e["id"].str(), name = e["name"].str();
            std::string sub = e["os"].str().empty() ? e["platform"].str() : e["os"].str();
            sub += " \xC2\xB7 " + e["address"].str() + ":" + std::to_string(e["port"].integer());
            if (!e["version"].str().empty()) sub += " \xC2\xB7 v" + e["version"].str();
            const long long theirs = e["protocol"].integer(1);
            const bool mismatch = theirs != ours;
            std::string note;
            Color nc = muted;
            if (mismatch) {
                note = theirs < ours ? t("Older Alembic: update it to use it with this engine")
                                     : t("Newer Alembic: update this engine to use them together");
                nc = bad;
            } else if (e["manual"].boolean() && !e["reachable"].boolean()) {
                note = t("Not reachable: {why}", {{"why", t_msg(e["problem"].str())}});
                nc = bad;
            } else if (e["paired"].boolean()) {
                note = t("Paired");
                nc = ok;
            } else if (e["manual"].boolean()) {
                note = t("Added by address");
            }
            std::vector<RowButton> b;
            if (e["paired"].boolean()) {
                b.push_back({"check:" + id, t("Check"), draw::Button::normal, [&app, id] {
                                 app.get("/api/peers/" + url_encode(id) + "/pair/check", [&app](const Reply& r) {
                                     if (r.ok())
                                         app.toast(t("The pairing works"), App::Toast::ok);
                                     else
                                         app.toast(t_msg(r.error()), App::Toast::bad);
                                 });
                             }});
                b.push_back({"unpair:" + id, t("Unpair"), draw::Button::danger, [&app, id, name] {
                                 ask(app, t("Unpair {name}?", {{"name", name}}),
                                     text_body(t("Both engines forget their shared key; pair them again to use them together.")),
                                     t("Unpair"), true, [&app, id] {
                                         app.api("DELETE", "/api/peers/" + url_encode(id) + "/pair", JsonValue(),
                                                 app.or_toast([&app](const Reply& r) {
                                                     app.peers = r.json;
                                                     app.toast(t("Unpaired"), App::Toast::ok);
                                                 }));
                                     });
                             }});
            } else {
                if (!mismatch) {
                    if (e["waiting_for_code"].boolean())
                        b.push_back({"code:" + id, t("Enter the code"), draw::Button::normal,
                                     [&app, e] { code_dialog(app, e); }});
                    else
                        b.push_back({"pair:" + id, t("Pair"), draw::Button::primary, [&app, e, id] {
                                         app.toast(t("Connecting\xE2\x80\xA6"), App::Toast::ok);
                                         app.api("POST", "/api/peers/" + url_encode(id) + "/pair", JsonValue::object(),
                                                 app.or_toast([&app, e](const Reply& r) {
                                                     app.peers = r.json;
                                                     code_dialog(app, e);
                                                 }),
                                                 0, 20000);
                                     }});
                }
                if (e["manual"].boolean())
                    b.push_back({"rm:" + id, t("Remove"), draw::Button::danger, [&app, id, name] {
                                     ask(app, t("Remove {name}?", {{"name", name}}),
                                         text_body(t("Nothing changes on that engine; it can be added again.")), t("Remove"),
                                         true, [&app, id] {
                                             app.api("DELETE", "/api/peers/" + url_encode(id), JsonValue(),
                                                     app.or_toast([&app](const Reply& r) {
                                                         app.peers = r.json;
                                                         app.toast(t("Engine removed"), App::Toast::ok);
                                                     }));
                                         });
                                 }});
            }
            yy += ftprow(ui, ix, yy, iw, draw::Icon::server, name, sub, note, nc, b, first);
            first = false;
        }
        yy += btns(ui, ix, yy, iw,
                   {{"peer:add", t("Add an engine by address\xE2\x80\xA6"), draw::Button::normal,
                     [&app] { app.open(std::make_unique<AddEngineForm>()); }}});
        y = card_end("engines", top, yy);
    }

    // ---- FTP servers ----
    head(t("FTP servers"));
    {
        const float top = card_begin(ui, "ftp", x, y, w);
        float yy = top;
        const float ix = x + 1.2f * R, iw = w - 2.4f * R;
        yy += draw::paragraph(ui, Face::regular,
                              t("Games on another computer's FTP server can be converted from there, and exFAT, FFPKG, "
                                "FFPFS and folder outputs saved there. FFPFSC and PKG outputs must be saved on the console "
                                "first. Plain FTP: the password crosses the network unencrypted and is kept on the console."),
                              ix, yy, iw, 1.0f * R, 1.45f * R, muted) +
              0.5f * R;
        bool first = true;
        for (const auto& s : app.ftp.items()) {
            const std::string id = s["id"].str(), name = s["name"].str();
            JsonValue server = s;
            yy += ftprow(ui, ix, yy, iw, draw::Icon::server, name,
                         s["user"].str() + "@" + s["host"].str() + ":" + std::to_string(s["port"].integer(21)), "", muted,
                         {{"ftp:ed:" + id, t("Edit"), draw::Button::normal,
                           [&app, server] {
                               auto f = std::make_unique<FtpForm>();
                               f->server = server;
                               f->name = server["name"].str();
                               f->host = server["host"].str();
                               f->port = std::to_string(server["port"].integer(21));
                               f->user = server["user"].str();
                               app.open(std::move(f));
                           }},
                          {"ftp:rm:" + id, t("Remove"), draw::Button::danger,
                           [&app, id, name] {
                               ask(app, t("Remove {name}?", {{"name", name}}),
                                   text_body(t("Its files stay on the server; jobs that use it will fail.")), t("Remove"),
                                   true, [&app, id] {
                                       app.api("DELETE", "/api/ftp/" + url_encode(id), JsonValue(), [&app](const Reply& r) {
                                           if (r.ok())
                                               app.toast(t("FTP server removed"), App::Toast::ok);
                                           else
                                               app.toast(t_msg(r.error()), App::Toast::bad);
                                           app.load_info();
                                       });
                                   });
                           }}},
                         first);
            first = false;
        }
        yy += btns(ui, ix, yy, iw,
                   {{"ftp:add", t("Add an FTP server\xE2\x80\xA6"), draw::Button::normal,
                     [&app] { app.open(std::make_unique<FtpForm>()); }}});
        y = card_end("ftp", top, yy);
    }

    // ---- The engine ----
    head(t("Engine"));
    {
        const float top = card_begin(ui, "engine", x, y, w);
        float yy = top;
        const float ix = x + 1.2f * R, iw = w - 2.4f * R;
        std::vector<Fact> rows;
        if (i.is_null()) {
            rows.push_back({t("Engine"), t("not connected"), ""});
        } else {
            rows.push_back({t("Version"), i["version"].str(), ""});
            rows.push_back({t("Running on"), app.is_ps5() ? t("PS5 (payload)") : i["os"].str().empty() ? t("PC") : i["os"].str(), ""});
            if (!i["host_name"].str().empty() && !app.is_ps5()) rows.push_back({t("Computer"), i["host_name"].str(), ""});
            rows.push_back({t("Address"), app.engine.host() + ":" + std::to_string(app.engine.port()), "mono"});
            std::string urls;
            for (const auto& u : i["network_urls"].items()) urls += (urls.empty() ? "" : "\n") + u.str();
            rows.push_back({t("From other devices"),
                            !urls.empty() ? urls : i["open_to_network"].boolean() ? "\xE2\x80\x93" : t("This computer only"), "mono"});
            std::string roots;
            for (const auto& u : i["roots"].items()) roots += (roots.empty() ? "" : "\n") + u.str();
            if (!roots.empty()) rows.push_back({t("Shared folders"), roots, "mono"});
            rows.push_back({t("State folder"), i["state_dir"].str(), "mono"});
            if (!i["cpu_instructions"].str().empty())
                rows.push_back({t("CPU instructions"), i["cpu_instructions"].str() == "none" ? "\xE2\x80\x93" : i["cpu_instructions"].str(), ""});
            const JsonValue& rt = i["lz4_runtime"];
            rows.push_back({t("LZ4 runtime"), rt["present"].boolean() ? (rt["pinned"].boolean() ? "ampr_emu 0.4.2.1" : t("present, not the pinned build"))
                                                                     : t("not downloaded"), ""});
        }
        yy += facts(ui, ix, yy, iw, rows);
        std::vector<RowButton> engine_btns;
        if (!app.not_connected.empty() && app.platform.start_engine) {
            // Stopped (here or elsewhere): the app's own engine starts it again.
            engine_btns.push_back({"s:start", t("Start the engine"), draw::Button::primary, [&app] { app.start_own_engine(); },
                                   app.restarting()});
        } else {
            if (i["can_restart"].boolean() || (app.platform.start_engine && app.is_ps5()))
                engine_btns.push_back({"s:restart", t("Restart the engine"), draw::Button::normal, [&app] {
                                           bool running = false, restarts = false;
                                           for (const auto& j : app.jobs)
                                               if (j["state"].str() == "running") {
                                                   running = true;
                                                   restarts = restarts || !j["keeps_progress"].boolean();
                                               }
                                           std::string body = t("The engine stops and starts again through the ELF loader (port 9021).");
                                           if (restarts)
                                               body += "\n\n!" + t("A running job can't keep its progress; it starts over after the restart.");
                                           else if (running)
                                               body += "\n\n~" + t("The running job continues after the restart.");
                                           ask(app, t("Restart the engine?"), text_body(body), t("Restart"), false,
                                               [&app] { app.restart_engine(); });
                                       },
                                       app.restarting()});
            engine_btns.push_back({"s:quit", t("Stop the engine"), draw::Button::danger, [&app] {
                                       bool running = false;
                                       for (const auto& j : app.jobs) running = running || j["state"].str() == "running";
                                       std::string body = t("The app and the page stop working until the engine is loaded again.");
                                       if (running) body += "\n\n!" + t("A job is running; it will be cancelled.");
                                       ask(app, t("Stop the engine?"), text_body(body), t("Stop"), true, [&app] {
                                           app.post("/api/quit", JsonValue::object(), [&app](const Reply& r) {
                                               if (r.ok())
                                                   app.toast(t("Engine stopped"), App::Toast::ok);
                                               else
                                                   app.toast(t_msg(r.error()), App::Toast::bad);
                                           });
                                       });
                                   }});
        }
        yy += btns(ui, ix, yy, iw, engine_btns);
        y = card_end("engine", top, yy);
    }

    // ---- Updates ----
    if (update_.is_object()) {
        const JsonValue& u = update_;
        head(t("Updates"));
        const bool busy = u["state"].str("idle") != "idle";
        const JsonValue& latest = u["latest"];
        const std::string state = u["state"].str();
        std::string status;
        if (!wait_for_.empty())
            status = t("Starting Alembic {version}\xE2\x80\xA6 This page reloads when it is up.", {{"version", wait_for_}});
        else if (busy)
            status = state == "checking"      ? t("Asking GitHub\xE2\x80\xA6")
                     : state == "searching"   ? t("Looking for the payload files\xE2\x80\xA6")
                     : state == "downloading" ? t("Downloading\xE2\x80\xA6")
                     : state == "replacing"   ? t("Replacing the payload files\xE2\x80\xA6")
                     : state == "starting"    ? t("Starting the new engine\xE2\x80\xA6")
                                              : state;
        else if (!latest.is_object())
            status = u["checked"].integer() ? t("Could not check") : t("Not checked yet");
        else
            status = u["newer"].boolean() ? t("Version {version} is available", {{"version", latest["version"].str()}})
                                          : t("Up to date");
        // The console app's image: replaced when the release's differs.
        const JsonValue& ap = u["app"];
        const std::string av = ap["version"].str().empty() ? "?" : ap["version"].str();
        const bool app_differs = ap.is_object() && ap["image"].boolean() && ap["in_release"].boolean() && !ap["current"].boolean();
        const bool app_on = app_differs && !off_["app"];
        std::string app_text;
        if (ap.is_object())
            app_text = ap["path"].str().empty()        ? t("not found")
                       : !ap["image"].boolean()        ? t("{version}, a folder: not replaced", {{"version", av}})
                       : ap["current"].boolean()       ? t("{version}, up to date", {{"version", av}})
                       : ap["in_release"].boolean()    ? t("{version}; the release's image differs", {{"version", av}})
                                                       : av;
        const float top = card_begin(ui, "updates", x, y, w);
        float yy = top;
        const float ix = x + 1.2f * R, iw = w - 2.4f * R;
        std::vector<Fact> update_rows = {{t("Latest release"), latest.is_object() ? latest["version"].str() : "\xE2\x80\x93", ""},
                                         {t("Status"), status, ""}};
        // This app: what the engine found on the drives, else its own version.
        update_rows.push_back({t("Console app"), ap.is_object() ? app_text : app.platform.version, ""});
        if (ap.is_object() && !ap["path"].str().empty()) update_rows.push_back({t("App image"), ap["path"].str(), "mono"});
        yy += facts(ui, ix, yy, iw, update_rows);
        if (u["app_pending"].boolean())
            yy += 0.5f * R + draw::paragraph(ui, Face::regular,
                                             t("A new app image is waiting: close the app on the console to finish its update."),
                                             ix, yy + 0.5f * R, iw, 0.88f * R, 0.88f * 1.45f * R, warn);
        if (!u["error"].str().empty() && !busy && wait_for_.empty())
            yy += 0.5f * R + draw::paragraph(ui, Face::regular, t_msg(u["error"].str()), ix, yy + 0.5f * R, iw, 0.88f * R,
                                             0.88f * 1.45f * R, bad);
        if (!wait_for_.empty() && wait_total_ > 90.0f)
            yy += 0.5f * R + draw::paragraph(ui, Face::regular,
                                             t("The new engine hasn't answered yet. If it doesn't start, load Alembic "
                                               "again from your loader: its files were already updated."),
                                             ix, yy + 0.5f * R, iw, 0.88f * R, 0.88f * 1.45f * R, warn);
        if (latest.is_object() && u["newer"].boolean() && !latest["notes"].str().empty()) {
            const float ns = 0.85f * R;
            const float nh = std::min(16.0f * R, draw::paragraph_height(ui, Face::regular, latest["notes"].str(), iw - 1.6f * R, ns, ns * 1.45f) + 1.2f * R);
            yy += 0.6f * R;
            ui.list->rounded_rect({ix, yy, iw, nh}, 0.6f * R, bg2);
            ui.list->push_clip({ix, yy, iw, nh});
            draw::paragraph(ui, Face::regular, latest["notes"].str(), ix + 0.8f * R, yy + 0.6f * R, iw - 1.6f * R, ns,
                            ns * 1.45f, muted);
            ui.list->pop_clip();
            yy += nh;
        }
        std::vector<RowButton> update_btns = {{"u:check", t("Check now"), draw::Button::normal, [this, &app] {
                                                   app.post("/api/update/check", JsonValue::object(), [this, &app](const Reply& r) {
                                                       if (r.ok())
                                                           update_ = r.json;
                                                       else
                                                           app.toast(t_msg(r.error()), App::Toast::bad);
                                                   });
                                               },
                                               busy || !wait_for_.empty()}};
        if (app_differs && latest.is_object() && latest["version"].str() == u["current"].str() && !u["app_pending"].boolean())
            update_btns.push_back({"u:app", t("Update the app"), draw::Button::primary, [this, &app] {
                                       app.post("/api/update/app", JsonValue::object(), [this, &app](const Reply& r) {
                                           if (!r.ok()) {
                                               app.toast(t_msg(r.error()), App::Toast::bad);
                                               return;
                                           }
                                           update_ = r.json;
                                           app.toast(t("The new app image is downloaded; it replaces the app once the app is closed."),
                                                     App::Toast::ok);
                                       });
                                   },
                                   busy || !wait_for_.empty()});
        yy += btns(ui, ix, yy, iw, update_btns);
        if (u["newer"].boolean() && !u["can_install"].boolean())
            yy += 0.5f * R + draw::paragraph(ui, Face::regular,
                                             t("On a PC, replace alembic-engine with the new release and start it again."),
                                             ix, yy + 0.5f * R, iw, 1.0f * R, 1.45f * R, muted);
        y = card_end("updates", top, yy);
        if (u["newer"].boolean() && u["can_install"].boolean() && latest.is_object()) {
            head(t("Payload files to update"));
            const float top2 = card_begin(ui, "payloads", x, y, w);
            float y2 = top2;
            if (u["payloads"].items().empty())
                y2 += draw::paragraph(ui, Face::regular,
                                      u["searched"].str() == "no"
                                          ? t("Not searched yet.")
                                          : t("No Alembic payload files in the usual places (Payload Manager, the "
                                              "autoloaders). If your loader keeps it elsewhere, search the drives."),
                                      ix, y2, iw, 1.0f * R, 1.45f * R, muted);
            if (app_differs) {
                bool on = app_on, changed = false;
                y2 += switch_row(ui, Id("u:app-on"), ix, y2, iw, t("Console app"), ap["path"].str(), &on, &changed);
                if (changed) off_["app"] = !on;
            }
            for (const auto& pl : u["payloads"].items()) {
                const std::string path = pl["path"].str();
                bool on = !off_[path];
                std::string hint = pl["loader"].str() == "autoloader" ? t("autoloader") : pl["loader"].str();
                hint += " \xC2\xB7 " + (pl["version"].str().empty() ? t("an older version")
                                                                    : t("version {version}", {{"version", pl["version"].str()}}));
                if (!pl["rename_to"].str().empty()) hint += " \xC2\xB7 " + t("renamed to {name}", {{"name", pl["rename_to"].str()}});
                bool changed = false;
                y2 += switch_row(ui, Id("u:p") / path, ix, y2, iw, path, hint, &on, &changed);
                if (changed) off_[path] = !on;
            }
            const std::string version = latest["version"].str();
            y2 += btns(ui, ix, y2, iw,
                       {{"u:find", t("Search the drives"), draw::Button::normal,
                         [this, &app] {
                             JsonValue body = JsonValue::object();
                             body.set("everywhere", true);
                             app.post("/api/update/find", body, [this, &app](const Reply& r) {
                                 if (r.ok())
                                     update_ = r.json;
                                 else
                                     app.toast(t_msg(r.error()), App::Toast::bad);
                             });
                         },
                         busy || !wait_for_.empty()},
                        {"u:install", t("Update to {version}", {{"version", version}}), draw::Button::primary,
                         [this, &app, version] {
                             std::vector<std::string> chosen;
                             for (const auto& pl : update_["payloads"].items())
                                 if (!off_[pl["path"].str()]) chosen.push_back(pl["path"].str());
                             std::string body = t("Alembic {version} is downloaded from GitHub and checked; the files "
                                                  "below are replaced, and the new engine starts through the ELF loader "
                                                  "(port 9021) and takes over from this one.",
                                                  {{"version", version}});
                             if (chosen.empty())
                                 body += "\n\n!" + t("No payload files are replaced: your loader still starts the old version next time.");
                             for (const auto& c : chosen) body += "\n\n@" + c;
                             const bool with_app = update_["app"].is_object() && update_["app"]["image"].boolean() &&
                                                   update_["app"]["in_release"].boolean() && !update_["app"]["current"].boolean() &&
                                                   !off_["app"];
                             if (with_app) body += "\n\n~" + t("The console app's image is replaced too, once the app is closed.");
                             bool restarts = false, running = false;
                             for (const auto& j : app.jobs)
                                 if (j["state"].str() == "running") {
                                     running = true;
                                     restarts = restarts || !j["keeps_progress"].boolean();
                                 }
                             if (restarts)
                                 body += "\n\n!" + t("A running job can't keep its progress; it starts over after the update.");
                             else if (running)
                                 body += "\n\n~" + t("The running job continues after the update.");
                             ask(app, t("Update to {version}?", {{"version", version}}), text_body(body), t("Update"), false,
                                 [this, &app, chosen, version, with_app] {
                                     JsonValue paths = JsonValue::array();
                                     for (const auto& c : chosen) paths.push(c);
                                     JsonValue req = JsonValue::object();
                                     req.set("paths", paths);
                                     req.set("app", with_app);
                                     app.post("/api/update/install", req, [this, &app, version](const Reply& r) {
                                         if (!r.ok()) {
                                             app.toast(t_msg(r.error()), App::Toast::bad);
                                             return;
                                         }
                                         update_ = r.json;
                                         wait_for_ = version;
                                         wait_timer_ = 0.0f;
                                         wait_total_ = 0.0f;
                                     });
                                 });
                         },
                         busy || !wait_for_.empty()}});
            y = card_end("payloads", top2, y2);
        }
    }

    // ---- Cache ----
    if (i["cache"].is_object()) {
        const JsonValue& c = i["cache"];
        head(t("Cache"));
        const float top = card_begin(ui, "cache", x, y, w);
        float yy = top;
        const float ix = x + 1.2f * R, iw = w - 2.4f * R;
        yy += facts(ui, ix, yy, iw,
                    {{t("Game info"), plural(c["games"].integer(), "{n} game", "{n} games") + " \xC2\xB7 " +
                                          bytes(c["bytes"].number()), ""}});
        yy += 0.5f * R + draw::paragraph(ui, Face::regular,
                                         t("Titles and icons, so lists open quickly. Entries unused for 30 days are "
                                           "removed at start, and the cache is kept under 100 MB."),
                                         ix, yy + 0.5f * R, iw, 1.0f * R, 1.45f * R, muted);
        yy += btns(ui, ix, yy, iw, {{"s:cache", t("Clear cache"), draw::Button::normal, [this, &app] {
                                         cache_busy_ = true;
                                         app.post("/api/cache/clear", JsonValue::object(), [this, &app](const Reply& r) {
                                             if (r.ok()) {
                                                 app.info.set("cache", r.json);
                                                 app.clear_meta();
                                                 app.toast(t("Cache cleared"), App::Toast::ok);
                                             } else {
                                                 app.toast(t_msg(r.error()), App::Toast::bad);
                                             }
                                             cache_busy_ = false;
                                         });
                                     },
                                     cache_busy_ || !(c["games"].integer() || c["bytes"].integer())}});
        y = card_end("cache", top, yy);
    }
    y += 2.0f * R;
    ui.end_scroll(y + offset - area.y);
    ui.end_zone();
}

}  // namespace alembic::app
