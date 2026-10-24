// SPDX-License-Identifier: GPL-3.0-or-later
// The page's look in the app's 1920 x 1080 canvas: the colours of
// src/engine/web/src/styles/base.css, and its sizes. The page is laid out for
// a 1600-pixel-wide browser (the PS5's), so one CSS rem (16 px) is 19.2 here
// and every size in the stylesheet carries over as `rem * R`.
#pragma once

#include "gfx/draw_list.hpp"

#include <cstdint>

namespace alembic::app::style {

using hui::gfx::Color;

constexpr float R = 19.2f;  // one rem
constexpr float W = 1920.0f;
constexpr float H = 1080.0f;

inline Color rgb(std::uint32_t hex, float alpha = 1.0f) { return Color::rgb(hex, alpha); }

// --bg ... --focus
inline const Color bg = rgb(0x0b0e14);
inline const Color bg2 = rgb(0x111621);
inline const Color panel = rgb(0x151b27);
inline const Color panel2 = rgb(0x1b2231);
inline const Color raise = rgb(0x222b3d);
inline const Color line = rgb(0x263044);
inline const Color line2 = rgb(0x34405a);
inline const Color text = rgb(0xe9edf5);
inline const Color muted = rgb(0x8e98ad);
inline const Color dim = rgb(0x5d677c);
inline const Color accent = rgb(0x6f93ff);
inline const Color accent2 = rgb(0x9a7bff);
inline const Color ok = rgb(0x3fc282);
inline const Color bad = rgb(0xf06a62);
inline const Color warn = rgb(0xe6b445);
inline const Color focus = rgb(0x8fb0ff);
inline const Color teal = rgb(0x45d6a8);
// Form rows and option tiles (#121825), inputs (#0d121c), the side panel (#0f141e).
inline const Color field = rgb(0x121825);
inline const Color input = rgb(0x0d121c);
inline const Color side = rgb(0x0f141e);

// The controller glyph colours of the page (Icons.tsx).
inline const Color glyph_x = rgb(0x8aa7ff);
inline const Color glyph_o = rgb(0xff6f8f);
inline const Color glyph_s = rgb(0xe993dc);

// Common sizes.
constexpr float page_pad = 2.5f * R;  // the view's side padding
constexpr float radius = 0.75f * R;   // --radius

}  // namespace alembic::app::style
