// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/ui.hpp"

#include "ui/style.hpp"

#include <algorithm>
#include <cmath>

namespace alembic::app {

namespace {

float approach(float value, float target, float rate, float dt) {
    return target + (value - target) * std::exp(-rate * dt);
}

Rect approach(const Rect& a, const Rect& b, float rate, float dt) {
    return {approach(a.x, b.x, rate, dt), approach(a.y, b.y, rate, dt), approach(a.w, b.w, rate, dt),
            approach(a.h, b.h, rate, dt)};
}

}  // namespace

void Ui::begin_frame(const InputFrame& input, float frame_dt) {
    input_ = input;
    dt = frame_dt;
    time += frame_dt;
    ++frame_;
}

void Ui::end_frame() {
    for (auto it = layers_.begin(); it != layers_.end();) {
        if (it->second.seen_frame != frame_)
            it = layers_.erase(it);
        else
            ++it;
    }
}

void Ui::begin_layer(int key, bool active) {
    const bool fresh = layers_.find(key) == layers_.end();
    layer_ = &layers_[key];
    layer_->prev.swap(layer_->items);
    layer_->items.clear();
    layer_->seen_frame = frame_;
    layer_->moved_to = Id();
    if (fresh) layer_->want_reset = true;
    layer_active_ = active;
    step_dir_ = 0;
    step_id_ = Id();
    // ← and → on a stepper change its value: the focus stays.
    if (active && (input_.nav == Direction::left || input_.nav == Direction::right)) {
        const Rec* cur = find(layer_->prev, layer_->focus);
        if (cur && (cur->flags & kStepper)) {
            step_dir_ = input_.nav == Direction::left ? -1 : 1;
            step_id_ = cur->id;
        }
    }
}

const Ui::Rec* Ui::find(const std::vector<Rec>& items, Id id) const {
    for (const Rec& r : items)
        if (r.id == id) return &r;
    return nullptr;
}

bool Ui::item(Id id, const Rect& r, unsigned flags, float radius) {
    Rec rec;
    rec.id = id;
    rec.rect = r;
    rec.flags = flags;
    rec.radius = radius;
    if (!zones_.empty()) {
        rec.zone = zones_.back().id;
        rec.column = zones_.back().column;
        rec.remember = zones_.back().remember;
    }
    if (!scrolls_.empty()) {
        rec.scroll = scrolls_.back().id;
        rec.clip = scrolls_.back().view;
        rec.has_clip = true;
    }
    layer_->items.push_back(rec);
    return layer_active_ && layer_->focus == id && input_.is_pressed(Action::confirm);
}

bool Ui::focused(Id id) const { return layer_ != nullptr && layer_->focus == id; }
Id Ui::focus() const { return layer_ ? layer_->focus : Id(); }
int Ui::stepped(Id id) const { return step_id_ == id ? step_dir_ : 0; }
bool Ui::seen(Id id) const { return layer_ != nullptr && find(layer_->prev, id) != nullptr; }

void Ui::set_focus(Id id) {
    if (!layer_) return;
    if (layer_->focus != id) layer_->moved_to = id;
    layer_->focus = id;
    layer_->want_reset = false;
}

void Ui::reset_focus() {
    if (layer_) layer_->want_reset = true;
}

void Ui::begin_zone(Id zone, bool column, bool remember) { zones_.push_back({zone, column, remember}); }
void Ui::end_zone() {
    if (!zones_.empty()) zones_.pop_back();
}

Id Ui::zone_last(Id zone) const {
    if (!layer_) return Id();
    auto it = layer_->zone_last.find(zone);
    return it == layer_->zone_last.end() ? Id() : it->second;
}

float Ui::begin_scroll(Id id, const Rect& view) {
    Scroll& s = layer_->scrolls[id];
    s.view = view;
    s.seen_frame = frame_;
    scrolls_.push_back({id, view});
    list->push_clip(view);
    return s.offset;
}

void Ui::end_scroll(float content_height) {
    const Id id = scrolls_.back().id;
    scrolls_.pop_back();
    list->pop_clip();
    Scroll& s = layer_->scrolls[id];
    s.content = content_height;
    const float max = std::max(0.0f, content_height - s.view.h);
    // The focused item stays in view (12 px from the edges, as the page).
    for (const Rec& rec : layer_->items) {
        if (rec.id != layer_->focus || rec.scroll != id) continue;
        const float pad = 12.0f * 1.2f;
        const float top = rec.rect.y + s.offset - s.view.y;
        const float lo = top + rec.rect.h - (s.view.h - pad);
        const float hi = top - pad;
        if (s.target < lo) s.target = lo;
        if (s.target > hi) s.target = hi;
        break;
    }
    s.target = std::clamp(s.target, 0.0f, max);
    if (s.snap) {
        s.offset = s.target;
        s.snap = false;
    } else {
        s.offset = approach(s.offset, s.target, 22.0f, dt);
        if (std::fabs(s.offset - s.target) < 0.5f) s.offset = s.target;
    }
}

void Ui::scroll_to_top(Id id) {
    if (!layer_) return;
    Scroll& s = layer_->scrolls[id];
    s.target = 0.0f;
    s.offset = 0.0f;
}

bool Ui::visible(const Rect& r) const {
    if (scrolls_.empty()) return true;
    const Rect& v = scrolls_.back().view;
    return r.y + r.h >= v.y - 40.0f && r.y <= v.y + v.h + 40.0f;
}

bool Ui::pressed(Action a) const { return layer_active() && input_.is_pressed(a); }

// The nearest item in the pressed direction (focus.ts move()).
Id Ui::step(const std::vector<Rec>& items, const Rec& cur, Direction dir) const {
    const Rect& r = cur.rect;
    const float cx = r.cx(), cy = r.cy();
    const bool vertical = dir == Direction::up || dir == Direction::down;
    const Rec* best = nullptr;
    float best_score = 1e30f;
    for (const Rec& c : items) {
        if (c.id == cur.id) continue;
        const Rect& q = c.rect;
        const float qx = q.cx(), qy = q.cy();
        float main = 0.0f, ortho = 0.0f;
        if (dir == Direction::down) {
            if (qy <= cy + 1.0f || q.y < r.y + 2.0f) continue;
            main = std::max(0.0f, q.y - (r.y + r.h));
            ortho = std::max({0.0f, q.x - (r.x + r.w), r.x - (q.x + q.w)});
        } else if (dir == Direction::up) {
            if (qy >= cy - 1.0f || q.y + q.h > r.y + r.h - 2.0f) continue;
            main = std::max(0.0f, r.y - (q.y + q.h));
            ortho = std::max({0.0f, q.x - (r.x + r.w), r.x - (q.x + q.w)});
        } else if (dir == Direction::right) {
            if (q.x < r.x + r.w - 8.0f) continue;
            main = std::max(0.0f, q.x - (r.x + r.w));
            ortho = std::max({0.0f, q.y - (r.y + r.h), r.y - (q.y + q.h)});
        } else {
            if (q.x + q.w > r.x + 8.0f) continue;
            main = std::max(0.0f, r.x - (q.x + q.w));
            ortho = std::max({0.0f, q.y - (r.y + r.h), r.y - (q.y + q.h)});
        }
        // Up and down stay in a column: the end of a list is the end.
        if (vertical && ortho > 0.0f && cur.column) continue;
        const float align = vertical ? std::fabs(qx - cx) : std::fabs(qy - cy);
        const float score = main + ortho * 4.0f + align * 0.05f;
        if (score < best_score) {
            best_score = score;
            best = &c;
        }
    }
    return best ? best->id : Id();
}

void Ui::end_layer() {
    Layer& L = *layer_;
    const Rec* cur = find(L.items, L.focus);
    // No focus yet, or its item is gone: the autofocus item, else the item
    // nearest to where the focus was (in its zone), else the first.
    if (L.want_reset || !cur) {
        const Rec* pick = nullptr;
        if (L.want_reset) {
            for (const Rec& r : L.items)
                if (r.flags & kAutofocus) {
                    pick = &r;
                    break;
                }
        }
        if (!pick && !L.want_reset && L.have_last_rect) {
            float best = 1e30f;
            for (const Rec& r : L.items) {
                if (r.zone != L.last_focus_zone) continue;
                const float d = std::fabs(r.rect.cy() - L.last_focus_rect.cy()) +
                                std::fabs(r.rect.cx() - L.last_focus_rect.cx()) * 0.25f;
                if (d < best) {
                    best = d;
                    pick = &r;
                }
            }
        }
        if (!pick && !L.items.empty()) {
            for (const Rec& r : L.items)
                if (r.flags & kAutofocus) {
                    pick = &r;
                    break;
                }
            if (!pick) pick = &L.items.front();
        }
        if (pick) {
            if (L.want_reset) L.ring_on = false;  // a new screen: no glide from elsewhere
            L.focus = pick->id;
            L.want_reset = false;
        }
        cur = pick;
    }
    // The D-pad, and L2 / R2 (eight steps inside the same zone).
    if (layer_active_ && cur && step_dir_ == 0) {
        Id target;
        if (input_.nav != Direction::none) {
            target = step(L.items, *cur, input_.nav);
            if (target) {
                const Rec* t = find(L.items, target);
                if (t && t->zone != cur->zone && t->remember) {
                    auto it = L.zone_last.find(t->zone);
                    if (it != L.zone_last.end() && find(L.items, it->second)) target = it->second;
                }
            }
        } else if (input_.is_pressed(Action::jump_prev) || input_.is_pressed(Action::jump_next)) {
            const Direction d = input_.is_pressed(Action::jump_prev) ? Direction::up : Direction::down;
            const Rec* at = cur;
            for (int i = 0; i < 8; ++i) {
                const Id next = step(L.items, *at, d);
                const Rec* n = next ? find(L.items, next) : nullptr;
                if (!n || n->zone != cur->zone) break;
                at = n;
            }
            if (at != cur) target = at->id;
        }
        if (target && target != L.focus) {
            L.focus = target;
            L.moved_to = target;
            cur = find(L.items, target);
            play(hui::audio::Cue::focus);
        }
    }
    if (cur) {
        if (cur->zone) L.zone_last[cur->zone] = cur->id;
        L.last_focus_rect = cur->rect;
        L.last_focus_zone = cur->zone;
        L.have_last_rect = true;
        // The ring glides to the focused item.
        if (!(cur->flags & kNoRing) && layer_active_) {
            const Rect target = cur->rect;
            if (!L.ring_on) {
                L.ring = target;
                L.ring_on = true;
            } else {
                L.ring = approach(L.ring, target, 30.0f, dt);
            }
            L.ring_radius = cur->radius;
            const float gap = 0.14f * style::R, width = 0.16f * style::R;
            Rect ring = L.ring.inset(-(gap + width));
            if (cur->has_clip) list->push_clip(cur->clip.inset(-(gap + width + 2.0f)));
            const float rad = (cur->flags & kPill) ? ring.h * 0.5f : L.ring_radius + gap + width;
            list->bordered_rect(ring, rad, Color{0, 0, 0, 0}, width, style::focus);
            if (cur->has_clip) list->pop_clip();
        } else {
            L.ring_on = false;
        }
    }
    zones_.clear();
    scrolls_.clear();
    layer_ = nullptr;
}

}  // namespace alembic::app
