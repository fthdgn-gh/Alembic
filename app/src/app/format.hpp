// SPDX-License-Identifier: GPL-3.0-or-later
// Sizes, times and paths as people read them (lib/format.ts).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace alembic::app {

std::string bytes(double n);
std::string duration(double seconds);
// A time of day today, else a date; full: date and time.
std::string date(std::int64_t when, bool full = false);
std::string base(std::string_view path);
std::string dir(std::string_view path);
// "01.004.000" -> "1.004.000"
std::string version_text(std::string_view v);
// < 0, 0, > 0 like strcmp, by number ("0.2.10" > "0.2.9"; a leading "v" is ignored).
int compare_versions(std::string_view a, std::string_view b);
bool starts_with(std::string_view s, std::string_view prefix);
std::string fixed(double v, int decimals);

}  // namespace alembic::app
