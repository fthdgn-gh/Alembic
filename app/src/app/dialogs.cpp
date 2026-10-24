// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/dialogs.hpp"

#include "ui/components/keyboard.hpp"
#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>

namespace alembic::app {

using namespace style;
using draw::Face;
using hui::gfx::Align;

// ---------------------------------------------------------------- Sheet

void Sheet::begin(Ui& ui, const std::string& title, const std::string& sub, Size size, bool foot) {
    DrawList& l = *ui.list;
    age_ += ui.dt;
    const float fade = std::min(1.0f, age_ / 0.15f);
    l.push_opacity(first_ ? 0.0f : fade);
    // The dimmed page (.overlay).
    l.rounded_rect({0, 0, W, H}, 0.0f, rgb(0x04060a, 0.72f));
    const float rem = size == Size::wide ? 64.0f : size == Size::narrow ? 26.0f : 38.0f;
    const float width = std::min(rem * R, W - 3.0f * R);
    head_h_ = title.empty() ? 0.0f : 1.2f * R + 1.3f * R * 1.45f + 0.2f * R;
    if (!sub.empty()) head_h_ += 0.9f * 0.85f * R * 1.45f;
    foot_ = foot;
    const float foot_h = foot ? 0.9f * R + 2.55f * R + 2.0f + 1.2f * R : 0.0f;
    const float top = 1.5f * R, avail = H - 5.0f * R - top;
    const float height = std::min(head_h_ + 2.0f * R + body_h_ + foot_h, avail);
    rect_ = {(W - width) * 0.5f, top + (avail - height) * 0.5f, width, height};
    l.shadow({rect_.x, rect_.y + 1.5f * R, rect_.w, rect_.h}, 1.1f * R, 4.0f * R, rgb(0x000000, 0.6f));
    l.bordered_rect(rect_, 1.1f * R, panel, 1.0f, line2);
    float y = rect_.y;
    if (!title.empty()) {
        y += 1.2f * R;
        draw::text_fit(ui, Face::semibold, title, rect_.x + 1.5f * R, draw::baseline(y + 1.3f * R * 0.725f, 1.3f * R),
                       1.3f * R, rect_.w - 3.0f * R, style::text);
        y += 1.3f * R * 1.45f + 0.2f * R;
    }
    if (!sub.empty()) {
        const float s = 0.9f * 0.85f * R;
        draw::text_fit(ui, Face::mono, sub, rect_.x + 1.5f * R, draw::baseline(y + s * 0.725f, s), s,
                       rect_.w - 3.0f * R, muted);
        y += s * 1.45f;
    }
    const Rect view{rect_.x, rect_.y + head_h_, rect_.w, rect_.h - head_h_ - foot_h};
    scroll_ = Id("sheet") / static_cast<std::int64_t>(reinterpret_cast<std::intptr_t>(this) & 0xffffff);
    const float offset = ui.begin_scroll(scroll_, view);
    body_x_ = rect_.x + 1.5f * R;
    body_w_ = rect_.w - 3.0f * R;
    content_top_ = view.y + 1.0f * R - offset;
    foot_y_ = rect_.y + rect_.h - foot_h;
    foot_right_ = rect_.x + rect_.w - 1.5f * R;
    foot_left_ = rect_.x + 1.5f * R;
}

void Sheet::end_body(Ui& ui, float bottom) {
    body_h_ = std::max(0.0f, bottom - content_top_);
    ui.end_scroll(body_h_ + 2.0f * R);
    if (foot_) draw::hline(ui, rect_.x, foot_y_, rect_.w, line);
}

bool Sheet::button(Ui& ui, Id id, const std::string& label, draw::Button kind, bool disabled, unsigned flags) {
    const float w = draw::button_width(ui, label, false);
    const float h = 2.55f * R + 2.0f;
    const Rect r{foot_right_ - w, foot_y_ + 0.9f * R, w, h};
    foot_right_ -= w + 0.6f * R;
    return draw::button(ui, id, r, label, kind, false, disabled, flags);
}

void Sheet::message(Ui& ui, const std::string& text, bool error) {
    if (text.empty()) return;
    const float s = 0.88f * R;
    const float w = foot_right_ - foot_left_ - 1.0f * R;
    const float h = draw::paragraph_height(ui, Face::regular, text, w, s, s * 1.45f, 3);
    const float cy = foot_y_ + 0.9f * R + (2.55f * R + 2.0f) * 0.5f;
    draw::paragraph(ui, Face::regular, text, foot_left_, cy - h * 0.5f, w, s, s * 1.45f, error ? bad : muted, 3);
}

void Sheet::end(Ui& ui) {
    ui.list->pop_opacity();
    first_ = false;
}

// ---------------------------------------------------------------- rows

float section(Ui& ui, const std::string& text, float x, float y, float top_margin) {
    const float s = 0.72f * R;
    y += top_margin * R;
    draw::caps(ui, text, x, draw::baseline(y + s * 0.725f, s), s, dim);
    return top_margin * R + s * 1.45f + 0.5f * R;
}

namespace {

// The label and hint of a form row; returns the height of the text block.
float row_text(Ui& ui, float x, float y, float w, const std::string& label, const std::string& hint, bool draw_it) {
    const float ls = 1.0f * R, hs = 0.8f * R;
    float h = ls * 1.45f;
    if (draw_it) draw::text_fit(ui, Face::regular, label, x, draw::baseline(y + ls * 0.725f, ls), ls, w, style::text);
    if (!hint.empty()) {
        if (draw_it)
            h += draw::paragraph(ui, Face::regular, hint, x, y + h, w, hs, hs * 1.45f, muted);
        else
            h += draw::paragraph_height(ui, Face::regular, hint, w, hs, hs * 1.45f);
    }
    return h;
}

float frow(Ui& ui, Id id, float x, float y, float w, const std::string& label, const std::string& hint, float right_w,
           unsigned flags, bool* pressed, Rect* out) {
    const float pad_x = 0.9f * R, pad_y = 0.7f * R;
    const float text_w = w - 2.0f * pad_x - right_w - 1.0f * R;
    const float th = row_text(ui, 0, 0, text_w, label, hint, false);
    const float h = std::max(3.2f * R, th + 2.0f * pad_y + 2.0f);
    const Rect r{x, y, w, h};
    *out = r;
    if (ui.visible(r)) {
        ui.list->bordered_rect(r, 0.75f * R, field, 1.0f, line);
        row_text(ui, x + pad_x, y + (h - th) * 0.5f, text_w, label, hint, true);
    }
    const bool p = ui.item(id, r, flags, 0.75f * R);
    if (pressed) *pressed = p;
    return h + 0.5f * R;
}

}  // namespace

float switch_row(Ui& ui, Id id, float x, float y, float w, const std::string& label, const std::string& hint, bool* on,
                 bool* changed) {
    bool pressed = false;
    Rect r;
    const float h = frow(ui, id, x, y, w, label, hint, 2.8f * R, 0, &pressed, &r);
    if (pressed) {
        *on = !*on;
        ui.play(hui::audio::Cue::toggle);
    }
    if (changed) *changed = pressed;
    if (ui.visible(r)) draw::switch_(ui, {r.x + r.w - 0.9f * R - 2.8f * R, r.cy() - 0.8f * R, 2.8f * R, 1.6f * R}, *on);
    return h;
}

float choice_row(Ui& ui, Id id, float x, float y, float w, const std::string& label, const std::string& hint,
                 const std::vector<std::string>& labels, int* index, bool* changed) {
    float vw = 6.0f * R;
    for (const auto& s : labels) vw = std::max(vw, draw::measure(ui, Face::semibold, s, 1.0f * R) + 0.4f * R);
    const float arrows = 2.0f * (1.1f * R * 0.6f + 1.1f * R);
    bool pressed = false;
    Rect r;
    const float h = frow(ui, id, x, y, w, label, hint, vw + arrows, kStepper, &pressed, &r);
    const int n = static_cast<int>(labels.size());
    int i = std::clamp(*index, 0, std::max(0, n - 1));
    int next = i;
    if (const int d = ui.stepped(id)) next = std::clamp(i + d, 0, n - 1);
    if (pressed) next = i == n - 1 ? 0 : i + 1;
    if (next != i) {
        *index = next;
        ui.play(hui::audio::Cue::slider);
        if (changed) *changed = true;
    } else if (changed) {
        *changed = false;
    }
    if (ui.visible(r) && n > 0) {
        const bool f = ui.focused(id);
        const float right = r.x + r.w - 0.9f * R;
        const float cy = r.cy();
        const Color arr = f ? focus : dim;
        const float as = 1.1f * R;
        draw::chevron(ui, right - 0.55f * R - as * 0.2f, cy, as, 1, arr);
        const float vx = right - 0.55f * R - as * 0.4f - 0.55f * R - vw;
        draw::text_fit(ui, Face::semibold, labels[static_cast<std::size_t>(*index)], vx + vw * 0.5f,
                       draw::baseline(cy, 1.0f * R), 1.0f * R, vw, style::text, Align::center);
        draw::chevron(ui, vx - 0.55f * R - as * 0.2f, cy, as, -1, arr);
    }
    return h;
}

float link_row(Ui& ui, Id id, float x, float y, float w, const std::string& label, const std::string& hint,
               const std::string& value, bool* pressed) {
    const float vw = value.empty() ? 0.0f : std::min(16.0f * R, draw::measure(ui, Face::semibold, value, 1.0f * R));
    Rect r;
    const float h = frow(ui, id, x, y, w, label, hint, vw + 2.0f * R, 0, pressed, &r);
    if (ui.visible(r)) {
        const float right = r.x + r.w - 0.9f * R;
        const float as = 1.1f * R;
        draw::chevron(ui, right - 0.55f * R - as * 0.2f, r.cy(), as, 1, ui.focused(id) ? focus : dim);
        if (!value.empty())
            draw::text_fit(ui, Face::semibold, value, right - 0.55f * R - as * 0.4f - 0.55f * R,
                           draw::baseline(r.cy(), 1.0f * R), 1.0f * R, 16.0f * R, style::text, Align::right);
    }
    return h;
}

float facts(Ui& ui, float x, float y, float w, const std::vector<Fact>& rows) {
    const float s = 0.88f * R, lh = s * 1.45f, pad = 0.4f * R;
    float label_w = 0.0f;
    for (const auto& f : rows) label_w = std::max(label_w, draw::measure(ui, Face::regular, f.label, s));
    label_w = std::min(label_w + 1.0f * R, w * 0.5f);
    const float vw = w - label_w;
    float at = y;
    for (const auto& f : rows) {
        const Face face = f.cls == "mono" ? Face::mono : Face::regular;
        const float vs = f.cls == "mono" ? s * 0.92f : s;
        // Values wrap; long ones (paths, addresses) break anywhere.
        const float vh = std::max(lh, draw::paragraph_height(ui, face, f.value, vw, vs, lh));
        const float h = vh + 2.0f * pad;
        if (ui.visible({x, at, w, h})) {
            draw::hline(ui, x, at, w, line);
            draw::text_fit(ui, Face::regular, f.label, x, draw::baseline(at + pad + lh * 0.5f, s), s, label_w - 0.5f * R,
                           muted);
            // Right-aligned lines.
            std::size_t start = 0;
            float ly = at + pad;
            const std::string& v = f.value;
            const auto& fnt = draw::font(ui, face);
            while (start <= v.size()) {
                std::size_t nl = v.find('\n', start);
                if (nl == std::string::npos) nl = v.size();
                for (const auto& line_text : fnt.font->wrap(std::string_view(v).substr(start, nl - start), vs, vw)) {
                    draw::text(ui, face, line_text, x + w, draw::baseline(ly + lh * 0.5f, vs), vs,
                               f.cls == "err" ? bad : style::text, Align::right);
                    ly += lh;
                }
                if (nl == start) ly += 0.0f;
                start = nl + 1;
            }
        }
        at += h;
    }
    return at - y;
}

// ---------------------------------------------------------------- ask

namespace {

class AskOverlay : public Overlay {
public:
    std::string title, ok;
    Body body;
    bool danger = false;
    std::function<void()> yes, no;
    Sheet sheet;

    void build(Ui& ui, App& app) override {
        sheet.begin(ui, title, "", Sheet::Size::narrow, true);
        float y = sheet.content_top();
        if (body) y += body(ui, app, sheet.x(), y, sheet.w());
        sheet.end_body(ui, y);
        if (sheet.button(ui, "ask:ok", ok.empty() ? t("OK") : ok,
                         danger ? draw::Button::danger_solid : draw::Button::primary, false, kAutofocus)) {
            app.close(this);
            if (yes) yes();
        }
        if (sheet.button(ui, "ask:cancel", t("Cancel"), draw::Button::normal)) back(app);
        sheet.end(ui);
    }
    std::vector<Hint> hints(App&) override { return {{Glyph::cross, t("Choose")}, {Glyph::circle, t("Cancel")}}; }
    void back(App& app) override {
        app.close(this);
        if (no) no();
    }
};

}  // namespace

void ask(App& app, const std::string& title, Body body, const std::string& ok, bool danger, std::function<void()> yes,
         std::function<void()> no) {
    auto o = std::make_unique<AskOverlay>();
    o->title = title;
    o->body = std::move(body);
    o->ok = ok;
    o->danger = danger;
    o->yes = std::move(yes);
    o->no = std::move(no);
    app.open(std::move(o));
}

Body text_body(const std::string& text) {
    return [text](Ui& ui, App&, float x, float y, float w) -> float {
        const float s = 1.0f * R, lh = s * 1.45f;
        float at = y;
        bool first = true;
        std::size_t start = 0;
        while (start <= text.size()) {
            std::size_t end = text.find("\n\n", start);
            if (end == std::string::npos) end = text.size();
            std::string part = text.substr(start, end - start);
            start = end + 2;
            if (part.empty()) continue;
            if (part[0] == '@') {  // a path
                const float ms = 0.85f * R;
                at += draw::paragraph(ui, Face::mono, part.substr(1), x, at, w, ms, ms * 1.45f, style::text);
                first = false;
                continue;
            }
            if (part[0] == '!') {  // .warnbox
                const float ws = 0.92f * R;
                const float ph = draw::paragraph_height(ui, Face::regular, part.substr(1), w - 1.8f * R, ws, ws * 1.45f);
                const Rect box{x, at, w, ph + 1.4f * R};
                ui.list->bordered_rect(box, 0.7f * R, bad.with_alpha(0.08f), 1.0f, bad.with_alpha(0.3f));
                draw::paragraph(ui, Face::regular, part.substr(1), x + 0.9f * R, at + 0.7f * R, w - 1.8f * R, ws,
                                ws * 1.45f, style::text);
                at += box.h + 0.6f * R;
                first = false;
                continue;
            }
            Color c = style::text;
            if (part[0] == '~') {  // .muted
                part.erase(0, 1);
                c = muted;
            }
            if (!first) at += 1.0f * R;
            at += draw::paragraph(ui, Face::regular, part, x, at, w, s, lh, c);
            first = false;
        }
        return at - y;
    };
}

// ---------------------------------------------------------------- choose

namespace {

class ChooseOverlay : public Overlay {
public:
    std::string title, sub;
    std::vector<Choice> items;
    Sheet sheet;

    void build(Ui& ui, App& app) override {
        sheet.begin(ui, title, sub, Sheet::Size::narrow, false);
        float y = sheet.content_top();
        const float x = sheet.x(), w = sheet.w();
        bool any_selected = false;
        for (const auto& it : items) any_selected = any_selected || (it.selected && !it.heading);
        bool first_choice = true;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const Choice& it = items[i];
            if (it.heading) {
                const float s = 0.78f * R;
                y += 0.7f * R;
                draw::text_fit(ui, Face::semibold, it.label, x + 1.0f * R, draw::baseline(y + s * 0.725f, s), s,
                               w - 2.0f * R, muted);
                y += s * 1.45f + 0.25f * R;
                continue;
            }
            const float ls = 1.05f * R, ss = 0.8f * R;
            const float tw = w - 2.0f * R;
            const float sub_h = it.sub.empty() ? 0.0f : draw::paragraph_height(ui, Face::regular, it.sub, tw, ss, ss * 1.45f);
            const float h = 0.75f * R * 2.0f + ls * 1.45f + sub_h;
            y += 0.1f * R;
            const Rect r{x, y, w, h};
            const Id id = Id("choice") / static_cast<std::int64_t>(i);
            const bool autof = it.selected || (first_choice && !any_selected);
            first_choice = false;
            const bool f = ui.focused(id);
            if (ui.visible(r)) {
                if (f) ui.list->rounded_rect(r, 0.6f * R, it.danger ? bad : accent);
                const Color ink = f ? Color{1, 1, 1, 1} : it.danger ? bad : style::text;
                draw::text_fit(ui, Face::regular, it.label, x + 1.0f * R, draw::baseline(y + 0.75f * R + ls * 0.725f, ls),
                               ls, tw, ink);
                if (!it.sub.empty())
                    draw::paragraph(ui, Face::regular, it.sub, x + 1.0f * R, y + 0.75f * R + ls * 1.45f, tw, ss, ss * 1.45f,
                                    f ? Color{1, 1, 1, 0.8f} : muted);
            }
            if (ui.item(id, r, kNoRing | (autof ? kAutofocus : 0u), 0.6f * R)) {
                app.close(this);
                if (it.act) it.act();
            }
            y += h + 0.1f * R;
        }
        sheet.end_body(ui, y);
        sheet.end(ui);
    }
    std::vector<Hint> hints(App&) override { return {{Glyph::cross, t("Choose")}, {Glyph::circle, t("Cancel")}}; }
};

}  // namespace

void choose(App& app, const std::string& title, std::vector<Choice> items, const std::string& sub) {
    auto o = std::make_unique<ChooseOverlay>();
    o->title = title;
    o->sub = sub;
    o->items = std::move(items);
    app.open(std::move(o));
}

// ---------------------------------------------------------------- text entry

namespace {

hui::ui::Theme keyboard_theme() {
    hui::ui::Theme th = hui::ui::themes()[3];  // flat surfaces
    th.page = panel;
    th.surface = panel2;
    th.surface_high = raise;
    th.text = style::text;
    th.text_muted = muted;
    th.primary = accent;
    th.on_primary = Color{1, 1, 1, 1};
    th.secondary = panel2;
    th.on_secondary = style::text;
    th.accent = accent;
    th.outline = line2;
    th.focus = focus;
    th.shadow = rgb(0x000000, 0.5f);
    th.light = rgb(0xffffff, 0.06f);
    th.radius = 0.6f * R;
    th.radius_card = 0.9f * R;
    th.border = 1.0f;
    th.label = hui::ui::FontRole::regular;
    th.heading = hui::ui::FontRole::semibold;
    th.caps = false;
    th.dark = true;
    return th;
}

void pop_utf8(std::string* s) {
    if (s->empty()) return;
    std::size_t i = s->size() - 1;
    while (i > 0 && (static_cast<unsigned char>((*s)[i]) & 0xC0) == 0x80) --i;
    s->erase(i);
}

class TextOverlay : public Overlay {
public:
    std::string title, ok, value;
    bool secret = false;
    std::function<void(const std::string&)> done;
    hui::ui::Keyboard keys;
    Sheet sheet;
    bool ready = false;

    void setup(bool numeric) {
        keys.style.theme = keyboard_theme();
        keys.style.bindings = hui::ui::KeyboardBindings::standard();
        keys.style.bindings.done = Action::menu;
        keys.style.space_label = t("Space");
        keys.style.done_label = ok;
        keys.style.panel = false;
        keys.style.label_size = 1.35f * R;
        keys.style.wide_label_size = 0.95f * R;
        if (numeric) keys.set_layouts({hui::ui::KeyboardLayout::numeric()});
        keys.on_text = [this](std::string_view s) { value += s; };
        keys.on_backspace = [this] { pop_utf8(&value); };
        keys.set_length(-1);
    }

    void build(Ui& ui, App& app) override {
        sheet.begin(ui, title, "", Sheet::Size::wide, false);
        float y = sheet.content_top();
        const float x = sheet.x(), w = sheet.w();
        // The line being typed (.input), with a caret.
        const float s = 1.15f * R;
        const Rect box{x, y, w, s * 1.45f + 1.3f * R + 2.0f};
        ui.list->bordered_rect(box, 0.65f * R, input, 1.0f, focus);
        std::string shown = value;
        if (secret) shown = std::string(value.size(), '*');
        const float tw = draw::measure(ui, Face::mono, shown, s);
        const float avail = w - 1.6f * R - 0.4f * R;
        const float shift = std::max(0.0f, tw - avail);
        ui.list->push_clip(box.inset(0.4f * R));
        draw::text(ui, Face::mono, shown, x + 0.8f * R - shift, draw::baseline(box.cy(), s), s, style::text);
        if (std::fmod(ui.time, 1.0f) < 0.6f)
            ui.list->rounded_rect({x + 0.8f * R - shift + tw + 2.0f, box.cy() - s * 0.6f, 2.0f, s * 1.2f}, 0.0f, focus);
        ui.list->pop_clip();
        y += box.h + 1.0f * R;
        // The keyboard.
        const int rows = static_cast<int>(keys.layouts()[static_cast<std::size_t>(keys.layout())].rows.size());
        const float kh = static_cast<float>(rows) * 3.5f * R;
        keys.set_bounds({x, y, w, kh});
        if (!ready) {
            keys.enter();
            ready = true;
        }
        keys.set_active(true);
        if (ui.layer_active()) {
            const hui::ui::Event e = keys.handle(ui.input(), *ui.feedback);
            if (e == hui::ui::Event::activated) {
                app.close(this);
                if (done) done(value);
            } else if (e == hui::ui::Event::cancelled) {
                back(app);
            }
        }
        keys.update(ui.dt);
        hui::ui::Canvas canvas{*ui.list, *ui.fonts, 0, ui.time};
        keys.draw(canvas);
        y += kh;
        sheet.end_body(ui, y);
        sheet.end(ui);
    }
    std::vector<Hint> hints(App&) override {
        return {{Glyph::cross, t("Type")}, {Glyph::square, t("Delete")}, {Glyph::triangle, t("Space")},
                {Glyph::options, ok}, {Glyph::circle, t("Cancel")}};
    }
};

}  // namespace

void ask_text(App& app, const std::string& title, const std::string& initial, const std::string& ok,
              std::function<void(const std::string&)> done, bool numeric, bool secret) {
    auto o = std::make_unique<TextOverlay>();
    o->title = title;
    o->ok = ok.empty() ? t("OK") : ok;
    o->value = initial;
    o->secret = secret;
    o->done = std::move(done);
    o->setup(numeric);
    app.open(std::move(o));
}

bool field_row(Ui& ui, App& app, Id id, float x, float* y, float w, const std::string& label, std::string* value,
               const std::string& placeholder, bool numeric, bool secret) {
    const float ls = 0.72f * R;
    *y += 1.1f * R;
    draw::caps(ui, label, x + 0.1f * R, draw::baseline(*y + ls * 0.725f, ls), ls, dim);
    *y += ls * 1.45f + 0.45f * R;
    const float s = 0.9f * R;
    const Rect box{x, *y, w, s * 1.45f + 1.3f * R + 2.0f};
    ui.list->bordered_rect(box, 0.65f * R, input, 1.0f, line2);
    const bool empty = value->empty();
    const std::string shown = empty ? placeholder : secret ? std::string(value->size(), '*') : *value;
    draw::text_fit(ui, Face::mono, shown, x + 0.8f * R, draw::baseline(box.cy(), s), s, w - 1.6f * R,
                   empty ? dim : style::text);
    *y += box.h;
    if (ui.item(id, box, 0, 0.65f * R)) {
        const int owner = app.top() ? app.top()->key : 0;
        ask_text(app, label, *value, t("OK"),
                 [&app, owner, value](const std::string& v) {
                     if (app.overlay_alive(owner)) *value = v;
                 },
                 numeric, secret);
        return true;
    }
    return false;
}

}  // namespace alembic::app
