// SPDX-License-Identifier: GPL-3.0-or-later
// The page's pieces, drawn: text in its three faces, the icons of
// Icons.tsx, format and state pills, meters, progress bars, switches and
// buttons (styles/*.css).
#pragma once

#include "ui/style.hpp"
#include "ui/ui.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::app::draw {

using hui::gfx::Align;

enum class Face { regular, semibold, mono };

const hui::ui::FontRef& font(Ui& ui, Face face);
float measure(Ui& ui, Face face, std::string_view text, float size);
// One line with its baseline at y. Returns the width.
float text(Ui& ui, Face face, std::string_view text, float x, float baseline, float size, Color color,
           Align align = Align::left, float tracking = 0.0f);
// The same, cut with an ellipsis to max_width.
float text_fit(Ui& ui, Face face, std::string_view text, float x, float baseline, float size, float max_width,
               Color color, Align align = Align::left);
// The baseline that centres a line of this size on cy.
inline float baseline(float cy, float size) { return cy + 0.36f * size; }
// Word-wrapped text from its first line's top; returns the height used.
float paragraph(Ui& ui, Face face, std::string_view text, float x, float top, float width, float size,
                float line_height, Color color, int max_lines = 99);
float paragraph_height(Ui& ui, Face face, std::string_view text, float width, float size, float line_height,
                       int max_lines = 99);
// Small capitals with tracking (.sect, .lbl): "DRIVES".
float caps(Ui& ui, std::string_view text, float x, float baseline, float size, Color color);

enum class Icon { folder, drive, usb, server, place, clock, file };
void icon(Ui& ui, Icon icon, const Rect& box, Color color);

// The square in front of a row (.ico): a game's icon when `texture` is set,
// else a folder or file symbol, else the format label.
void ico(Ui& ui, const Rect& box, std::string_view kind, std::string_view label, std::uint32_t texture);
// The colour pair of a format (.fmt / .ico classes): background and text.
void kind_colors(std::string_view kind, Color* fill, Color* ink);
// "EXFAT" pill; returns its width.
float fmt_pill(Ui& ui, std::string_view kind, std::string_view label, float x, float cy);
// "RUNNING" pill ending at right; returns its width.
float state_pill(Ui& ui, std::string_view state, std::string_view label, float right, float cy);
// "ffpfsc" outlined tag ending at right; returns its width.
float tag(Ui& ui, std::string_view label, float right, float cy);
void meter(Ui& ui, const Rect& r, float used, bool low);
void progress(Ui& ui, const Rect& r, float fraction, bool indeterminate, float opacity = 1.0f);
void switch_(Ui& ui, const Rect& r, bool on);

enum class Button { normal, primary, danger, danger_solid, ghost };
float button_width(Ui& ui, std::string_view label, bool small);
// A focusable button; true when pressed. A disabled one is drawn dimmed and
// takes no focus.
bool button(Ui& ui, Id id, const Rect& r, std::string_view label, Button kind, bool small, bool disabled = false,
            unsigned flags = 0);

// ‹ › and ⋯, drawn (the baked fonts lack them). dir: -1 left, +1 right.
void chevron(Ui& ui, float cx, float cy, float size, int dir, Color color);
void dots(Ui& ui, float cx, float cy, float size, Color color);
constexpr const char* kMore = "\xE2\x8B\xAF";  // a button labelled kMore shows dots

// The 1 px separator lines of the page.
void hline(Ui& ui, float x, float y, float w, Color color);

}  // namespace alembic::app::draw
