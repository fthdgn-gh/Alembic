// SPDX-License-Identifier: GPL-3.0-or-later
// The page's translations in the app: the same i18n.js the engine serves
// (var I18N = <JSON>), keyed by the English text, with the same rules as
// src/engine/web/src/lib/i18n.ts. The app's fonts have Latin, Greek and
// Cyrillic only, so Japanese shows the English texts for now.
#pragma once

#include <initializer_list>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alembic::app {

using Vars = std::initializer_list<std::pair<const char*, std::string>>;

// The languages the page offers: code and name.
const std::vector<std::pair<std::string, std::string>>& languages();

// Loads the tables from i18n.js; false when it is not understood.
bool load_i18n(std::string_view script);
void set_language(const std::string& code);
const std::string& language();
// Whether the app can draw this language (not Japanese yet).
bool language_drawable(const std::string& code);

std::string t(std::string_view text);
std::string t(std::string_view text, Vars vars);
std::string plural(long long n, std::string_view one, std::string_view many);
// An engine message: an exact key, else a pattern of I18N.messages.
std::string t_msg(std::string_view text);
// Rewrites paths inside engine messages before they are translated (another
// engine's "/peer/<id>/x" as "Name: x"), like the page's setPathNamer.
void set_path_namer(std::function<std::string(std::string_view)> namer);
std::string place_label(std::string_view label);
std::string t_method(std::string_view method);
std::string t_status(std::string_view status);
// Number with the language's separators ("3,184" / "3.184").
std::string number(long long n);

}  // namespace alembic::app
