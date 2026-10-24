// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/format.hpp"

#include "app/i18n.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace alembic::app {

std::string fixed(double v, int decimals) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, v);
    return buf;
}

std::string bytes(double n) {
    static const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int i = 0;
    while (n >= 1000.0 && i < 4) {
        n /= 1000.0;
        ++i;
    }
    if (i == 0) return std::to_string(static_cast<long long>(n)) + " B";
    return fixed(n, n < 10 ? 2 : n < 100 ? 1 : 0) + " " + units[i];
}

std::string duration(double seconds) {
    const long long s = std::max(0LL, static_cast<long long>(std::llround(seconds)));
    const long long h = s / 3600, m = s % 3600 / 60, r = s % 60;
    if (h) return t("{h} h {m} min", {{"h", std::to_string(h)}, {"m", std::to_string(m)}});
    if (m) {
        if (r) return t("{m} min {s} s", {{"m", std::to_string(m)}, {"s", std::to_string(r)}});
        return t("{m} min", {{"m", std::to_string(m)}});
    }
    return t("{s} s", {{"s", std::to_string(r)}});
}

namespace {

// The civil date of a day count since 1970-01-01 (Howard Hinnant's algorithm).
void civil(std::int64_t days, int* y, int* m, int* d) {
    days += 719468;
    const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    *m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    *y = static_cast<int>(yoe + era * 400 + (*m <= 2 ? 1 : 0));
}

}  // namespace

// In UTC: a native title has no localtime_r (the page uses the browser's zone).
std::string date(std::int64_t when, bool full) {
    if (!when) return "";
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::int64_t day = when >= 0 ? when / 86400 : (when - 86399) / 86400;
    const std::int64_t secs = when - day * 86400;
    int y, m, d;
    civil(day, &y, &m, &d);
    char date_text[32], time_text[16];
    std::snprintf(time_text, sizeof(time_text), "%02d:%02d", static_cast<int>(secs / 3600), static_cast<int>(secs / 60 % 60));
    std::snprintf(date_text, sizeof(date_text), "%04d-%02d-%02d", y, m, d);
    const bool today = day == now / 86400;
    if (today && !full) return time_text;
    return full ? std::string(date_text) + " " + time_text : std::string(date_text);
}

std::string base(std::string_view p) {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const std::size_t slash = p.rfind('/');
    return std::string(slash == std::string_view::npos ? p : p.substr(slash + 1));
}

std::string dir(std::string_view p) {
    while (p.size() > 1 && p.back() == '/') p.remove_suffix(1);
    const std::size_t slash = p.rfind('/');
    if (slash == std::string_view::npos || slash == 0) return "/";
    return std::string(p.substr(0, slash));
}

std::string version_text(std::string_view v) {
    std::size_t i = 0;
    while (i + 1 < v.size() && v[i] == '0' && v[i + 1] >= '0' && v[i + 1] <= '9') ++i;
    return std::string(v.substr(i));
}

bool starts_with(std::string_view s, std::string_view prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

}  // namespace alembic::app
