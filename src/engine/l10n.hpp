// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's own texts in the user's language (console notifications), from
// the same tables as the page (web/i18n.js, embedded in the engine).
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace alembic::engine::l10n {

using Vars = std::vector<std::pair<std::string, std::string>>;

// Reads the tables from the embedded i18n.js ("var I18N = {...};").
void load(const char* data, std::size_t size);
// "en", "tr", ...: a code with a table, or "en".
void set_language(const std::string& code);
std::string language();
bool has_language(const std::string& code);

// As the page's t(): English in, translated out; {name} filled from `vars`.
std::string t(const std::string& english, const Vars& vars = {});
// As the page's tMsg(): an engine message by exact key or pattern; the matched
// parts are translated too.
std::string message(const std::string& text);

// {name: text} when `text` has the shape of `pattern`; false otherwise.
bool match(const std::string& pattern, const std::string& text, Vars& out);

} // namespace alembic::engine::l10n
