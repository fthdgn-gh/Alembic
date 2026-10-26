// SPDX-License-Identifier: GPL-3.0-or-later
#include "alembic/unicode.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace alembic::unicode {

namespace {

struct CasePair {
    char32_t from;
    char32_t to;
};

constexpr CasePair kLowercase[] = {
#include "data/lowercase_table.inc"
};

void append_utf16(std::u16string& out, char32_t cp) {
    if (cp < 0x10000) {
        out.push_back(static_cast<char16_t>(cp));
    } else {
        cp -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
}

} // namespace

Result<std::u16string> utf8_to_utf16(std::string_view in) {
    std::u16string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        auto b0 = static_cast<unsigned char>(in[i]);
        char32_t cp;
        std::size_t len;
        if (b0 < 0x80) { cp = b0; len = 1; }
        else if ((b0 & 0xE0) == 0xC0) { cp = b0 & 0x1F; len = 2; }
        else if ((b0 & 0xF0) == 0xE0) { cp = b0 & 0x0F; len = 3; }
        else if ((b0 & 0xF8) == 0xF0) { cp = b0 & 0x07; len = 4; }
        else return make_error(Errc::invalid_data, "invalid UTF-8 in name");
        if (i + len > in.size()) return make_error(Errc::invalid_data, "truncated UTF-8 in name");
        for (std::size_t k = 1; k < len; ++k) {
            auto b = static_cast<unsigned char>(in[i + k]);
            if ((b & 0xC0) != 0x80) return make_error(Errc::invalid_data, "invalid UTF-8 in name");
            cp = (cp << 6) | (b & 0x3F);
        }
        static constexpr char32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < kMin[len] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            return make_error(Errc::invalid_data, "invalid UTF-8 in name");
        append_utf16(out, cp);
        i += len;
    }
    return out;
}

std::string utf16_to_utf8(std::u16string_view in) {
    std::string out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        char32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

char32_t to_lower_invariant(char32_t cp) {
    if (cp < 0x80) return (cp >= 'A' && cp <= 'Z') ? cp + 32 : cp;
    auto it = std::lower_bound(std::begin(kLowercase), std::end(kLowercase), cp,
                               [](const CasePair& pair, char32_t value) { return pair.from < value; });
    return (it != std::end(kLowercase) && it->from == cp) ? it->to : cp;
}

std::u16string to_lower_invariant(std::u16string_view in) {
    std::u16string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        char32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < in.size() && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (in[i + 1] - 0xDC00);
            ++i;
            append_utf16(out, to_lower_invariant(cp));
        } else {
            // Lone surrogates pass through unchanged, as in .NET.
            out.push_back(static_cast<char16_t>(cp >= 0xD800 && cp <= 0xDFFF ? cp : to_lower_invariant(cp)));
        }
    }
    return out;
}

bool ascii_iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        auto x = static_cast<unsigned char>(a[i]);
        auto y = static_cast<unsigned char>(b[i]);
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return false;
    }
    return true;
}

namespace {

// Primary weight of a byte: its rank in the .NET/ICU order; letters share a
// weight with their other case.
int primary_weight(unsigned char c) {
    static const char kOrder[] = " _-,;:!?.'\"()[]{}@*/\\&#%`^+<=>|~$0123456789";
    if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
    if (c >= 'a' && c <= 'z') return 100 + (c - 'a');
    if (const char* at = c ? std::strchr(kOrder, c) : nullptr) return static_cast<int>(at - kOrder);
    return 200 + c;
}

} // namespace

int dotnet_compare(std::string_view a, std::string_view b) {
    std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        int wa = primary_weight(static_cast<unsigned char>(a[i]));
        int wb = primary_weight(static_cast<unsigned char>(b[i]));
        if (wa != wb) return wa < wb ? -1 : 1;
    }
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (std::size_t i = 0; i < n; ++i) {
        bool ua = a[i] >= 'A' && a[i] <= 'Z', ub = b[i] >= 'A' && b[i] <= 'Z';
        if (ua != ub) return ua ? 1 : -1;
    }
    return 0;
}

} // namespace alembic::unicode
