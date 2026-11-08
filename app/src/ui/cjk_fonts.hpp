// SPDX-License-Identifier: GPL-3.0-or-later
// The faces drawn wherever Inter or DejaVu has no glyph: IBM Plex Sans JP and
// IBM Plex Sans SC, each in Inter's two weights (app/tools/fonts.sh). Both
// have hanzi/kanji, drawn differently, so the language picks which is asked
// first: Chinese with the page in Chinese, Japanese otherwise.
#pragma once

#include "gfx/font.hpp"
#include "ui/fonts.hpp"

#include <string>

namespace alembic::app {

struct CjkFonts {
    hui::gfx::Font ja, ja_semibold, zh, zh_semibold;
    hui::ui::Fonts ja_ref, zh_ref;  // .regular and .semibold
    bool has_ja = false, has_zh = false;
    int order = -1;  // 1: Chinese first, 0: Japanese first, -1: not set yet

    // Sets the fallbacks of the three faces when the language asks for
    // another order; cheap to call every frame.
    void apply(const std::string& language, hui::gfx::Font& regular, hui::gfx::Font& semibold,
               hui::gfx::Font& mono) {
        const int want = language == "zh" ? 1 : 0;
        if (want == order) return;
        order = want;
        regular.clear_fallbacks();
        semibold.clear_fallbacks();
        mono.clear_fallbacks();
        for (int pass = 0; pass < 2; ++pass) {
            const bool chinese = (pass == 0) == (want == 1);
            if (chinese ? !has_zh : !has_ja) continue;
            const hui::ui::Fonts& ref = chinese ? zh_ref : ja_ref;
            regular.add_fallback(chinese ? &zh : &ja, ref.regular.texture);
            mono.add_fallback(chinese ? &zh : &ja, ref.regular.texture);
            semibold.add_fallback(chinese ? &zh_semibold : &ja_semibold, ref.semibold.texture);
        }
    }
};

}  // namespace alembic::app
