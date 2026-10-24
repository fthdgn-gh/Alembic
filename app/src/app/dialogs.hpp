// SPDX-License-Identifier: GPL-3.0-or-later
// Dialogs (components/Overlay.tsx, Dialogs.tsx): a sheet over the dimmed
// page with a title, a scrolling body and a row of buttons; a question; a
// list of choices; text typed on the kit's on-screen keyboard.
#pragma once

#include "app/app.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace alembic::app {

// The sheet of a dialog, laid out from what it held last frame.
class Sheet {
public:
    enum class Size { normal, wide, narrow };
    // Dims the page, draws the sheet and its title, and opens the body's
    // scroll area. Draw the body from content_top(), at x() with width w().
    void begin(Ui& ui, const std::string& title, const std::string& sub, Size size, bool foot);
    float x() const { return body_x_; }
    float w() const { return body_w_; }
    float content_top() const { return content_top_; }
    // Closes the body; bottom is where its content ended (screen coordinates).
    void end_body(Ui& ui, float bottom);
    // The foot's buttons, placed from the right; true when pressed.
    bool button(Ui& ui, Id id, const std::string& label, draw::Button kind, bool disabled = false,
                unsigned flags = 0);
    // The foot's message, left of the buttons.
    void message(Ui& ui, const std::string& text, bool error);
    void end(Ui& ui);
    const Rect& rect() const { return rect_; }

private:
    Rect rect_;
    float body_h_ = 160.0f;  // content height last frame
    float head_h_ = 0.0f;
    float body_x_ = 0.0f, body_w_ = 0.0f, content_top_ = 0.0f;
    float foot_y_ = 0.0f, foot_right_ = 0.0f, foot_left_ = 0.0f;
    bool foot_ = false;
    bool first_ = true;
    float age_ = 0.0f;
    Id scroll_;
};

// Draws part of a dialog's body at (x, y), width w; returns its height.
using Body = std::function<float(Ui& ui, App& app, float x, float y, float w)>;

// A question with OK and Cancel (Dialogs.tsx ask()).
void ask(App& app, const std::string& title, Body body, const std::string& ok, bool danger,
         std::function<void()> yes, std::function<void()> no = nullptr);
// A text body (paragraphs separated by "\n\n"; a line starting with "@" is mono).
Body text_body(const std::string& text);

// One line of text to enter, on the on-screen keyboard.
void ask_text(App& app, const std::string& title, const std::string& initial, const std::string& ok,
              std::function<void(const std::string&)> done, bool numeric = false, bool secret = false);

// A list of choices (Dialogs.tsx choose()); headings are not choices.
struct Choice {
    std::string label, sub;
    bool danger = false, selected = false, heading = false;
    std::function<void()> act;
};
void choose(App& app, const std::string& title, std::vector<Choice> items, const std::string& sub = "");

// A labelled text field in a form: ✕ opens the keyboard.
bool field_row(Ui& ui, App& app, Id id, float x, float* y, float w, const std::string& label, std::string* value,
               const std::string& placeholder, bool numeric = false, bool secret = false);
// The page's form rows (components/Rows.tsx). Each returns its height.
float switch_row(Ui& ui, Id id, float x, float y, float w, const std::string& label, const std::string& hint,
                 bool* on, bool* changed = nullptr);
float choice_row(Ui& ui, Id id, float x, float y, float w, const std::string& label, const std::string& hint,
                 const std::vector<std::string>& labels, int* index, bool* changed = nullptr);
// A row that opens something (› at the end).
float link_row(Ui& ui, Id id, float x, float y, float w, const std::string& label, const std::string& hint,
               const std::string& value, bool* pressed);
// Label and value pairs (.facts); classes: "" | "err" | "mono".
struct Fact {
    std::string label, value, cls;
};
float facts(Ui& ui, float x, float y, float w, const std::vector<Fact>& rows);
// Small capitals heading (.sect / .lbl); returns its height.
float section(Ui& ui, const std::string& text, float x, float y, float top_margin = 1.0f);

}  // namespace alembic::app
