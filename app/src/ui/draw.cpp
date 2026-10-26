// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/draw.hpp"

#include <algorithm>
#include <cmath>

namespace alembic::app::draw {

using namespace style;

const hui::ui::FontRef& font(Ui& ui, Face face) {
    switch (face) {
    case Face::semibold: return ui.fonts->semibold;
    case Face::mono: return ui.fonts->mono;
    default: return ui.fonts->regular;
    }
}

namespace {

// Characters of the page's texts that the baked fonts lack: ⋯ ‹ › become … < >.
bool needs_swap(std::string_view s) {
    for (std::size_t i = 0; i + 2 < s.size(); ++i)
        if (static_cast<unsigned char>(s[i]) == 0xE2 &&
            ((static_cast<unsigned char>(s[i + 1]) == 0x8B && static_cast<unsigned char>(s[i + 2]) == 0xAF) ||
             (static_cast<unsigned char>(s[i + 1]) == 0x80 &&
              (static_cast<unsigned char>(s[i + 2]) == 0xB9 || static_cast<unsigned char>(s[i + 2]) == 0xBA))))
            return true;
    return false;
}
std::string swapped(std::string_view s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i + 2 < s.size() && static_cast<unsigned char>(s[i]) == 0xE2) {
            const unsigned char a = static_cast<unsigned char>(s[i + 1]), b = static_cast<unsigned char>(s[i + 2]);
            if (a == 0x8B && b == 0xAF) {
                out += "\xE2\x80\xA6";
                i += 2;
                continue;
            }
            if (a == 0x80 && (b == 0xB9 || b == 0xBA)) {
                out += b == 0xB9 ? '<' : '>';
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

}  // namespace

float measure(Ui& ui, Face face, std::string_view s, float size) {
    if (needs_swap(s)) return font(ui, face).measure(swapped(s), size);
    return font(ui, face).measure(s, size);
}

float text(Ui& ui, Face face, std::string_view s, float x, float baseline, float size, Color color, Align align,
           float tracking) {
    if (needs_swap(s)) return hui::ui::text(*ui.list, font(ui, face), swapped(s), x, baseline, size, color, align, tracking);
    return hui::ui::text(*ui.list, font(ui, face), s, x, baseline, size, color, align, tracking);
}

float text_fit(Ui& ui, Face face, std::string_view s_in, float x, float baseline, float size, float max_width,
               Color color, Align align) {
    const std::string copy = needs_swap(s_in) ? swapped(s_in) : std::string();
    const std::string_view s = copy.empty() ? s_in : std::string_view(copy);
    const auto& f = font(ui, face);
    if (max_width <= 0.0f) return 0.0f;
    if (f.measure(s, size) <= max_width) return hui::ui::text(*ui.list, f, s, x, baseline, size, color, align);
    const std::string cut = f.font->fit(s, size, max_width);
    return hui::ui::text(*ui.list, f, cut, x, baseline, size, color, align);
}

float paragraph(Ui& ui, Face face, std::string_view s_in, float x, float top, float width, float size,
                float line_height, Color color, int max_lines) {
    const std::string copy = needs_swap(s_in) ? swapped(s_in) : std::string();
    const std::string_view s = copy.empty() ? s_in : std::string_view(copy);
    const auto& f = font(ui, face);
    hui::ui::ParagraphLines lines;
    // Explicit line breaks are kept (facts with several values).
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t nl = s.find('\n', at);
        if (nl == std::string_view::npos) nl = s.size();
        auto part = hui::ui::paragraph_lines(f, s.substr(at, nl - at), size, width, max_lines);
        for (auto& l : part.lines) lines.lines.push_back(std::move(l));
        at = nl + 1;
    }
    if (static_cast<int>(lines.lines.size()) > max_lines) lines.lines.resize(static_cast<std::size_t>(max_lines));
    float y = top + (line_height - size) * 0.5f + 0.86f * size;
    for (const std::string& l : lines.lines) {
        hui::ui::text(*ui.list, f, l, x, y, size, color);
        y += line_height;
    }
    return static_cast<float>(lines.lines.size()) * line_height;
}

float paragraph_height(Ui& ui, Face face, std::string_view s, float width, float size, float line_height,
                       int max_lines) {
    const auto& f = font(ui, face);
    int count = 0;
    std::size_t at = 0;
    while (at <= s.size()) {
        std::size_t nl = s.find('\n', at);
        if (nl == std::string_view::npos) nl = s.size();
        count += static_cast<int>(hui::ui::paragraph_lines(f, s.substr(at, nl - at), size, width, max_lines).lines.size());
        at = nl + 1;
    }
    return static_cast<float>(std::min(count, max_lines)) * line_height;
}

float caps(Ui& ui, std::string_view s, float x, float baseline, float size, Color color) {
    std::string upper;
    upper.reserve(s.size());
    for (char c : s) upper += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    return text(ui, Face::semibold, upper, x, baseline, size, color, Align::left, size * 0.14f);
}

namespace {

// A stroked rectangle in the icon's 24-unit box.
void stroke_rect(DrawList& l, const Rect& box, float x, float y, float w, float h, float r, float sw, Color c) {
    const float k = box.w / 24.0f;
    l.bordered_rect({box.x + x * k, box.y + y * k, w * k, h * k}, r * k, Color{0, 0, 0, 0}, sw * k, c);
}
void seg(DrawList& l, const Rect& box, float x1, float y1, float x2, float y2, float sw, Color c) {
    const float k = box.w / 24.0f;
    l.line(box.x + x1 * k, box.y + y1 * k, box.x + x2 * k, box.y + y2 * k, sw * k, c);
}
void dot(DrawList& l, const Rect& box, float x, float y, float r, Color c) {
    const float k = box.w / 24.0f;
    l.circle(box.x + x * k, box.y + y * k, r * k, c);
}

}  // namespace

void icon(Ui& ui, Icon kind, const Rect& box, Color c) {
    DrawList& l = *ui.list;
    const float k = box.w / 24.0f;
    const float sw = 1.8f;
    switch (kind) {
    case Icon::folder:
        l.rounded_rect({box.x + 3 * k, box.y + 4 * k, 8.5f * k, 5 * k}, 1.6f * k, c);
        l.rounded_rect({box.x + 3 * k, box.y + 6 * k, 18 * k, 14 * k}, 2.5f * k, c);
        break;
    case Icon::drive:
        stroke_rect(l, box, 3, 7, 18, 10, 2, sw, c);
        dot(l, box, 17, 12, 1.1f, c);
        break;
    case Icon::usb:
        stroke_rect(l, box, 7, 9, 10, 12, 1.5f, sw, c);
        seg(l, box, 9, 9, 9, 4, sw, c);
        seg(l, box, 9, 4, 15, 4, sw, c);
        seg(l, box, 15, 4, 15, 9, sw, c);
        break;
    case Icon::server:
        stroke_rect(l, box, 4, 4, 16, 7, 1.5f, sw, c);
        stroke_rect(l, box, 4, 13, 16, 7, 1.5f, sw, c);
        dot(l, box, 8, 7.5f, 1.0f, c);
        dot(l, box, 8, 16.5f, 1.0f, c);
        break;
    case Icon::place:
        stroke_rect(l, box, 4, 6, 7, 4, 1.5f, sw, c);
        stroke_rect(l, box, 4, 8, 16, 11, 1.5f, sw, c);
        break;
    case Icon::clock:
        l.ring(box.x + 12 * k, box.y + 12 * k, 8 * k, sw * k, c);
        seg(l, box, 12, 8, 12, 12, sw, c);
        seg(l, box, 12, 12, 15, 14, sw, c);
        break;
    case Icon::file:
        seg(l, box, 7, 3, 14, 3, sw, c);
        seg(l, box, 14, 3, 18, 7, sw, c);
        seg(l, box, 18, 7, 18, 21, sw, c);
        seg(l, box, 18, 21, 7, 21, sw, c);
        seg(l, box, 7, 21, 7, 3, sw, c);
        seg(l, box, 14, 3, 14, 7, sw, c);
        seg(l, box, 14, 7, 18, 7, sw, c);
        break;
    }
}

void kind_colors(std::string_view kind, Color* fill, Color* ink) {
    if (kind == "fpkg") {
        *fill = accent.with_alpha(0.16f);
        *ink = accent;
    } else if (kind == "ffpfsc") {
        *fill = ok.with_alpha(0.15f);
        *ink = ok;
    } else if (kind == "exfat") {
        *fill = warn.with_alpha(0.15f);
        *ink = warn;
    } else if (kind == "ffpkg") {
        *fill = accent2.with_alpha(0.16f);
        *ink = accent2;
    } else if (kind == "ffpfs") {
        *fill = teal.with_alpha(0.14f);
        *ink = teal;
    } else if (kind == "folder" || kind == "game") {
        *fill = accent.with_alpha(0.12f);
        *ink = focus;
    } else {
        *fill = rgb(0x2a3142);
        *ink = rgb(0xc5ccdd);
    }
}

void ico(Ui& ui, const Rect& box, std::string_view kind, std::string_view label, std::uint32_t texture) {
    DrawList& l = *ui.list;
    const float radius = box.w * (0.7f / 3.4f);
    if (texture) {
        l.rounded_rect(box, radius, rgb(0x0f131b));
        l.image(texture, box, hui::gfx::Rect{0, 0, 1, 1}, Color{1, 1, 1, 1}, radius);
        return;
    }
    Color fill, ink;
    if (kind == "file") {
        fill = rgb(0x1d2433);
        ink = muted;
    } else {
        kind_colors(kind, &fill, &ink);
        if (kind != "folder" && kind != "game" && kind != "fpkg" && kind != "ffpfsc" && kind != "exfat" &&
            kind != "ffpkg" && kind != "ffpfs") {
            fill = rgb(0x1d2433);
            ink = muted;
        }
    }
    l.rounded_rect(box, radius, fill);
    const Rect inner{box.x + box.w * 0.225f, box.y + box.h * 0.225f, box.w * 0.55f, box.h * 0.55f};
    if (kind == "folder") {
        icon(ui, Icon::folder, inner, ink);
    } else if (kind == "file") {
        icon(ui, Icon::file, inner, ink);
    } else {
        const float size = box.w * (0.62f / 3.4f);
        text(ui, Face::semibold, label, box.cx(), baseline(box.cy(), size), size, ink, Align::center, size * 0.04f);
    }
}

float fmt_pill(Ui& ui, std::string_view kind, std::string_view label, float x, float cy) {
    Color fill, ink;
    kind_colors(kind, &fill, &ink);
    if (kind == "folder") {
        fill = rgb(0x2a3142);
        ink = rgb(0xc5ccdd);
    }
    const float size = 0.66f * R;
    const float w = measure(ui, Face::semibold, label, size) + size * 0.06f * static_cast<float>(label.size()) +
                    0.9f * R;
    const float h = size + 0.44f * R;
    ui.list->rounded_rect({x, cy - h * 0.5f, w, h}, h * 0.5f, fill);
    text(ui, Face::semibold, label, x + 0.45f * R, baseline(cy, size), size, ink, Align::left, size * 0.06f);
    return w;
}

float platform_pill(Ui& ui, std::string_view platform, float x, float cy) {
    const bool ps5 = platform == "ps5";
    if (!ps5 && platform != "ps4") return 0;
    const std::string_view label = ps5 ? "PS5" : "PS4";
    const float size = 0.66f * R;
    const float w = measure(ui, Face::semibold, label, size) + size * 0.06f * static_cast<float>(label.size()) + 0.9f * R;
    const float h = size + 0.44f * R;
    if (ps5) {
        ui.list->rounded_rect({x, cy - h * 0.5f, w, h}, h * 0.5f, rgb(0xffffff));
    } else {
        // Outlined, so the black pill shows on the dark rows (as the page's inset shadow).
        const float line = std::max(1.0f, 0.06f * R);
        ui.list->rounded_rect({x, cy - h * 0.5f, w, h}, h * 0.5f, rgb(0xffffff, 0.4f));
        ui.list->rounded_rect({x + line, cy - h * 0.5f + line, w - 2 * line, h - 2 * line}, h * 0.5f - line, rgb(0x000000));
    }
    text(ui, Face::semibold, label, x + 0.45f * R, baseline(cy, size), size, ps5 ? rgb(0x000000) : rgb(0xffffff), Align::left,
         size * 0.06f);
    return w;
}

float state_pill(Ui& ui, std::string_view state, std::string_view label, float right, float cy) {
    Color fill = rgb(0x232a3a), ink = muted;
    if (state == "running") {
        fill = accent.with_alpha(0.15f);
        ink = accent;
    } else if (state == "paused") {
        fill = rgb(0xf0b450, 0.15f);
        ink = warn;
    } else if (state == "done") {
        fill = ok.with_alpha(0.15f);
        ink = ok;
    } else if (state == "failed" || state == "interrupted") {
        fill = bad.with_alpha(0.14f);
        ink = bad;
    }
    std::string upper;
    for (char c : label) upper += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
    const float size = 0.7f * R;
    const float tw = measure(ui, Face::semibold, upper, size) + size * 0.06f * static_cast<float>(upper.size());
    const float w = tw + 1.2f * R, h = size + 0.5f * R;
    ui.list->rounded_rect({right - w, cy - h * 0.5f, w, h}, h * 0.5f, fill);
    text(ui, Face::semibold, upper, right - w + 0.6f * R, baseline(cy, size), size, ink, Align::left, size * 0.06f);
    return w;
}

float tag(Ui& ui, std::string_view label, float right, float cy) {
    const float size = 0.72f * R;
    const float w = measure(ui, Face::regular, label, size) + 0.9f * R + 2.0f, h = size + 0.55f * R;
    ui.list->bordered_rect({right - w, cy - h * 0.5f, w, h}, 0.4f * R, Color{0, 0, 0, 0}, 1.0f, line2);
    text(ui, Face::regular, label, right - w + 0.45f * R + 1.0f, baseline(cy, size), size, muted);
    return w;
}

void meter(Ui& ui, const Rect& r, float used, bool low) {
    ui.list->rounded_rect(r, r.h * 0.5f, rgb(0x263049));
    used = std::clamp(used, 0.0f, 1.0f);
    if (used > 0.0f) ui.list->rounded_rect({r.x, r.y, r.w * used, r.h}, r.h * 0.5f, low ? warn : accent);
}

void progress(Ui& ui, const Rect& r, float fraction, bool indeterminate, float opacity) {
    DrawList& l = *ui.list;
    l.rounded_rect(r, r.h * 0.5f, rgb(0x0a0d13, opacity));
    if (indeterminate) {
        // A third of the bar sliding across, as the page's animation.
        const float phase = std::fmod(ui.time / 1.4f, 1.0f);
        const float w = r.w * 0.3f;
        const float x = r.x - w + (r.w + w) * phase;
        l.push_clip(r);
        l.gradient_rect_h({x, r.y, w, r.h}, r.h * 0.5f, accent.with_alpha(opacity), accent2.with_alpha(opacity));
        l.pop_clip();
        return;
    }
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0.0f) return;
    l.gradient_rect_h({r.x, r.y, std::max(r.h, r.w * fraction), r.h}, r.h * 0.5f, accent.with_alpha(opacity),
                      accent2.with_alpha(opacity));
}

void switch_(Ui& ui, const Rect& r, bool on) {
    DrawList& l = *ui.list;
    l.rounded_rect(r, r.h * 0.5f, on ? accent : rgb(0x2c3549));
    const float d = r.h - 0.4f * R;
    const float x = on ? r.x + r.w - 0.2f * R - d : r.x + 0.2f * R;
    l.circle(x + d * 0.5f, r.cy(), d * 0.5f, on ? Color{1, 1, 1, 1} : rgb(0xc9d1e2));
}

float button_width(Ui& ui, std::string_view label, bool small) {
    const float size = (small ? 0.9f : 1.0f) * R;
    if (label == kMore) return size * 1.3f + 2.0f * (small ? 0.75f : 1.05f) * R + 2.0f;
    return measure(ui, Face::regular, label, size) + 2.0f * (small ? 0.75f : 1.05f) * R + 2.0f;
}

bool button(Ui& ui, Id id, const Rect& r, std::string_view label, Button kind, bool small, bool disabled,
            unsigned flags) {
    DrawList& l = *ui.list;
    const float size = (small ? 0.9f : 1.0f) * R;
    const float radius = 0.65f * R;
    Color fill = panel2, border = line2, ink = style::text;
    Face face = Face::regular;
    switch (kind) {
    case Button::primary:
        fill = accent;
        border = accent;
        ink = Color{1, 1, 1, 1};
        face = Face::semibold;
        break;
    case Button::danger: ink = bad; break;
    case Button::danger_solid:
        fill = bad;
        border = bad;
        ink = Color{1, 1, 1, 1};
        break;
    case Button::ghost:
        fill = Color{0, 0, 0, 0};
        border = Color{0, 0, 0, 0};
        break;
    default: break;
    }
    const float alpha = disabled ? 0.45f : 1.0f;
    l.bordered_rect(r, radius, fill.with_alpha(fill.a * alpha), 1.0f, border.with_alpha(border.a * alpha));
    if (label == kMore)
        dots(ui, r.cx(), r.cy(), size * 1.3f, ink.with_alpha(alpha));
    else
        text_fit(ui, face, label, r.cx(), baseline(r.cy(), size), size, r.w - 0.8f * R, ink.with_alpha(alpha),
                 Align::center);
    if (disabled) return false;
    return ui.item(id, r, flags, radius);
}

void chevron(Ui& ui, float cx, float cy, float size, int dir, Color color) {
    const float w = size * 0.22f, hgt = size * 0.26f, t = std::max(1.5f, size * 0.085f);
    const float tip = cx + dir * w * 0.5f, back = cx - dir * w * 0.5f;
    ui.list->line(back, cy - hgt, tip, cy, t, color);
    ui.list->line(tip, cy, back, cy + hgt, t, color);
}

void dots(Ui& ui, float cx, float cy, float size, Color color) {
    const float r = size * 0.07f, gap = size * 0.26f;
    for (int i = -1; i <= 1; ++i) ui.list->circle(cx + i * gap, cy, r, color);
}

void hline(Ui& ui, float x, float y, float w, Color color) { ui.list->rounded_rect({x, y, w, 1.0f}, 0.0f, color); }

}  // namespace alembic::app::draw
