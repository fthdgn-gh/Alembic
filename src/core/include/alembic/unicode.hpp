// SPDX-License-Identifier: GPL-3.0-or-later
// The few Unicode operations the formats need. PS5PKGTool works on .NET
// UTF-16 strings, so names are compared and hashed as UTF-16 to match it.
#pragma once

#include "alembic/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace alembic::unicode {

// Strict UTF-8 → UTF-16. Rejects overlong forms, surrogates and bad bytes.
Result<std::u16string> utf8_to_utf16(std::string_view utf8);
std::string utf16_to_utf8(std::u16string_view utf16);

// Matches .NET string.ToLowerInvariant() (simple per-code-point mapping).
char32_t to_lower_invariant(char32_t code_point);
std::u16string to_lower_invariant(std::u16string_view text);

// ASCII-only case-insensitive comparison (.NET OrdinalIgnoreCase on ASCII).
bool ascii_iequals(std::string_view a, std::string_view b);

// .NET's default string order (String.CompareTo, culture-aware, ICU root
// collation) for ASCII: whitespace, then punctuation and symbols in the order
// _ - , ; : ! ? . ' " ( ) [ ] { } @ * / \ & # % ` ^ + < = > | ~ $, then
// digits, then letters ignoring case; ties go to the first lowercase letter.
// Checked against .NET 10 on macOS. Other bytes sort after ASCII, by value.
// Returns <0, 0 or >0.
int dotnet_compare(std::string_view a, std::string_view b);

} // namespace alembic::unicode
