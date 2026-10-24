// SPDX-License-Identifier: GPL-3.0-or-later
// The app's immediate-mode UI over the kit's draw list. Every frame a screen
// lays itself out and draws in one pass, and declares the things that can
// take the focus with item(). The D-pad moves the focus to the nearest item in
// its direction, as the web page does (src/engine/web/src/input/focus.ts):
// columns keep up and down to themselves and, entered from elsewhere, give
// the focus back to the item that had it last. Each dialog is a layer of its
// own; only the top layer takes input.
#pragma once

#include "core/input.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace alembic::app {

using hui::Action;
using hui::Direction;
using hui::InputFrame;
using hui::gfx::Color;
using hui::gfx::DrawList;
using hui::gfx::Rect;

// A stable name for an item, a zone or a scroll area (FNV-1a of a string).
struct Id {
    std::uint64_t v = 0;
    Id() = default;
    explicit Id(std::uint64_t value) : v(value) {}
    Id(std::string_view s) { v = mix(14695981039346656037ull, s); }
    Id(const char* s) : Id(std::string_view(s)) {}
    Id(const std::string& s) : Id(std::string_view(s)) {}
    Id operator/(std::string_view s) const { return Id(mix(v ^ 0x9e3779b97f4a7c15ull, s)); }
    Id operator/(std::int64_t n) const { return *this / std::string_view(std::to_string(n)); }
    bool operator==(const Id& o) const { return v == o.v; }
    bool operator!=(const Id& o) const { return v != o.v; }
    bool operator<(const Id& o) const { return v < o.v; }
    explicit operator bool() const { return v != 0; }

private:
    static std::uint64_t mix(std::uint64_t h, std::string_view s) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 1099511628211ull;
        }
        return h ? h : 1;
    }
};

// item() flags.
enum ItemFlag : unsigned {
    kStepper = 1u << 0,    // ← and → change its value instead of moving (ChoiceRow)
    kAutofocus = 1u << 1,  // takes the focus when the layer has none yet
    kNoRing = 1u << 2,     // draws its own focus look (menu entries)
    kPill = 1u << 3,       // the ring is a pill
};

class Ui {
public:
    DrawList* list = nullptr;
    const hui::ui::Fonts* fonts = nullptr;
    hui::ui::Feedback* feedback = nullptr;
    float time = 0.0f;  // seconds since start
    float dt = 0.0f;

    void begin_frame(const InputFrame& input, float dt);
    void end_frame();

    // Layers: 0 is the page, every dialog its own key (unique for its life).
    void begin_layer(int key, bool active);
    void end_layer();
    bool layer_active() const { return layer_ != nullptr && layer_active_; }

    // A focusable item at r (screen coordinates, scrolled). True on ✕ while
    // it has the focus. radius shapes the focus ring.
    bool item(Id id, const Rect& r, unsigned flags = 0, float radius = 0.7f * 19.2f);
    bool focused(Id id) const;
    Id focus() const;
    // -1 or +1 when ← or → was pressed on this stepper item this frame.
    int stepped(Id id) const;
    // Moves the focus (now, and drawn from the next frame on).
    void set_focus(Id id);
    // Gives the focus to the layer's first autofocus item (or first item) at
    // the end of this frame.
    void reset_focus();
    // Whether this layer saw `id` last frame (for a focus that must wait for
    // an item to appear).
    bool seen(Id id) const;

    // Zones: items between begin_zone and end_zone belong to it. In a column,
    // up and down stay inside; `remember` gives the focus back to the item
    // that had it last when the zone is entered from another.
    void begin_zone(Id zone, bool column, bool remember);
    void end_zone();
    // The zone's last focused item (to land there when a section opens).
    Id zone_last(Id zone) const;

    // A vertical scroll area: items inside move with it, and the focused one
    // is kept in view. Returns the offset to subtract from content positions.
    float begin_scroll(Id id, const Rect& view);
    void end_scroll(float content_height);
    void scroll_to_top(Id id);
    // Whether r (screen coordinates) is inside the open scroll area's view
    // (worth drawing).
    bool visible(const Rect& r) const;

    // Buttons pressed this frame, for the active layer only.
    bool pressed(Action a) const;
    Direction nav() const { return layer_active() ? input_.nav : Direction::none; }
    const InputFrame& input() const { return input_; }
    void play(hui::audio::Cue cue) {
        if (feedback) feedback->play(cue);
    }

private:
    struct Rec {
        Id id;
        Rect rect;
        Id zone;
        bool column = false;
        bool remember = false;
        Id scroll;
        Rect clip;
        bool has_clip = false;
        unsigned flags = 0;
        float radius = 0.0f;
    };
    struct Scroll {
        float offset = 0.0f;
        float target = 0.0f;
        float content = 0.0f;
        Rect view;
        bool snap = true;
        int seen_frame = 0;
    };
    struct Layer {
        Id focus;
        Id moved_to;  // the focus moved here this frame (cue and scroll)
        bool want_reset = true;
        std::vector<Rec> items, prev;
        std::map<Id, Id> zone_last;
        std::map<Id, Scroll> scrolls;
        Rect ring;
        bool ring_on = false;
        float ring_radius = 0.0f;
        bool ring_pill = false;
        int seen_frame = 0;
        Rect last_focus_rect;
        bool have_last_rect = false;
        Id last_focus_zone;
    };
    struct ZoneFrame {
        Id id;
        bool column;
        bool remember;
    };
    struct ScrollFrame {
        Id id;
        Rect view;
    };

    const Rec* find(const std::vector<Rec>& list, Id id) const;
    Id step(const std::vector<Rec>& items, const Rec& cur, Direction dir) const;

    InputFrame input_{};
    int frame_ = 0;
    std::map<int, Layer> layers_;
    Layer* layer_ = nullptr;
    bool layer_active_ = false;
    std::vector<ZoneFrame> zones_;
    std::vector<ScrollFrame> scrolls_;
    int step_dir_ = 0;  // ← / → on a stepper this frame
    Id step_id_;
};

}  // namespace alembic::app
