// ps5-homebrew-ui - Baked SDF font: loading, measuring and glyph layout.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "gfx/font.hpp"

#include <algorithm>
#include <cstring>

namespace hui::gfx
{

namespace ff = font_format;

std::uint32_t next_codepoint(std::string_view text, std::size_t *index)
{
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const std::size_t i = *index;
    const unsigned char lead = byte(i);
    int length = 1;
    std::uint32_t value = lead;
    if (lead >= 0xf0 && lead < 0xf8)
    {
        length = 4;
        value = lead & 0x07u;
    }
    else if (lead >= 0xe0)
    {
        length = 3;
        value = lead & 0x0fu;
    }
    else if (lead >= 0xc0)
    {
        length = 2;
        value = lead & 0x1fu;
    }
    else if (lead >= 0x80)
    {
        *index = i + 1;
        return 0xfffd;
    }
    if (i + static_cast<std::size_t>(length) > text.size())
    {
        *index = text.size();
        return 0xfffd;
    }
    for (int k = 1; k < length; ++k)
    {
        const unsigned char continuation = byte(i + static_cast<std::size_t>(k));
        if ((continuation & 0xc0u) != 0x80u)
        {
            *index = i + 1;
            return 0xfffd;
        }
        value = (value << 6) | (continuation & 0x3fu);
    }
    *index = i + static_cast<std::size_t>(length);
    return value;
}

bool Font::load(std::string_view data)
{
    error_.clear();
    if (data.size() < sizeof(ff::Header))
    {
        error_ = "font too small";
        return false;
    }
    std::memcpy(&header_, data.data(), sizeof(header_));
    if (header_.magic != ff::kMagic || header_.version != ff::kVersion ||
        header_.pixel_size <= 0.0f)
    {
        error_ = "not a huifont v1";
        return false;
    }
    const std::size_t glyph_bytes =
        static_cast<std::size_t>(header_.glyph_count) * sizeof(ff::Glyph);
    const std::size_t kern_bytes = static_cast<std::size_t>(header_.kern_count) * sizeof(ff::Kern);
    const std::size_t atlas_bytes = static_cast<std::size_t>(header_.atlas_width) *
                                    static_cast<std::size_t>(header_.atlas_height);
    if (data.size() != sizeof(ff::Header) + glyph_bytes + kern_bytes + atlas_bytes)
    {
        error_ = "font size mismatch";
        return false;
    }
    const char *cursor = data.data() + sizeof(ff::Header);
    // A font may have no glyphs or (monospaced faces) no kerning pairs.
    glyphs_.resize(header_.glyph_count);
    if (glyph_bytes != 0)
        std::memcpy(glyphs_.data(), cursor, glyph_bytes);
    cursor += glyph_bytes;
    kerns_.resize(header_.kern_count);
    if (kern_bytes != 0)
        std::memcpy(kerns_.data(), cursor, kern_bytes);
    cursor += kern_bytes;
    atlas_.assign(reinterpret_cast<const std::uint8_t *>(cursor),
                  reinterpret_cast<const std::uint8_t *>(cursor) + atlas_bytes);
    for (const ff::Glyph &glyph : glyphs_)
    {
        if (glyph.x + glyph.w > header_.atlas_width || glyph.y + glyph.h > header_.atlas_height)
        {
            error_ = "glyph outside atlas";
            return false;
        }
    }
    return true;
}

const ff::Glyph *Font::find(std::uint32_t codepoint) const
{
    const auto it =
        std::lower_bound(glyphs_.begin(), glyphs_.end(), codepoint,
                         [](const ff::Glyph &g, std::uint32_t c) { return g.codepoint < c; });
    return it != glyphs_.end() && it->codepoint == codepoint ? &*it : nullptr;
}

float Font::kern(std::uint32_t first, std::uint32_t second) const
{
    const auto it = std::lower_bound(
        kerns_.begin(), kerns_.end(), std::make_pair(first, second),
        [](const ff::Kern &k, const std::pair<std::uint32_t, std::uint32_t> &key)
        { return k.first != key.first ? k.first < key.first : k.second < key.second; });
    return it != kerns_.end() && it->first == first && it->second == second ? it->amount : 0.0f;
}

std::string Font::fit(std::string_view text, float size, float max_width, float tracking) const
{
    if (measure(text, size, tracking) <= max_width)
        return std::string(text);
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    const std::string_view mark = has_glyph(0x2026) ? kEllipsis : std::string_view("...");
    std::string best;
    for (std::size_t index = 0; index < text.size();)
    {
        const std::size_t start = index;
        next_codepoint(text, &index);
        std::string candidate(text.substr(0, start));
        while (!candidate.empty() && candidate.back() == ' ')
            candidate.pop_back();
        candidate.append(mark);
        if (measure(candidate, size, tracking) > max_width)
            break;
        best = std::move(candidate);
    }
    return best;
}

bool Font::has_glyph(std::uint32_t codepoint) const
{
    if (find(codepoint) != nullptr)
        return true;
    for (const Fallback &fallback : fallbacks_)
    {
        if (fallback.face->find(codepoint) != nullptr)
            return true;
    }
    return false;
}

Font::Resolved Font::resolve(std::uint32_t *codepoint) const
{
    if (const ff::Glyph *glyph = find(*codepoint))
        return {this, glyph, 0};
    for (const Fallback &fallback : fallbacks_)
    {
        if (const ff::Glyph *glyph = fallback.face->find(*codepoint))
            return {fallback.face, glyph, fallback.texture};
    }
    *codepoint = '?';
    return {this, find('?'), 0};
}

float Font::measure(std::string_view text, float size, float tracking) const
{
    float width = 0.0f;
    std::uint32_t previous = 0;
    const Font *previous_face = nullptr;
    int glyphs = 0;
    for (std::size_t index = 0; index < text.size();)
    {
        std::uint32_t codepoint = next_codepoint(text, &index);
        const Resolved resolved = resolve(&codepoint);
        if (resolved.glyph == nullptr)
            continue;
        // Each face has metrics of its own; pairs are kerned within one face.
        const float scale = size / resolved.face->header_.pixel_size;
        if (previous != 0 && previous_face == resolved.face)
            width += resolved.face->kern(previous, codepoint) * scale;
        width += resolved.glyph->advance * scale;
        previous = codepoint;
        previous_face = resolved.face;
        ++glyphs;
    }
    return glyphs > 1 ? width + tracking * static_cast<float>(glyphs - 1) : width;
}

float Font::layout(std::string_view text, float x, float y, float size, Align align,
                   std::vector<GlyphQuad> &quads, float tracking) const
{
    const float width = measure(text, size, tracking);
    float pen = x;
    if (align == Align::center)
        pen -= width * 0.5f;
    else if (align == Align::right)
        pen -= width;
    std::uint32_t previous = 0;
    const Font *previous_face = nullptr;
    for (std::size_t index = 0; index < text.size();)
    {
        std::uint32_t codepoint = next_codepoint(text, &index);
        const Resolved resolved = resolve(&codepoint);
        const ff::Glyph *glyph = resolved.glyph;
        if (glyph == nullptr)
            continue;
        const ff::Header &face = resolved.face->header_;
        const float scale = size / face.pixel_size;
        if (previous != 0 && previous_face == resolved.face)
            pen += resolved.face->kern(previous, codepoint) * scale;
        if (glyph->w > 0 && glyph->h > 0)
        {
            const float inverse_w = 1.0f / static_cast<float>(face.atlas_width);
            const float inverse_h = 1.0f / static_cast<float>(face.atlas_height);
            GlyphQuad quad;
            quad.x0 = pen + glyph->offset_x * scale;
            quad.y0 = y + glyph->offset_y * scale;
            quad.x1 = quad.x0 + static_cast<float>(glyph->w) * scale;
            quad.y1 = quad.y0 + static_cast<float>(glyph->h) * scale;
            quad.u0 = static_cast<float>(glyph->x) * inverse_w;
            quad.v0 = static_cast<float>(glyph->y) * inverse_h;
            quad.u1 = static_cast<float>(glyph->x + glyph->w) * inverse_w;
            quad.v1 = static_cast<float>(glyph->y + glyph->h) * inverse_h;
            quad.texture = resolved.texture;
            quad.range = resolved.face->sdf_range(size);
            quads.push_back(quad);
        }
        pen += glyph->advance * scale + tracking;
        previous = codepoint;
        previous_face = resolved.face;
    }
    return width;
}

namespace
{

// Alembic: what may not start a line (closing punctuation, small kana, the
// prolonged sound mark) and what may not end one (opening brackets).
bool no_line_start(std::uint32_t c)
{
    switch (c)
    {
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E: case 0x30FB: case 0xFF1A: case 0xFF1B:
    case 0xFF01: case 0xFF1F: case 0x300D: case 0x300F: case 0xFF09: case 0x3011: case 0x3015:
    case 0x3009: case 0x300B: case 0x30FC: case 0x301C: case 0x2026: case 0x22EF: case 0x3005:
    case 0x309D: case 0x309E: case 0x30FD: case 0x30FE:
        return true;
    default:
        break;
    }
    // Small hiragana and katakana.
    switch (c)
    {
    case 0x3041: case 0x3043: case 0x3045: case 0x3047: case 0x3049: case 0x3063: case 0x3083:
    case 0x3085: case 0x3087: case 0x308E: case 0x3095: case 0x3096: case 0x30A1: case 0x30A3:
    case 0x30A5: case 0x30A7: case 0x30A9: case 0x30C3: case 0x30E3: case 0x30E5: case 0x30E7:
    case 0x30EE: case 0x30F5: case 0x30F6:
        return true;
    default:
        return c >= 0x31F0 && c <= 0x31FF;
    }
}

// Characters of scripts that write no spaces between words: a line may
// break before or after any of them.
bool is_wide(std::uint32_t c)
{
    return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF) ||
           (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFFEF) || (c >= 0x20000 && c <= 0x3FFFF);
}

bool no_line_end(std::uint32_t c)
{
    return c == 0x300C || c == 0x300E || c == 0xFF08 || c == 0x3010 || c == 0x3014 || c == 0x3008 ||
           c == 0x300A;
}

} // namespace

// Alembic: breaks between words, and also between any two characters of a
// script that writes no spaces (Chinese, Japanese), keeping the Japanese
// line-breaking rules: closing marks stay with what comes before them and
// opening brackets with what follows.
std::vector<std::string> Font::wrap(std::string_view text, float size, float max_width) const
{
    struct Piece
    {
        std::string text;
        bool space_before = false;
    };
    std::vector<std::string> lines;
    std::size_t index = 0;
    while (index <= text.size())
    {
        const std::size_t newline = text.find('\n', index);
        const std::string_view paragraph = text.substr(
            index, newline == std::string_view::npos ? std::string_view::npos : newline - index);

        // The pieces a line may break between: words, and single wide characters.
        std::vector<Piece> pieces;
        bool space = false, in_word = false, glue_next = false;
        for (std::size_t at = 0; at < paragraph.size();)
        {
            const std::size_t from = at;
            const std::uint32_t c = next_codepoint(paragraph, &at);
            const std::string_view bytes = paragraph.substr(from, at - from);
            if (c == ' ')
            {
                space = true;
                in_word = false;
                continue;
            }
            const bool wide = is_wide(c);
            const bool join = !pieces.empty() && !space &&
                              (glue_next || no_line_start(c) || (in_word && !wide));
            if (join)
                pieces.back().text += bytes;
            else
                pieces.push_back({std::string(bytes), space && !pieces.empty()});
            glue_next = no_line_end(c);
            in_word = !wide;
            space = false;
        }

        std::string line;
        for (const Piece &piece : pieces)
        {
            std::string candidate = line.empty() ? piece.text
                                                 : line + (piece.space_before ? " " : "") + piece.text;
            if (measure(candidate, size) <= max_width)
            {
                line = std::move(candidate);
                continue;
            }
            if (!line.empty())
                lines.push_back(line);
            // The piece starts a line of its own. One wider than the line
            // gives up as many code points as fit, line after line.
            const std::string_view word = piece.text;
            std::size_t start = 0;
            while (measure(word.substr(start), size) > max_width)
            {
                std::size_t end = start;
                next_codepoint(word, &end);
                for (std::size_t next = end; next < word.size(); end = next)
                {
                    next_codepoint(word, &next);
                    if (measure(word.substr(start, next - start), size) > max_width)
                        break;
                }
                if (end >= word.size())
                    break;
                lines.emplace_back(word.substr(start, end - start));
                start = end;
            }
            line.assign(word.substr(start));
        }
        lines.push_back(line);
        if (newline == std::string_view::npos)
            break;
        index = newline + 1;
    }
    return lines;
}

} // namespace hui::gfx
